# sumdiff — the scored domain for DOCKET 1.15

**Status (2026-09-22): built on Kitchen; gate result in [`results/2026-09-22-gate/RECORD.md`](./results/2026-09-22-gate/RECORD.md).**
This is the gate for the discovery cluster ([`../DESIGN.md`](../DESIGN.md), DOCKET 1.15–1.21).
It answers one question only: does a scored domain exist here, at local model tier, that
returns an honest scalar per attempt? It says nothing about exploration policies, replay, or
whether any of the cluster's later claims hold.

## The task

A program, `solve.py`, prints a finite set of integers A. Its score is

    Gamma(A) = log(|A+A| / |A|) / log(|A-A| / |A|)        higher is better

— the sum–difference problem, Problem 2 in the source paper's appendix (arXiv:2609.14858v1).
The model is asked to improve `solve.py` inside a sandbox root. Anchors, measured by this
scorer: an interval scores exactly 1.0 (the seed); Conway's 8-element set
{0,2,3,4,7,11,12,14} scores 1.034421 (|A+A| = 26, |A−A| = 25). The paper reports 1.145427 for
its own system and 1.143975 for SimpleTES — quoted, not reproduced, and produced without the
caps below, so not a ceiling that applies here.

Caps are part of the task: at most 20,000 distinct integers, max − min ≤ 1,048,576, 60 s
wall clock, stdout of integers and list punctuation only, Python standard library only.
Measured cost of scoring a set at both caps: 1.2 s.

## Why this domain and not runtime

DESIGN.md names two candidate shapes: script runtime on a pinned input, or a fixed scorer's
objective value. This takes the second. The reasons are arguments, not measurements; the
runtime alternative was not built:

- **The score is exact.** Gamma is a function of three set sizes, computed from the printed
  output. There is no noise floor to measure and argue about, which is the property the design
  wants most: replay is cheap because recorded scores are trustworthy as recorded.
- **No trusted timer wrapped around untrusted code.** Timing a candidate means a clock
  running either inside the candidate's confined root, beside the thing it measures, or
  outside it, across a process launch and a Landlock setup. Neither is hard, but both
  need a noise-floor measurement before any difference counts, and this domain needs none.
- **The objective has a ladder.** 1.0 for any interval, ~1.034 for the smallest classical
  construction, ~1.14 reported for large searched ones. Whether local models climb it is
  what the gate attempts measure.

What it costs: the domain is far from Hermit's own job of filesystem work, and a model can
score by recalling a construction rather than discovering one. Neither matters to the gate.
Both matter to 1.21, which has to decide whether this domain is where the validation
experiment runs.

## The scorer contract

| file | role |
|---|---|
| [`objective.py`](./objective.py) | Gamma and the parse/validity rules. Pure function of stdout. Run as a script, it runs `solve.py` and prints the score: the model's development loop |
| [`score.py`](./score.py) | The official scorer. Writes `OUT/eval/score.json` |
| [`attempt.py`](./attempt.py) | One recorded model attempt: seed a root, `hermit agent --shell`, score, record |
| [`seed/solve.py`](./seed/solve.py) | The starting program: an interval, score 1.0 |
| [`controls/`](./controls/) | Hand-written candidates with known expected outcomes |

**Trust boundary.** The candidate is untrusted code, and runs through an existing confined
path: `hermit mcp` with `shell` enabled, which refuses to start unless D10's own probe
reports Enforced. It runs in a fresh root holding only a copy of `solve.py`, a new root per
repeat. The objective is computed in the unconfined parent from the captured stdout, and
`score.json` is written under the record directory, which the confined child was never
granted.

This deviates from the letter of DOCKET 1.15's shape ("scored by an existing confined path
writing `eval/score.json`"), deliberately. The first design wrote the file from inside the
confined root, and a candidate can rewrite anything under its own writable root, including
from a detached process that outlives the call. `controls/escape.py` does exactly that, and
its recorded score is its real one (1.0), not the 99.0 it plants.

The attempt's own root gets a copy of `objective.py` as `check.py` so the model can see its
score. The official score never reads that copy or anything else from the attempt root
except `solve.py`.

**Repeats measure determinism, not variance.** The objective is exact, so a candidate either
prints the same set on every run or it does not. `deterministic_across_repeats` records which
(null when a repeat printed nothing). A candidate that searches with an unseeded RNG is
legitimate, but its recorded score is one draw, and the record says so.

## What this does not check

- **Confined egress.** D10 governs the filesystem only; a candidate can reach the network
  during scoring. The gate's candidates did not try. ROUTING.md §8's reason for keeping shell
  off the default MCP surface applies here unchanged.
- **Detached processes outliving the scoring call.** `start_new_session` escapes the process
  group R8 kills on timeout. They cannot touch the score, but they can hold CPU. Not bounded.
- **Recall versus discovery.** A score says what set was printed, not how it was found.
- **CPU-dependence of search candidates.** A candidate that searches until a deadline finds
  different sets on different hardware. The record pins the host; it does not pin the result.
