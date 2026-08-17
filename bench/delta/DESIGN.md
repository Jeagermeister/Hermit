# bench/delta — the before/after measurement

Design settled 2026-08-16, deliberately **before any run exists** — the rubric and the
overturn conditions are recorded here first, in the same discipline
[bench/fsops](../fsops/) learned the hard way: a leaderboard whose grader had holes got
retracted, and a headline delta that sat inside the noise floor got corrected. This
document exists so neither happens again.

**Status: design only. Runs are blocked on [ROUTING.md](../../ROUTING.md) §12 steps 5–6**
(the agent loop and `mcp.cpp`) — which is deliberate: this suite is those steps' first
real workout, and the proof gate for shipping them.

**What this measures in one line:** the same tasks, with and without the Hermit layer —
wall time, completeness graded from the filesystem, and tokens — reported as *paired
deltas*, never as a leaderboard.

---

## Two experiments, separated on purpose

They answer different questions for different audiences, and they have different hardware
requirements. Conflating them would weaken both.

### E2 — Economics (the delegation delta)

**Question:** what does a large model spend doing filesystem work *itself*, versus
delegating the hands to Hermit over MCP?

**Requires no local model.** Tier 0 is inference-free (§2 of ROUTING.md): the delegation
win comes from verified tools keeping file bodies, tree dumps and retries out of paid
context — hashes and row summaries travel; bytes stay local. This experiment therefore
runs in environments with no local-inference hardware at all, which is exactly the
environment the workplace case describes.

| | arm A — direct | arm B — delegated |
|---|---|---|
| the model | one large cloud model | the same model, same version, pinned |
| its tools | a good-faith naive file toolset (read/write/list as plain schemas, results verbatim in context) | the Hermit MCP surface |
| what flows through paid context | every file body, every listing, every retry | tool rows: paths, hashes, tuples, refusals |

**Token accounting:** drive both arms through an API that reports exact usage per call
(input, output, cache reads billed at their real rate), and define the metric before the
first run: *billed tokens per completed task*. Agentic-IDE credit systems are opaque
billing; where a credits number is wanted (e.g. Kiro), report measured tokens as the
primary number and credit-equivalents as a **labeled estimate**, never the headline.

### E1 — Reliability (the supervision delta)

**Question:** R7's arithmetic — ~67% per attempt approaching ~96% under verified retries —
is currently a claim computed from measured instability, not a measured outcome. This
experiment tests our own thesis.

**Requires local hardware; designed to be lifted out.** Everything under `bench/delta/`
that E1 needs stays self-contained — no tailnet dependencies, no private-repo references —
so the local suite can be extracted to its own public repository intact.

| | arm A — bare | arm B — supervised |
|---|---|---|
| the model | `qwen3.5:9b` via Ollama, pinned tag | the same |
| the harness | upstream Hermes Agent as shipped — the good-faith baseline, and literally the documented origin of the evidence base | the Hermit loop |
| tokens | exact both sides: Ollama reports `prompt_eval_count` / `eval_count` per request | same |

The one column that *is* the product: **pass@1 versus pass-after-retries**, reported
separately. If supervision works, the gap between those columns is where it shows.

---

## The task set

Reuse [bench/fsops](../fsops/) tasks — they come with documented failure modes
(`01_create_file`'s wrong-directory escape, `05_copy`'s destructive overwrite,
`03_move_file`'s completed-then-hung run) — plus per-task **hash-based assertions**
written before any run, graded from the filesystem with the same R3 machinery the product
is built on. Never from the transcript: "the file still exists" once passed while the
content had been destroyed, and that lesson is load-bearing here.

ROUTING.md §12 step 8's scheduled fsops re-run **is** E1's arm A — same tasks, same
model, same machine, collected once, used twice.

## The guardrails, pre-registered

1. **The claim is the delta, never a ranking.** Paired per-task comparisons, sign test
   across tasks. Rankings died at ±7 points of noise and p=0.37 across repeats; paired
   deltas on identical tasks are the design that survives that history.
2. **Noise floor first.** Same-arm repeats before any cross-arm claim; no delta smaller
   than the measured floor gets reported as a finding.
3. **≥5 repeats per cell; medians with spread shown.** Single runs are anecdotes.
4. **Timeouts are failures**, never missing data (R8's rule — dropped timeouts flattered
   numbers that were later walked back).
5. **Rubric frozen before runs.** The tournament's 30/30 ceiling was reachable from the
   spec alone; graders get reviewed as adversarially as code, then pinned.
6. **No strawman baselines.** Arm A must be the good-faith version of "without" — upstream
   as shipped, or a naive toolset a reasonable engineer would actually write.
7. **Publish the losses.** Tasks where the layer costs more — a trivial single-file write
   paying supervision overhead — go in the report with the wins. Expected, and honest.
8. **Versions pinned in the report**: model tags, Ollama version, Hermit commit,
   task-set hash. A number nobody can reproduce is a rumor.

## What would overturn the thesis

Recorded now, so the results cannot be argued into the conclusion later:

- **E1 fails** if arm B's completeness does not beat arm A's beyond the measured noise
  floor — or if pass-after-retries fails to close most of the gap R7's arithmetic
  predicts. Either result gets published, not shelved.
- **E2 fails** if the billed-token delta is within noise or negative on the majority of
  tasks. Plausible mechanism worth watching: MCP tool-definition overhead per request
  outweighing the file-body savings on small tasks. If that is the finding, it bounds
  *when* delegation pays, and the report says so.

## What this is not

Not a model leaderboard, not a harness shootout, not a claim about models other than the
ones named, and not transferable across hardware without re-running — every lesson in
[the evidence base](../fsops/) says per-model, per-machine numbers do not travel.
