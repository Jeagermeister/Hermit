# The next run — deterministic pass

**Do this on an idle GPU.** Both sweeps so far ran at model-default sampling, so every score
mixes two different things: whether the model *can* do the task, and whether the dice landed.
39% of model×task cells in sweep 1 were unstable, and the control swung 3/3 → 0/3 on the
simplest task in the suite between sweeps. Until sampling is pinned, no per-task number means
anything.

```bash
cd ~/Source/Hermit/bench/fsops
./selftest.py                                    # always first
BENCH_GPU="NVIDIA RTX 5080 Laptop 16GB" \
  ./run_fsops.py --models qwen-9b llama31-8b llama32-3b \
                 --repeats 3 --deterministic --seed 1337
```

`--deterministic` builds a `temperature 0`, fixed-seed variant of each model and runs against
that. It has to work that way: there are no `OLLAMA_TEMPERATURE` / `OLLAMA_SEED` environment
variables, so setting them would be silently ignored and produce *fake* determinism.

## How to read it

| outcome | meaning |
|---|---|
| task still fails at temp 0, all 3 repeats | **genuine capability limit** — design around it |
| task flips to passing, or stops varying | it was only ever **sampling noise** — more repeats or a retry loop fixes it |

Repeats stay at 3 even though sampling is pinned: Ollama is not bit-reproducible across loads,
so this measures *reduced* variance, not zero.

**Do not compare deterministic numbers to the sweep 1 / sweep 2 tables.** Those ran unpinned.
Pinned and unpinned runs answer different questions and belong in different tables.

## Two things to fix or confirm first

**1. ~~Confirm the working-directory escape.~~ Confirmed 2026-08-13 — it was the file tools,
not the shell.** Kept below for method. See `results/evidence/escaped-to-repo-root/`.
Agents wrote correct output into the repo root instead of `--in`, and it was scored as failure.
Run one shell task with `--transcripts` and read the `workdir` argument the model sends:

```bash
./run_fsops.py --models qwen-9b --tasks 07_run_script --repeats 1 --transcripts
python3 -c "import json;[print(json.loads(l)['response'].get('tool_calls')) for l in open('transcripts/qwen-9b-07_run_script-r1.jsonl')]"
```

Working trees now live in `~/.cache/hermit-fsops/runs` (override `FSOPS_RUNS`), outside any git
repo, so this can no longer pollute tracked source. **Re-run at least `01_create_file`,
`02_make_dirs` and `08_write_and_run_script` from outside the repo before trusting their
scores** — those are the three with confirmed escaped artifacts.

**Note the proxy destabilises long runs** (qwen-4b: 0 timeouts without, 4 in 7 with). Use
`--transcripts` for single-task diagnosis only, never a full sweep. That bug is unfixed.

**Note also that the destabilisation figure is n=3 against n=7 and was never task-matched.**
Sweep 1 records qwen-4b at 5 timeouts in 36 runs unproxied, and they cluster — 5 of the 12 runs
on `03_move_file`, `08_write_and_run_script`, `10_bulk_move` and `11_append_preserve`, versus 0
of the 24 elsewhere. Since `--transcripts` exists to diagnose *failing* tasks, the proxy arm was
most likely drawn from that 42% bucket, against which 4-in-7 is unremarkable. The baseline
appears in no committed results file. **Before treating the proxy as the cause, check which task
those 7 runs used** — and re-measure paired, alternating run by run, with the timeout raised so
slow runs are timed rather than censored.

**Transcript-derived fields in published results before 2026-08-16 are unreliable.**
`tool_calls` and `reasoning_chars` were read unconditionally while only the recording branch
unlinked the transcript, so a transcripts-off run inherited the previous proxy run's file for the
same model/task/repeat cell. `fsops-20260813T123040Z.json` shows it: `"transcripts": false`,
`"transcript": null`, and `01_create_file` r1 reporting `"tool_calls": ["terminal"]`. Both fields
now gate on the proxy and read empty when it is off. Treat those fields in older result files as
absent, not as evidence — the scores themselves are unaffected, since nothing scored off them.

**2. `gemma-e4b` is still untested.** Its base was removed from Ollama; the blobs are retained,
so `ollama pull gemma4:e4b-it-qat` should be near-instant.

## Then

- Re-run `hermes3-8b` vs `llama31-8b`. 3/36 vs 10/36 at matched size and quant is suggestive but
  inside the noise floor, and it is the one result that most deserves to be right.
- Phase 0 ran on 2026-08-13, but through `local-agent-benchmarks/hermes-diagnostic/`, not
  `bench/run_hermit_diagnostic.py` (which remains unrun and superseded). It produced a
  Hermes-only leaderboard rather than the harness comparison: there is no model overlap between
  the two harnesses, and the 64,000-context floor may make the matched run impossible as
  specified. So "is Hermes better or worse than OpenCode" is **still unanswered**, and this
  suite still does not answer it. See [../../ROADMAP.md](../../ROADMAP.md).
