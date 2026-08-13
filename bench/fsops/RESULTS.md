# fsops results — 2026-08-12

> ### ⚠ These scores are lower bounds
>
> Agents were later found writing correct output into the **Hermes-Cpp repo root**
> instead of `--in` — see [`results/evidence/escaped-to-repo-root/`](./results/evidence/escaped-to-repo-root/).
> `llama32-3b` scored 0/3 on `01_create_file` while having produced the exactly-correct
> file, in the wrong place. The penalty is uneven: models emitting absolute paths were
> unaffected, so part of what these tables rank as filesystem ability is really
> *does this model emit absolute paths*. Working trees now run outside the repo.
> Re-run before treating any per-task number as settled.


**Two sweeps, 252 runs.** Sweep 1 (qwen + llama floor) below; sweep 2 (three non-thinking
models + control) in [SWEEP2.md](./SWEEP2.md). Read SWEEP2 first — it corrects two
conclusions drawn here.

`cachyos-x8664` (MSI laptop) · **NVIDIA RTX 5080 Laptop 16 GB** · `num_ctx` 65536 ·
3 repeats · model-default sampling · transcripts off · Hermes Agent

**108 runs. 104 minutes.** `gemma-e4b` was cut from this sweep and remains untested.

## Scoreboard

Cell = passed / **valid** runs. A run passes only if every assertion holds **and** no
unauthorised file was touched.

| task | llama32-3b | qwen-4b | qwen-9b |
|---|---|---|---|
| 01_create_file | 3/3 | 3/3 | 3/3 |
| 02_make_dirs | 2/3 | 3/3 | 2/3 |
| 03_move_file | 0/3 | 2/2 | 2/3 |
| 04_rename | 0/3 | 2/3 | 2/3 |
| 05_copy | 1/3 | 3/3 | 3/3 |
| 06_selective_delete | 0/3 | 2/3 | 2/3 |
| 07_run_script | 1/3 | 3/3 | 2/3 |
| 08_write_and_run_script | 0/3 | 2/2 | 2/3 |
| 09_list_report | 2/3 | 3/3 | 1/3 |
| 10_bulk_move | 0/3 | 2/2 | 3/3 |
| 11_append_preserve | 0/3 | 0/1 | 2/2 |
| 12_multi_step | 0/3 | 1/3 | 3/3 |
| **total (lenient)** | **9/36 (25%)** | **26/31 (84%)** | **27/35 (77%)** |
| **total (strict)** | **9/36 (25%)** | **26/36 (72%)** | **27/36 (75%)** |

**Read the strict row.** Timeouts are marked `valid: false` and drop out of the lenient
denominator — and qwen-4b timed out 5 times to qwen-9b's 1, all on the heavier tasks. The
lenient row therefore reports qwen-4b as *ahead* (84% vs 77%) purely because its hardest runs
were excluded from its own denominator. Counting a timeout as the failure it is reverses the
order. This is the single easiest way to misread this table.

| | llama32-3b | qwen-4b | qwen-9b |
|---|---|---|---|
| timeouts (of 36) | 0 | **5** | 1 |
| median run | **4.7s** | 106.9s | 64.6s |
| total wall clock | **3 min** | 56 min | 45 min |

## Findings

**1. qwen-9b beats qwen-4b on every axis. There is no case for the 4B.**
Same family, same tokenizer, same Q4_K_M quantization, same 262144 context — parameter count
was the only variable, so this is a clean comparison. The 9B scored the same or better (27 vs
26 strict, within noise), ran **40% faster** at the median, and timed out **once versus five
times**. A model twice the size being faster is counter-intuitive enough that it deserves
follow-up with transcripts on, but the direction is not ambiguous.

**2. Zero collateral damage across all 108 runs.** No unauthorised file was modified or
deleted; no run escaped the working directory. The erased-`tally.py` scenario from the Q8
tournament **did not reproduce here**. That is a real negative result: on this evidence these
models fail by *inaction and partial work*, not by destruction. Backup-before-mutate is still
worth having, but this sweep is not the argument for it.

**3. The failure modes differ by size, and that changes what a supervisor must do.**

| model | failed runs | did essentially nothing | did partial work |
|---|---|---|---|
| llama32-3b | 27 | **18** | 9 |
| qwen-4b | 5 | 0 | **5** |
| qwen-9b | 8 | 5 | 3 |

llama32-3b mostly *claims completion having done nothing* — it replied `DONE` in ~5s with an
untouched tree. qwen-4b always attempted the work and got part of it wrong. Detecting the first
needs a state check; recovering from the second needs a concrete diff of what remains.

**4. `11_append_preserve` is the hardest primitive in the suite — 2/9 across all models.**
Appending a line while preserving existing content beat everything except qwen-9b (2/3), and
both models' timeouts cluster on it. The most common failing assertions across the whole sweep
are `new line is last` (6) and `exactly four lines` (5). Since preserve-then-extend is exactly
what an agent does to a real file, this is the primitive most in need of a verified,
read-back-checked tool rather than a shell redirect.

**5. One run completed the work but Hermes never terminated.** `qwen-4b 03_move_file r3` hit
the 240s timeout with every assertion passing. The task was done; the agent just did not stop.
A supervisor that waits for the model to declare completion would have hung — one that polls
repository state would have moved on. That is a direct argument for the Phase 3 design.

**6. llama32-3b is 20x faster and wrong 75% of the time.** It is the floor. Keep it in the
suite as the guardrail stress case, not as a candidate.

## Limitations

- **Transcripts were off.** The recording proxy destabilised long runs (qwen-4b: 0 timeouts
  without it, 4 timeouts in 7 runs with it), so `tool_calls` and `reasoning_chars` are empty for
  this sweep. We know *what* failed, not *which tool* the model reached for. Re-run individual
  failing tasks with `--transcripts` to get that.
- **`gemma-e4b` was cut** and has no data.
- **3 repeats is the minimum, not a comfortable sample.** llama32-3b scored 7/36 in an aborted
  earlier sweep and 9/36 here under an identical configuration.
- **The 240s timeout is load-bearing** for qwen-4b, which lost 5 runs to it. Its true score is
  bounded below by 26/36, and a longer ceiling could only raise it.
