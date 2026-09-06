# E1 — results, 2026-08-18

Collected under [E1-PROTOCOL.md](./E1-PROTOCOL.md) as frozen; the protocol was not
amended after runs began. Two collections were discarded for cause and are kept in
`results/` under quarantine names rather than deleted — both discards were product
defects the experiment itself exposed, and both are fixed and pinned by tests. That is
the discipline working, not the experiment failing.

## The headline

105 runs: 35 per cell, 7 tasks × 5 repeats, one referee for all three cells.

| task | A (Hermes as shipped) | B1 (hermit, 1 attempt) | B3 (hermit, 3 attempts) | B3−A |
|---|---|---|---|---|
| 01_create_file | 5/5 | 5/5 | 5/5 | 0 |
| 03_move_file | 5/5 | 5/5 | 5/5 | 0 |
| 04_rename | 3/5 | 5/5 | 5/5 | +0.40 |
| 05_copy | 4/5 | 5/5 | 5/5 | +0.20 |
| 09_list_report | 4/5 | 4/5 | 4/5 | 0 |
| 10_bulk_move | 2/5 | 5/5 | 5/5 | +0.60 |
| 11_append_preserve | 3/5 | 4/5 | 4/5 | +0.20 |
| **total** | **26/35 (74%)** | **33/35 (94%)** | **33/35 (94%)** | **+20 pts** |

Sign test across tasks (B3 vs A): **4 wins, 0 losses, 3 ties — p = 0.125.** Read that
honestly: the direction is uniformly positive and nothing lost, but seven paired tasks
cannot clear p < 0.05 with four informative pairs. The per-task noise floor (≥ 3/5)
admits exactly one per-task finding: **`10_bulk_move`, +0.60**. What the baseline
actually did there, from its own records: in two runs the three `.log` files
**vanished outright** — absent from the top level *and* absent from `logs/` — and in a
third they were copied instead of moved; the excluded files were never touched, so
this was not pattern over-application but the work itself being destroyed. Hermit's
bundle went ten for ten. Said carefully: this is a **bundle-versus-bundle**
comparison — harness, tools, prompt, endpoint all differ together as designed — so no
per-task win is attributable to a single mechanism. The one exception is R7, whose
retry sentences are in the logs. The suite-level +20 points is reported as the
observed delta, not as a significance claim; power needs more tasks (waiting on shell
and delete) or more repeats.

Per-task wall clock, median (range) over 5 repeats, per DESIGN guardrail 3:

| task | A | B1 | B3 |
|---|---|---|---|
| 01_create_file | 32s (32–63) | 9s (6–12) | 9s (8–58) |
| 03_move_file | 114s (60–122) | 6s (4–8) | 6s (3–7) |
| 04_rename | 95s (64–122) | 5s (3–8) | 5s (2–17) |
| 05_copy | 65s (32–132) | 6s (4–9) | 32s (7–52) |
| 09_list_report | 87s (64–133) | 13s (4–70) | 14s (5–40) |
| 10_bulk_move | 114s (96–122) | 7s (7–10) | 9s (6–14) |
| 11_append_preserve | 64s (32–128) | 13s (10–198) | 13s (7–249) |

Same bundle caveat: the 5–20× speed difference is the whole stacks compared, not any
one component measured.

## R7, observed doing its job

Re-invocation fired on exactly one task, and converted every time it fired:

| run | attempts | outcome |
|---|---|---|
| B3 · 05_copy r1 | 3 | passed |
| B3 · 05_copy r2 | 3 | passed |
| B3 · 05_copy r3 | 2 | passed |
| B3 · 05_copy r4 | 2 | passed |

Two honesty notes before the story. First: the **pre-registered decomposition
(analysis rule 2, B3 − B1) measured zero on every task** — B1 and B3 finished 33/35
each. Everything below is within-B3, attempt-level evidence, pooled post hoc, n = 4
conversions; it is the product observed working, not the pre-registered column
clearing a bar. Second: the pooled figure hides an inhomogeneity — B1's five first
attempts all passed while B3's first attempts went 1/5, a split unlikely under one
shared per-attempt rate, so "~60% per attempt" assumes an exchangeability the two
cells' first attempts do not obviously show.

