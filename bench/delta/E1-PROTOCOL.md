# E1 — the reliability run, pre-registered

Frozen before the first run, in the discipline [DESIGN.md](./DESIGN.md) records: the cells,
the task set, the expectation strings, the grading rule and the analysis are all written
down here first, so the results cannot be argued into the conclusion later. Any change to
this file after runs begin is a new experiment, not an amendment.

**The question** (DESIGN.md E1): R7's arithmetic — ~67% per attempt approaching ~96% under
verified retries — is a claim computed from measured instability. This measures it.

## The cells

| cell | harness | attempts | what it isolates |
|---|---|---|---|
| **A** | upstream Hermes Agent as shipped, via `bench/fsops/run_fsops.py` | its own | the good-faith baseline — literally the harness that produced the evidence base |
| **B1** | `hermit agent`, `--attempts 1` | 1 | supervision without re-invocation (pass@1) |
| **B3** | `hermit agent`, `--attempts 3` | 3 | the product (pass-after-retries) |

Same model in every cell: **`fsops-qwen3.5-9b:64k`** — the pinned variant `run_fsops.py`
builds (`num_ctx 65536`; Hermes refuses anything under 64K). Hermit sends
`num_ctx 65536` explicitly per request (D8, `max_num_ctx` default), so both harnesses run
the model at the same window. Same machine for every cell: this laptop
(`BENCH_GPU="NVIDIA RTX 5080 Laptop 16GB"`), which is where the fsops evidence base ran —
per-machine numbers do not travel, so the pairing must not cross machines.

**5 repeats per cell** (DESIGN guardrail 3). **Sampling unpinned** — model defaults, no
`--deterministic` — because R7's arithmetic is *about* sampling variance: a retry exploits
the independence of attempts, and pinning temperature to 0 would suppress the very
mechanism under test. Production sampling is what the supervisor faces.

## The task set: seven of twelve, and why

Hermit's Tier 0 surface has **no shell, no delete, and no way to create an empty
directory** — shell waits on kernel confinement (D10), delete is deferred
(ROUTING.md §11). Paired deltas require identical tasks, so tasks Hermit cannot express
are excluded rather than scored as zeros, and the exclusions are published:

| task | in E1? | reason |
|---|---|---|
| `01_create_file` | ✔ | |
| `02_make_dirs` | ✘ | requires *empty* directories and "create no files"; no mkdir tool |
| `03_move_file` | ✔ | |
| `04_rename` | ✔ | |
| `05_copy` | ✔ | |
| `06_selective_delete` | ✘ | no delete tool |
| `07_run_script` | ✘ | no shell |
| `08_write_and_run_script` | ✘ | no shell, no chmod |
| `09_list_report` | ✔ | |
| `10_bulk_move` | ✔ | |
| `11_append_preserve` | ✔ | |
| `12_multi_step` | ✘ | step 4 runs a script; no shell |

Five of the seven included tasks carry documented failure modes from the sweeps
(`01`'s wrong-directory escape, `03`'s completed-then-hung run, `05`'s destructive
overwrite, `10`'s glob over-application, `11`'s DONE-with-no-change), so the subset is
not tilted toward easy wins. When shell and delete land, the excluded five become a
follow-up experiment under a new protocol — not additions to this one.

## The expectation strings, verbatim

What arm B's supervisor is told to enforce — these drive R7's retries. **The grade does
not come from these** (see Grading below); they are the product's input, not the
experiment's referee. `--unjudged N` counts each requirement the four predicates cannot
express, exactly as the product intends.

| task | `--expect` (each repeatable flag listed) | `--unjudged` |
|---|---|---|
| 01 | `exists:hello.txt` · `preserved:README.md=README.md` | 1 (content is `HERMES-OK`) |
| 03 | `preserved:report.txt=archive/report.txt` · `absent:report.txt` | 0 |
| 04 | `preserved:draft_v1.md=final.md` · `absent:draft_v1.md` | 0 |
| 05 | `preserved:config.ini=config.ini.bak` · `preserved:config.ini=config.ini` | 0 |
| 09 | `preserved:P=P` for each of the five `tree/` files | 1 (`FILECOUNT=5` is a reply marker) |
| 10 | `preserved:X=logs/X` · `absent:X` for each of the three logs · `preserved:readme.txt=readme.txt` · `preserved:notes.txt=notes.txt` | 0 |
| 11 | `exists:tally.txt` | 1 (append-and-preserve is content, not structure) |

**Stated limitation, measured rather than hidden:** task 11's classic failure — `DONE`
over an unchanged file — is invisible to the structural judge (`exists:tally.txt` is a
guard the baseline already satisfies), so R7 cannot retry on it. B3−B1 on task 11 is
therefore expected to be ~0, and that number is part of the finding: it bounds what the
structural judge buys and is the standing argument for the semantic judge.

## Grading — one referee for every cell

A run passes iff **all of `tasks.py`'s per-task checks pass, no collateral damage, no
collateral deletion, and the run did not time out** — the same functions, applied to the
tree, for both harnesses. Hermit's own verdict and exit code are *recorded* (agreement
between the product's verdict and the referee is itself a finding) but never grade.
Marker checks (`09`) read the harness stdout in both arms, where each prints the model's
reply. Timeouts are failures, never missing data (R8; DESIGN guardrail 4).

Bounds, equal where the concept is shared: 240 s wall per run for arm A (the fsops
default); for arm B, 240 s `--budget` and `--chat-timeout` per attempt, `--max-turns 12`
per attempt, with a hard kill at `attempts × 240 + 120` s. B3 spending more wall clock
than A is expected, honest, and published (guardrail 7) — retries cost time, and the
report says what they cost.

## Analysis, decided now

1. **Primary: the paired sign test across the 7 tasks** on per-task pass-rate deltas,
   B3 − A. Wins vs losses, ties dropped, two-sided binomial. The claim is the delta,
   never a ranking (guardrail 1).
2. **The decomposition, reported separately:** B1 − A (supervision alone) and B3 − B1
   (re-invocation alone). The second is the column that is the product.
3. **Noise floor:** no per-task difference smaller than **3/5 runs** is reported as a
   per-task finding — within-arm repeats in the sweeps flipped 3/3→0/3 on the simplest
   task, so anything smaller is inside demonstrated same-arm noise. Raw counts are
   published for every cell regardless.
4. **Tokens:** recorded for arm B from Hermit's per-turn output (`prompt`, `generated`).
   **Not measured for arm A** and said so: the only capture path is the recording proxy,
   which destabilises long runs (NEXT-RUN.md, unfixed). No token claim is made in E1.
5. **Versions pinned in the report:** model tag and digest, Ollama version, Hermit
   commit, Hermes Agent version, `sha256(tasks.py)`, hostname and GPU.

## What would overturn the thesis (restated from DESIGN.md)

E1 fails if B3's completeness does not beat A beyond the floor above — or if B3 − B1
fails to close most of the gap the ~67%→~96% arithmetic predicts on the tasks the judge
can see (all but 11). Either result is published, not shelved.
