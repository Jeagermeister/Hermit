# Requirements

Every requirement here traces to a failure observed on this hardware, in
[`bench/fsops`](./bench/fsops/). None of them are preferences, and none are inherited from
upstream's design. Where a requirement came from a single run, it says so.

Evidence base: **264 runs** across two sweeps plus targeted probes, 2026-08-12/13,
`cachyos-x8664`, RTX 5080 Laptop 16 GB, Hermes Agent v0.20.0, Ollama 0.32.9.

---

## R1 — One sandbox root. Every path resolved against it, explicitly.

**Evidence.** Hermes' `terminal` tool follows the *process working directory* and ignores
`--in` entirely — confirmed by three probes launching from different directories with `--in`
pointed elsewhere; each returned its launch directory. The **file tools** resolve differently
again: agents wrote `hello.txt`, `project/src/utils/`, `data/` and `logs/` into the
**Hermes-Cpp repository root** while their working tree sat four levels below. One escaped
directory contained `.hermes-tmp.HRRyCn`, proving a Hermes file tool wrote it rather than a
shell command.

**Consequence.** `llama32-3b` scored 0/3 on `01_create_file` having produced the exactly-correct
file with the exactly-correct content, in the wrong directory. The penalty was uneven: models
emitting absolute paths were unaffected. **A tool-design bug was being measured as model
inability.**

**Requirement.** A single sandbox root, set once. Every tool — shell and file alike — resolves
relative paths against that root and nothing else. Never inherit a working directory, never
infer a project or git root, never let two tool families disagree. Paths escaping the root are
rejected, not silently rebased.

## R2 — Constrained decoding on tool arguments.

**Evidence.** `llama3.2:3b` emitted `"['echo', 'HERMES-OK > hello.txt']"` as a `terminal`
command — a Python list where a string belongs. Separately it emitted a whole tool call as
plain prose that was never parsed as a call at all. These are not semantic mistakes; they are
structurally invalid arguments.

**Requirement.** Tool arguments are schema-constrained at decode time (Ollama accepts a JSON
schema via `format`). A schema pinning `command: string` makes that specific failure
impossible. This does not fix wrong-but-well-formed calls, and should not be sold as if it does.

## R3 — Verify by content hash. Never by existence.

**Evidence.** On `05_copy`, two models destroyed `config.ini` and never produced the copy —
`hermes3-8b` overwrote it with the literal word `(unchanged)`, `llama31-8b` with invented
config content. **The assertion "original config.ini still exists" passed in both cases.**

**Requirement.** Post-conditions compare content hashes taken before and after. A file being
present proves nothing. This is also what caught the failure at all — a presence check would
have scored both runs as partial successes.

## R4 — Back up before mutating.

**Evidence.** As R3: silent destructive overwrite with plausible-looking replacement content.
Worse than deletion, because a missing file is obvious and a corrupted config is not.

**Requirement.** Every mutation is preceded by a recoverable snapshot of what it overwrites.
Undo is a first-class operation, not a debugging aid.

> Sweep 1 recorded zero collateral damage and I concluded these models "fail by inaction, not
> destruction." That was true of sweep 1's model set and **did not generalise** — the wider
> field in sweep 2 produced destruction on the second try. Guardrails are not optional.

## R5 — Read back after every write.

**Evidence.** Models write rendering artifacts into file content: `qwen3.5:4b` wrote
`5|2026-08-12 shipped` (a line-number-style prefix), `llama3.2:3b` wrote `<br>HERMES-OK` (an
HTML line break).

