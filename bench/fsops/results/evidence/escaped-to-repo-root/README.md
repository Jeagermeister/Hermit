# Work that landed in the repo root instead of `--in`

Recovered 2026-08-13 from the Hermes-Cpp repository root after sweeps 1 and 2.

| artifact | task | what it actually shows |
|---|---|---|
| `hello.txt` — contains exactly `HERMES-OK` | `01_create_file` | the task done **correctly**, in the wrong directory |
| `project/src/utils`, `project/tests` | `02_make_dirs` | the task done **correctly**, in the wrong directory |
| `data/` — `first.txt`, `second.txt`, `text1.txt`, `text2.txt`, `other.json` | `08_write_and_run_script` | **fabricated**. The task ships `data/a.txt`, `b.txt`, `c.txt`, `notes.md`. Unable to find `data/`, the model invented one rather than reporting the problem. |

`project/src/utils/.hermes-tmp.HRRyCn` is a Hermes temp file, which confirms Hermes' own tool
wrote these — not a shell command the model composed by hand.

## Why this matters more than a cleanup

`llama32-3b` scored **0/3** on `01_create_file` in sweep 2 and was reported as having "replied
DONE and done nothing." At least one of those runs produced the exactly-correct file with the
exactly-correct content. It was scored zero because the harness looked in the working directory
and Hermes wrote somewhere else.

**Scores in RESULTS.md and SWEEP2.md are therefore lower bounds, not measurements**, and the
effect is not evenly distributed: `qwen3.5:4b` was observed issuing absolute paths and was
unaffected, while models emitting relative paths were penalised. Part of what those tables rank
as "filesystem ability" is really "does this model emit absolute paths."

That is a **tool-design artifact**, on the harness side of exactly the line Phase 0 exists to
draw — and it is fixable, which makes it a Hermes-Cpp requirement rather than a model limitation.

## The fix

Working trees moved out of the repository (`~/.cache/hermes-fsops/runs`, override with
`FSOPS_RUNS`), so a git-root fallback can no longer land inside tracked source. `WATCH_DIRS`
still includes the Hermes-Cpp root so the same escape is caught if it recurs.

**Still unconfirmed:** whether Hermes resolves an empty `workdir` to the enclosing git root
specifically. Run one shell task with `--transcripts` on an idle GPU and read the `workdir`
argument the model sends. Until that is done, "git root fallback" is the leading hypothesis,
not a finding.