With that stated: pooling every first attempt at `05_copy` in this collection gives
**pass@1 = 6/10**, and pass-after-retries **5/5** — the ~67%→~96% arithmetic at its
own scale, a judge that names the one failure, a fresh session that acts on it. The sentence the retries were handed is worth quoting,
because it is the evidence base's own copy-corruption failure caught mid-flight:
`config.ini.bak does not have the bytes config.ini had`. Traced in the logs to the
byte: the model *read the original correctly*, then re-typed its content into the new
file and dropped the trailing newline — a one-byte difference invisible to an
existence check, caught by R3's hash, and fixed by the re-invoked session. The cost is published with the win: cell B3 spent 758 s and
461 K tokens against B1's 625 s and 309 K — retries re-send context, and four runs paid
it.

Everywhere else, R7 had nothing to do — which is the second finding:

## Where the remaining failures live

All four arm-B failures are **semantic**, invisible to the structural judge by
construction, and the referee caught every one from the tree or the log:

- `09_list_report` ×2 — the model **computed the correct count** ("Total count of
  regular files: 5", "Total = 5") and never emitted the required literal `FILECOUNT=`
  line — a pure format failure, in the thinking channel both times. The tree was
  untouched and every guard met.
- `11_append_preserve` ×2 — the append mangled the file's content while `exists:
  tally.txt` stayed vacuously true. The protocol predicted B3−B1 = 0 on this task for
  exactly this reason, and B3−B1 = 0 is what was measured.

Zero structural failures escaped the supervisor in 70 hermit runs, and zero collateral
damage occurred in any cell. The measured frontier is now content, not structure — the
data lands on the semantic judge as the next unit of leverage, with re-invocation
already proven able to consume whatever findings it produces.

## Two product defects this experiment exposed — and a third finding

Both defects were invisible to every prior test and both discards are kept under
quarantine names in `results/`. (The third product-relevant finding — a 14%
malformed-tool-call transport-failure rate on the native endpoint — is not a defect
in hermit's code and is recorded under losses below.)

1. **Hermit pinned `temperature 0.0` on every request since the client was written**
   (`…-pinned-temp0-protocol-violation.json`). The first arm-B collection ran fully
   deterministic against a freely-sampling baseline — and, worse for the product,
   suppressed R7 structurally: at temperature 0 a retry replays the failed attempt, and
   the collection shows `04_rename` failing identically three attempts in a row, twice.
   Fixed: `temperature` is optional end-to-end and **omitted by default**, so the
   model's own sampling is what the supervisor faces.
2. **A model can answer into its thinking channel and hermit dropped that answer**
   (`…-reply-channel-dropped.json`). Wire-captured: Ollama returned `content: ""` with
   the literal correct answer as the last line of `thinking`, 10 runs of 10 on task 09,
   while the same model under Hermes closes its think block and answers in content.
   Fixed: `LoopOutcome::final_reasoning` carries the channel and the CLI prints it,
   clearly labeled; nothing judges it (D13 stands). Task 09 went 0/10 → 8/10,
   matching the baseline's 4/5 — with the caveat, detailed under losses, that half of
   those 8 passes were graded from the labeled thinking output.

## Losses and oddities, published per guardrail 7

- **14 runs passed the referee while hermit exited 1**, and the stop-reason taxonomy
  matters: across all 70 runs, 55 answered, **10 stopped on `the request to Ollama
  failed`**, 4 on the turn budget, 1 on the wall clock. The 10 transport stops are a
  finding of their own: the model finished the work, then emitted a **malformed
  tool-call** (`XML syntax error … element <function> closed by </parameter>`) that
  Ollama's native-endpoint parser rejected with an HTTP 500, and D7's no-retry rule
  ended the run — a **14% malformed-tool-call rate** for this model on `/api/chat`,
  against zero such failures in 35 Hermes runs over `/v1`. The 4 turn-budget stops all
  ended with the work already complete and the model still calling tools — the
  re-read/re-verify loop. "Bounded out or transport-dropped with every
  stated expectation already met" arguably deserves a better exit than 1; open
  question, not changed mid-experiment.
- **A referee asymmetry, disclosed**: `run_fsops` excludes a run from scoring when the
  harness itself errors (its `FAILURE_SIGNATURES` gate); `run_delta` grades every
  hermit run from the tree, including those 10 transport-stopped runs, all of which
  passed. No arm A run hit the gate in this collection, so the asymmetry changed no
  arm A number — and excluding all 10 transport runs from arm B still leaves
  B1 = 25/27 (93%) and B3 = 31/33 (94%) against A's 74%.
- **One arm-B run exhausted hermit's internal R8 budget** (`11_append_preserve`,
  249 s) without tripping the runner's `timed_out`, which only the subprocess hard
  kill sets. Under a strict reading of the protocol's timeout rule that run should be
  a failure on time alone; it failed its content checks anyway, so no score depends on
  the reading. The runner now records the stop reason so the next collection can fold
  it in cleanly.
- **3 runs exited 0 while the referee failed them** — the judge-blind semantic cases
  above, `--unjudged` doing exactly what it promises: counting what was not examined.
- **Half of task 09's passes were graded from the thinking channel.** Of the 8 passing
  09 runs, 4 carried `FILECOUNT=5` in content and 4 only in the labeled
  thinking-channel output the reply-surfacing fix added mid-experiment. The frozen
  protocol's "each prints the model's reply" was written when hermit printed content
  only, so this widened arm B's marker surface after freeze; disclosed rather than
  hidden. Arm A never needed the channel — all five Hermes 09 runs answered in
  content. Without the fix, 09 reads 2/5 + 2/5 and both hermit totals become 31/35;
  the headline direction is unchanged either way.
- **The discarded collections did not manufacture the direction**: the quarantined
  temp-0 collection scored 59/70 (84%) and the reply-channel one 56/70 (80%) — both
  above the baseline's 74% — so arm B beat arm A under every binary this experiment
  ran, including the two rejected ones.
- B3's wall-clock premium over B1 was 21% for this pass profile.

## Provenance

| | |
|---|---|
| arm A file | `fsops/results/fsops-20260818T215558Z.json`, in **hermit-bench** |
| arm B file | `delta/results/delta-armB-20260818T230731Z.json`, in **hermit-bench** |
| arm B logs | `delta/results/logs-20260818T230731Z/`, in **hermit-bench** — every log-traced claim above is checkable there |
| wire capture | `delta/results/evidence/thinking-channel-tap-20260818.jsonl`, in **hermit-bench** — the raw request/response pairs behind the thinking-channel finding |

The raw data lives in the **hermit-bench** repository — the public extraction of both
suites, their frozen protocols, this document, and every result file above, including
the two quarantined collections. This repo keeps the suites as development tooling and
carries no result artifacts from 2026-08-18 on; `run_delta.py --report` still
reproduces the tables from a local, untracked copy of the same files.
| model | `fsops-qwen3.5-9b:64k`, digest `2080d161fb89`, `num_ctx` 65536 both arms |
| Hermes Agent | v0.20.0 (2026.8.3), as shipped |
| Ollama | 0.32.9 |
| machine | `cachyos-x8664`, NVIDIA RTX 5080 Laptop 16GB — the evidence-base machine |
| tasks.py | sha256 `615e8b6cad3a…` |
| hermit | `a5722dc` plus the two fixes above, committed together with this document |
| sampling | model defaults, unpinned, both arms |

Arm A tokens are not reported: the only capture path is the recording proxy, whose
destabilisation bug is documented in `bench/fsops/NEXT-RUN.md` and unfixed. Arm B
tokens come from hermit's own per-turn accounting.

The `hermit` id above is the one recorded at collection time and does not resolve today: the
repository's history was rewritten on 2026-09-04. `a5722dc` is now `68cfc63`; the mapping for
every pre-rewrite id is [docs/91-commit-map.md](../../docs/91-commit-map.md). The recorded id
is left as collected rather than edited to match the new history.

## What would change this picture

More power: the five excluded tasks join when shell and delete land, and that re-run is
a new protocol against this one's numbers. More leverage: a semantic judge would have
made all four remaining failures visible to R7 — and R7 converted 4/4 of the failures
it could see.
