# fsops results, sweep 2 — 2026-08-12

> ### ⚠ These scores are lower bounds
>
> Agents were later found writing correct output into the **Hermes-Cpp repo root**
> instead of `--in` — see [`results/evidence/escaped-to-repo-root/`](./results/evidence/escaped-to-repo-root/).
> `llama32-3b` scored 0/3 on `01_create_file` while having produced the exactly-correct
> file, in the wrong place. The penalty is uneven: models emitting absolute paths were
> unaffected, so part of what these tables rank as filesystem ability is really
> *does this model emit absolute paths*. Working trees now run outside the repo.
> Re-run before treating any per-task number as settled.


`cachyos-x8664` · RTX 5080 Laptop 16 GB · `num_ctx` 65536 · 3 repeats · model-default
sampling · transcripts off · **144 runs**

Three non-thinking models, chosen because every timeout in sweep 1 came from a thinking
model. `llama32-3b` re-ran as a **cross-sweep control**.

| task | llama32-3b | granite4-7b | hermes3-8b | llama31-8b |
|---|---|---|---|---|
| 01_create_file | 0/3 | 0/3 | 3/3 | 3/3 |
| 02_make_dirs | 2/3 | 0/3 | 0/3 | 2/3 |
| 03_move_file | 0/3 | 0/3 | 0/3 | 2/3 |
| 04_rename | 0/3 | 0/3 | 0/3 | 0/3 |
| 05_copy | 0/3 | 1/3 | 0/3 | 0/3 |
| 06_selective_delete | 0/3 | 1/3 | 0/3 | 0/3 |
| 07_run_script | 3/3 | 3/3 | 0/3 | 2/3 |
| 08_write_and_run_script | 0/3 | 0/3 | 0/3 | 0/3 |
| 09_list_report | 2/3 | 1/3 | 0/3 | 0/3 |
| 10_bulk_move | 0/3 | 0/3 | 0/3 | 0/3 |
| 11_append_preserve | 0/3 | 0/3 | 0/3 | 0/3 |
| 12_multi_step | 0/3 | 1/3 | 0/3 | 1/3 |
| **total** | **7/36** | **7/36** | **3/36** | **10/36** |

## Combined standings, both sweeps (strict, denominator 36)

| model | score | median run | thinking |
|---|---|---|---|
| **qwen3.5:9b** | **27/36 (75%)** | 64.6s | yes |
| qwen3.5:4b | 26/36 (72%) | 106.9s | yes |
| llama3.1:8b-q8 | 10/36 (28%) | ~17s | no |
| llama3.2:3b-fp16 | 9/36, 7/36 | ~5s | no |
| ibm/granite4:7b-a1b-h | 7/36 (19%) | ~6s | no |
| hermes3:8b-llama3.1-q8 | 3/36 (8%) | ~10s | no |

## 1. Collateral damage is real. This corrects sweep 1.

Sweep 1 recorded zero collateral damage and concluded these models "fail by inaction and
partial work, not by destruction," and that the sweep was not an argument for
backup-before-mutate. **That was true of sweep 1's model set and does not generalise.**

Two runs on `05_copy` destroyed the original file and never produced the copy:

| model | `config.ini` after |
|---|---|
| *original* | `[main]` / `mode = production` / `retries = 3` |
| `hermes3-8b` r2 | `(unchanged)` — 11 bytes; it wrote its own status word into the file |
| `llama31-8b` r3 | `-- copied from source --` / `# config file for original application` / `[config]` / `echo = false` — invented content |

Worse than deletion, because a missing file is obvious and a plausible-looking config is not.

**The assertion `original config.ini still exists` PASSED in both cases.** Existence is not
integrity. Only the SHA-256 before/after comparison caught this; a harness that checked for
file presence would have scored both as partial successes. Guardrails are not optional, and
this is now observed rather than inherited from the tournament.

## 2. Thinking is what makes these models work — and it is expensive

The non-thinking models were **4–15x faster** (6–17s vs 65–107s), which confirms reasoning
burn as the cause of sweep 1's slowness. But not one of them came close on accuracy: the best
non-thinking model scored **10/36** against qwen-9b's **27/36**.

This inverts the recommendation that led to sweep 2. Non-thinking models were picked to avoid
timeouts; the data says the reasoning is doing the work. There is no fast, accurate, small,
non-thinking option in this field — the choice is slow-and-right or fast-and-wrong.

## 3. The Nous fine-tune scored worse than the base model it is built on

`hermes3:8b` and `llama3.1:8b` were run at 8.0B / Q8_0 / 131072 ctx — same base model, same
size, same quantization. The only variable is the fine-tune.

**`llama3.1:8b` 10/36 · `hermes3:8b` 3/36.**

At n=36 with the variance measured below, a 7-run gap is suggestive rather than conclusive,
but the direction is clear and it is not the expected one. Worth re-running before it is
treated as settled.

## 4. The control moved more than any model comparison

`llama32-3b` scored **3/3 on `01_create_file` in sweep 1 and 0/3 in sweep 2** — the simplest
task in the suite, identical configuration, same machine, same night.

What it did in sweep 2: r1 created the file containing `<br>HERMES-OK`; r2 and r3 created
nothing and replied `DONE`.

Two consequences:

**A 3/3 at n=3 means very little.** If a model's true success rate is 50%, three consecutive
passes happen 12.5% of the time. Sweep 1's claim that `01_create_file` was "rock solid" was an
artifact of three samples. **Per-cell numbers in these tables should not be read as capability.**
Only the model totals (n=36) carry signal, and even those wobble by ±2.

**Cross-sweep comparison is weaker than it looks.** The control's own total moved 9/36 → 7/36.
Any model-to-model gap smaller than ~3 runs is noise.

## 5. Models write rendering artifacts into file content

Two instances, two families:

- `qwen3.5:4b` wrote `5|2026-08-12 shipped` into `tally.txt` — a line-number-style prefix.
- `llama3.2:3b` wrote `<br>HERMES-OK` into `hello.txt` — an HTML line break.

Confirmed **not** to be Hermes rendering: the wire transcript shows tool results arriving as
raw JSON with no prefixes anywhere. This is model-intrinsic content contamination, so it must
be designed *around*: the write path needs read-back verification, and content that must match
exactly cannot be trusted to a single unverified write.

## What this means for Hermes-Cpp

1. **Backup-before-mutate is mandatory**, on the evidence of §1, not by analogy.
2. **Verify by content hash, never by existence** (§1).
3. **`qwen3.5:9b` is the model to build against** — nothing else is close, and its cost is
   wall-clock, which a supervisor can absorb by bounding sessions.
4. **Every write needs read-back verification** (§5).
5. **Retry-with-verification is the whole product.** 39% of model×task cells in sweep 1 were
   unstable. A task succeeding ~67% per attempt, retried three times with state checks
   between, approaches 96%. The instability is the justification for the architecture.

## Limitations

- Transcripts off; `tool_calls` and `reasoning_chars` are empty for both sweeps.
- `gemma-e4b` still untested — its base was removed from Ollama before it ran.
- Mixed quantization across the field (Q8_0, Q4_K_M, F16). The hermes3/llama3.1 pair is
  matched; cross-family comparisons are not.
- 3 repeats is the documented minimum and is demonstrably too few for per-task claims (§4).
- No `--deterministic` pass yet, so sampling noise and capability are not separated.
