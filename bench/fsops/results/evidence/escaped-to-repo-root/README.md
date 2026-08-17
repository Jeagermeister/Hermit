# Work that landed in the repo root instead of `--in`

Recovered 2026-08-13 from the Hermit repository root after sweeps 1 and 2.

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
draw — and it is fixable, which makes it a Hermit requirement rather than a model limitation.

## The fix

Working trees moved out of the repository (`~/.cache/hermit-fsops/runs`, override with
`FSOPS_RUNS`), so a git-root fallback can no longer land inside tracked source. `WATCH_DIRS`
still includes the Hermit root so the same escape is caught if it recurs.

**Still unconfirmed:** whether Hermes resolves an empty `workdir` to the enclosing git root
specifically. Run one shell task with `--transcripts` on an idle GPU and read the `workdir`
argument the model sends. Until that is done, "git root fallback" is the leading hypothesis,
not a finding.

---

## Mechanism, investigated 2026-08-13

Three probes, each launching Hermes from a different directory with `--in` pointed somewhere
else entirely, asking only for `pwd`:

| launched from | `--in` | `terminal` reported |
|---|---|---|
| `Hermes-Cpp/` | `/tmp/.../cwdtest` | `Hermes-Cpp/` |
| `Hermes-Cpp/bench/fsops/` | `/tmp/.../cwdtest` | `Hermes-Cpp/bench/fsops/` |
| `~/.cache/hermes-fsops/` | `/tmp/.../cwdtest` | `~/.cache/hermes-fsops/` |

**The `terminal` tool follows the process working directory and ignores `--in` entirely.** It is
not a git-root walk — probe 2 returned a subdirectory, not the repo root. The harness already
sets `cwd=work`, so terminal was never the escape route, and `07_run_script` scoring 3/3 for
two models fits that.

**The escapes therefore came from the file tools, not the shell.** `project/src/utils/` contained
`.hermes-tmp.HRRyCn`, a Hermes-authored temp file, so a file tool wrote it. Those apparently
resolve relative paths against something other than cwd — an enclosing git or project root is
the remaining hypothesis, consistent with every escaped artifact landing at the Hermit root
while the runs sat four levels below it.

**After moving run trees to `~/.cache/hermit-fsops/runs`, six runs of the two previously-escaping
tasks produced zero strays** and the repo root stayed clean. That is consistent with the
hypothesis but is six runs, not proof.

## What it did NOT explain

`llama32-3b` on `01_create_file`, same model and config, three separate sessions:

| session | runs in | score |
|---|---|---|
| sweep 1 | inside repo | **3/3** |
| sweep 2 | inside repo | **0/3** |
| 2026-08-13 | outside repo | **1/3** |

Pooled: **4/9**. With escapes eliminated the failures are genuine — one run wrote `hello.txt`
with wrong content, one created nothing at all. The escape was real and did cost some runs, but
**raw run-to-run variance is the larger effect**, and moving the trees did not lift the score.
