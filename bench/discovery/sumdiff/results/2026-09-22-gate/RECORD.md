# DOCKET 1.15 gate — run record, 2026-09-22

**Position: passed as written, with one named gap.** The scorer exists and separates
known-different attempts exactly. A 30B-tier model working in the sandbox found a set the
official scorer places above the classical construction. It did not deliver that set in
`solve.py` before its budget ran out, so the attempt as run scored the seed's 1.0. The gate asks
whether an honest scalar domain exists at local tier, and the record says yes. It does not say
the attempt loop turns discoveries into deliverables, and 1.17 inherits that question.

## Setup

| | |
|---|---|
| host | `cachyos-kitchen-pc-x8664`, kernel 7.2.6-1-cachyos |
| CPU | Intel Core i7-14700KF, 28 threads, governor `powersave` |
| GPU | NVIDIA RTX PRO 5000 72GB Blackwell, driver 615.71.09 |
| Ollama | 0.34.2 |
| Hermit | commit `8a4fc7b` (`bench/discovery-design`), `build/hermit` sha256 `c5f1250fcce1…` (full hash in every `score.json`) |
| Python | 3.14.7, standard library only |
| scorer | `score.py` sha256 `25a1f4e09c97…`, `objective.py` sha256 `b4d4e99315ed…`; the same for every score below |
| confinement | `hermit mcp` shell, D10 Landlock; its probe reported Enforced at every server start |
| test suite | `ctest --test-dir build`: 821/821 passed, one skipped by design (`DefaultReuseMissesAMapSharedWrite`). The ASan build was not re-run: no C++ changed |

## Scores

Every score is exact, not an estimate. Every valid candidate printed byte-identical output on
both repeats.

| candidate | who wrote it | score | \|A\| | \|A+A\| | \|A−A\| | note |
|---|---|---|---|---|---|---|
| `controls/interval` (= seed) | hand | 1.000000 | 100 | 199 | 199 | the starting program |
| `controls/conway` | hand | 1.034421 | 8 | 26 | 25 | the classical 8-element set |
| `controls/malformed` | hand | 0 (invalid) | — | — | — | prose in stdout, refused by the parser |
| `controls/overtime` | hand | 0 (invalid) | — | — | — | killed at 60 s by R8; determinism recorded as null, not false |
| `controls/escape` | hand | 1.000000 | 4 | 7 | 7 | writes to `~` and `/tmp` refused (EACCES); its planted `eval/score.json` of 99.0 and a detached rewriter had no effect |
| `a1-qwen3.8-27b/eval` | model, as delivered | 1.000000 | 100 | 199 | 199 | `solve.py` unchanged from the seed |
| `a1-qwen3.8-27b/posthoc` | model's search, re-scored | **1.046752** | 12 | 39 | 37 | {0,1,2,4,5,10,13,14,15,16,18,19}, printed by the model's `scratch/ls.py` |

The Done-when condition, "two known-different attempts score measurably differently", is met by
interval versus Conway (1.0 versus 1.034421), and by the model's discovered set against both.

## The model attempt

Absolute working paths in `run.json` and `agent.log` were rewritten to `$WORK` after the run. Nothing else in either file was edited.

`qwen3.8:27b-q8_0`, digest `8f5fb6b71ea00052cbe8545738c55ce61112c4e571cb60ca4dad00b131766039`.
Flags: `--shell --shell-timeout 90 --max-turns 24 --budget 1500 --attempts 1
--expect exists:solve.py`, 65,536-token window. The exact instruction and argv are in
`a1-qwen3.8-27b/run.json`, and the per-turn trace is in `agent.log`.

What happened, from the trace:

- It spent all 1,500 s over 22 turns: 23 calls, none refused, no window rebuilds. Six turns
  generated more than 6,000 tokens each, with 17k–30k characters of thinking.
- It wrote 14 scratch scripts and never edited `solve.py`. The instruction said "edit only
  solve.py". The scorer copies only `solve.py`, so the extra files cost nothing, but the rule
  was not followed.
- It rediscovered Conway's set by exhaustive search (turn 13). A local search then reached
  1.03501 (turn 15) and 1.04675 (turn 19). Each figure was measured by its own script. The
  1.04675 set was later re-run through the official scorer and confirmed as 1.046752.
- The budget ran out at turn 22, while it was writing another exploration script.

The post-hoc score is stated separately on purpose. It is what the model's *search* found,
re-run by the scorer after the attempt. It is not what the *attempt* delivered. Treating it as
the attempt's result would credit the model with work it never committed to.

## What was not checked

- **Repeats.** One model attempt, one model. Nothing here measures how often a 30B-tier
  attempt reaches 1.046752, or even beats 1.0. n = 1 is underpowered for any rate, and this
  record claims no rate.
- **Other models.** qwen3.6:35b-a3b, gemma4:31b, granite4.2:30b and nemotron-3.5-lightning:30b
  all passed `hermit preflight` on this machine. None was run. The operator stopped the session
  at one model, which was the agreed scope.
- **Whether a nudge fixes delivery.** An instruction to update `solve.py` after every
  improvement, a larger budget, or `--attempts` with a `satisfies:` criterion might each close
  the gap. None was tried. Trying them here would have become prompt tuning before a protocol
  exists.
- **Egress during scoring**, and **detached processes outliving a scoring call.** Both are
  open and are named in [`../../README.md`](../../README.md).
- **The runtime-scored alternative.** Argued against in the README, not built or measured.

## Reproducing

```
cmake --build build
python3 bench/discovery/sumdiff/score.py bench/discovery/sumdiff/controls/conway.py /tmp/out
python3 bench/discovery/sumdiff/attempt.py --model qwen3.8:27b-q8_0 \
    --record-dir /tmp/rec --work-dir /tmp/work
```

The scores of the controls and of the post-hoc set are deterministic and should reproduce
exactly on any Linux host whose confinement probe reports Enforced. The model attempt will
not reproduce exactly. That is the variance this repository has been measuring since Phase 0.
