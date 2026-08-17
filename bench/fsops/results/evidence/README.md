# Evidence

**`count.sh`** — written by an agent at 17:34 on 2026-08-12, during the first sweep, into the
**Hermit repository root** rather than its own working directory. It is task
`08_write_and_run_script`'s artifact, and it is correct code: it counts `.txt` files under
`./data`. It simply ran somewhere it was never given.

This is an escape from `--in`, and the harness did not catch it. The escape canary sat one
level above the working tree; this landed four levels up. `WATCH_DIRS` in `run_fsops.py` now
covers the whole ancestor chain to the repo root, and `escaped_files` is recorded per run.

**Confirmed 2026-08-13 — and the mechanism was not the one guessed here.** The escapes came
through Hermes' **file tools**, not the shell: one escaped directory contained
`.hermes-tmp.HRRyCn`, which only a file tool writes. The `terminal` tool separately ignores
`--in` and follows the process working directory, confirmed by three probes. Both are recorded
as R1 in [REQUIREMENTS.md](../../../../REQUIREMENTS.md). The original hypothesis follows, kept
for the record: Hermes' `terminal` tool falls back to a project or git root when the model
leaves its `workdir` argument empty — a 2026-08-12 transcript caught
`llama3.2:3b` emitting exactly that, `"workdir": ""`. Models that used absolute paths
(`qwen3.5:4b` was observed doing so) worked correctly; models that used relative paths did not.
That would also explain `./check.sh: No such file or directory` in an early `07_run_script` run
where the script demonstrably existed and was executable.

This is a **tool-design artifact, not a model failure**, and it is a hard requirement for
Hermit: *every* tool's path resolution — file tools and shell alike — must be pinned to the
sandbox root, never inherited or inferred. Pinning only the shell would have missed the actual
escape route. Relative paths are what models actually emit.

Confirming it needs an idle GPU — run a single shell task with `--transcripts` and read the
`workdir` argument the model sends.