> **Corrected 2026-08-13.** This section previously claimed "wire transcripts confirm Hermes
> renders tool results as raw JSON with no prefixes anywhere" and concluded the contamination
> was **model-intrinsic**. That is false, and the Phase 0 data falsifies it directly: of 240
> `read_file` calls recorded in `local-agent-benchmarks/hermes-diagnostic/`, **226 return
> content with `N|` line-number prefixes**, plus a phantom trailing line marker — a one-line
> file comes back as `"1|MARKER-ORBIT-7319\n2|"`. The `5|` prefix above is Hermes' own
> rendering, echoed back by the model.
>
> It is caught in the act in `trials/trial-03/results/gemma26-a4b-q8-01_read.json`, where the
> model's final answer is literally `1|MARKER-ORBIT-7319` and the stage fails for it. Note also
> that the sweep-2 transcripts cited as proof were not collected — `bench/fsops/SWEEP2.md`
> records `tool_calls` as empty for both sweeps.

So this is **partly a tool-design artifact and partly model behaviour**, and the two have not
been separated. It belongs with the E4B rendered-annotation hazard in
[ROADMAP.md](./ROADMAP.md), not in the model-intrinsic column. A tool that returns undecorated
content removes most of it; the `<br>` case is not explained by rendering and may be genuinely
model-intrinsic.

**The requirement is unchanged either way** — read-back is what makes the distinction
unnecessary at runtime — but its classification and the scope claim below both move.

**Requirement.** Any write whose content must match exactly is read back and compared before the
turn is allowed to succeed.

## R6 — Never trust a completion claim. Poll state.

**Evidence.** `llama32-3b` replied `DONE` on an untouched tree in 18 of its 27 failed runs in
sweep 1. Separately, `qwen-4b 03_move_file r3` completed every assertion correctly and then
**hung past the 240s timeout** — the work was done, the agent just never stopped.

**Requirement.** Turn completion is decided by inspecting the filesystem, not by parsing the
model's reply. A supervisor waiting for the model to declare completion would have hung on that
run; one polling repository state would have moved on.

## R7 — Retry with one concrete remaining failure.

**Evidence.** 39% of model×task cells in sweep 1 were unstable — the same model, the same task,
different outcomes across 3 repeats. `llama32-3b` on `01_create_file` scored 3/3, 0/3 and 1/3
across three separate sessions (pooled **4/9**).

**Requirement.** This is the product. A task succeeding ~67% per attempt, retried three times
with state verification between attempts, approaches ~96%. If these models were consistent, an
external supervisor would be pointless — **the instability is the justification for the
architecture**, and it is now measured rather than assumed.

## R8 — Bound turns by wall clock, not just turn count.

**Evidence.** Thinking models are 4–15x slower (65–107s vs 6–17s median) and are also the only
accurate ones — the best non-thinking model scored 10/36 against `qwen3.5:9b`'s 27/36. Timeouts
cluster on the heavier tasks and silently drop runs from the denominator.

**Requirement.** Wall-clock budget per turn and per session, with a timeout treated as a
failure rather than as missing data.

## R9 — Preflight the model before running anything.

**Evidence.** Hermes hard-refuses any model reporting under 64K context. Ollama's default
`num_ctx` is 4096, so every model needs a pinned variant. Separately, Ollama `tools` capability
is an independent gate. Two installed models were disqualified by these — `phi3.5:3.8b`
(no tools) and `llama3-groq-tool-use:8b` (8192 context; the *tool-use-tuned* model is the one
that could not be loaded).

**Requirement.** Check context window and tool capability before the first request. Fail loudly
at startup rather than 60 runs into a job.

---

## What this means for scope

Requirements R3 through R7 — verification, backup, read-back, state polling, retry — have **no
upstream equivalent that these runs revealed**. Hermes Agent did not do them in any observed
run; that is why the failures above were observable at all. The valuable part of Hermes-Cpp is
therefore not ported code, it is new code.

*Stated precisely, because it is a negative claim about code that has not been read:* this is
inferred from behaviour across 408 runs, not from auditing upstream's 38k in-scope lines — every
module in `parity.tsv` is still `NOT_STARTED`. Treat it as well-evidenced, not as verified. If a
module read later turns up an equivalent, it is a finding, not a contradiction.

That reframes the rewrite: the goal is a thin, correct agent loop plus a supervisor that upstream
never had, not a reproduction of upstream's surface area. See [SCOPE.md](./SCOPE.md).
