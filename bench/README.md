# Phase 0 — Hermes vs OpenCode diagnostic

> ## ⚠ Superseded — this harness has never been run
>
> Phase 0 was executed in **2026-08-12/13** by
> `~/Source/local-agent-benchmarks/hermes-diagnostic/`, which is the harness of record and far
> more rigorous than this one (preflight/postflight, three-way classification, controller
> locking, protected-baseline hashing). See [../ROADMAP.md](../ROADMAP.md) for what it found.
>
> **The matched comparison this file describes cannot run as written.** Hermes refuses models
> under 64,000 context (`MINIMUM_CONTEXT_LENGTH`), and the tournament tags below are pinned at
> `num_ctx 32768`. Rebuilding them at 64K makes them no longer the configuration OpenCode
> measured. Read the rest of this file as a design record, not as instructions.

Runs the **six local-agent diagnostic stages under Hermes Agent**, so they can be compared
against the same stages run under OpenCode in `~/Source/local-agent-benchmarks/integration-diagnostic`.

## The question this answers

Some findings from the OpenCode run were suspected to be **tool-design artifacts, not model
behaviour**. Both remain **untested** — E4B was never run under Hermes — though Phase 0 did
show Hermes' own `read_file` decorating content, which is the same hazard by another route:

- *"E4B treats rendered end-of-file annotations as literal content during exact edits"* — that
  is OpenCode's file rendering, not Gemma.
- *"Prefer patch/write tools or line-based edits for very short files"* — that is OpenCode's
  edit tool design.

The distinction matters enormously for Hermes-Cpp: a **model-intrinsic** failure is one you must
design *around*, while a **harness** failure is one you can design *away*. Running identical
stages through a second harness is the cleanest control available.

**The output that matters is the delta**, not the score. Where the same model behaves
differently under two harnesses, the cause is tool design — and that becomes a requirements list.

## Running it

```bash
./run_hermes_diagnostic.py --models qwen-9b gemma-12b gemma-e4b --repeats 3
./run_hermes_diagnostic.py --models gemma-12b --stages 01_read --repeats 1 --tag <installed>  # smoke
```

Results land in `results/`, logs in `logs/`, agent working trees in `runs/` (gitignored).

## Three constraints that make or break the comparison

**1. Run it on `kitchen-desktop`.** The OpenCode baseline came from there. Running this on the
MSI laptop would confound *harness* differences with *GPU* differences — two variables, one
measurement. Every result file records `os.uname().nodename` so this cannot be lost later.

**2. Three repeats minimum.** The existing data has identical configurations scoring **6/6 and
4/6**, and one Q8 run that erased `tally.py`. At that variance, single runs are noise. The script
warns below three.

**3. The stages are byte-identical, on purpose.** `stages.py` was extracted verbatim from
`integration-diagnostic/scripts/run_all.py`. **Do not edit it.** Any change turns a
controlled comparison into two unrelated experiments.

## Prerequisites

- **Hermes Agent installed on the machine running this** (`~/.local/bin/hermes`). Installed on
  both the MSI laptop and `kitchen-desktop` (v0.20.0) as of 2026-08-13.
- **The `num_ctx`-pinned Ollama variants** the OpenCode run used, built from
  `tournament/models/*.Modelfile`:
  ```bash
  ollama create tournament-qwen-9b:32k   -f ~/Source/local-agent-benchmarks/tournament/models/qwen-9b-32k.Modelfile
  ollama create tournament-gemma-12b:32k -f ~/Source/local-agent-benchmarks/tournament/models/gemma-12b-32k.Modelfile
  ollama create tournament-gemma-e4b:32k -f ~/Source/local-agent-benchmarks/tournament/models/gemma-e4b-32k.Modelfile
  ```
  The script preflights these and refuses to start if any are missing — a 54-run job should not
  fail on run 1. **Note:** these 32k tags are below Hermes' own 64,000 floor, so Hermes will
  refuse them even when the preflight passes. See the banner at the top.

## A trap this harness handles

**Hermes exits 0 even when the API call fails outright.** A run that never reached the model
would otherwise be scored as a legitimate result, which is worse than a crash because it looks
like data. Every record carries `valid` and `harness_error`; invalid runs are not scored, and the
summary refuses to let a partial dataset pass quietly.

Verified in both directions on 2026-08-12: a working model scores `valid: true` with the marker
detected, and a bogus tag scores `valid: false` with no scoring attempted.

## Sampling, determinism, and why the variance exists

**The tournament Modelfiles pin only `num_ctx`.** No temperature, no seed, no sampling
parameters anywhere in the original harness. Every run therefore samples stochastically at
model defaults with no fixed seed — which is a complete explanation for the 6/6-versus-4/6
result on identical inputs. That variance is not mysterious, and not a quantization effect:
**nothing was pinning the dice.**

Two modes, and the choice matters:

| Mode | Use for | Why |
|---|---|---|
| **Default** (unpinned) | **Phase 0** — Hermes vs OpenCode | The OpenCode baseline ran at model-default sampling. Matching it is what makes the comparison like-for-like. Handle the noise with repeats. |
| `--deterministic` | **GPU A/B**, or isolating any backend effect | Pins `temperature 0` and a fixed `seed`, so a difference is attributable to the thing you changed rather than to sampling. |

```bash
./run_hermes_diagnostic.py --models qwen-9b --repeats 3                    # Phase 0
./run_hermes_diagnostic.py --models qwen-9b --repeats 3 --deterministic    # GPU A/B
```

`--deterministic` builds a `<model>-det:<seed>` variant with `temperature 0` and `seed` pinned,
then runs against that. It has to work this way: **there are no `OLLAMA_TEMPERATURE` or
`OLLAMA_SEED` environment variables** — sampling is per-request or per-Modelfile, so setting
env vars would be silently ignored and produce *fake* determinism.

## Record the GPU

Set `BENCH_GPU` so every result file names the card it ran on:

```bash
BENCH_GPU="Radeon Pro W7900 48GB" ./run_hermes_diagnostic.py --models qwen-9b --repeats 3
```

Kitchen's card changes on **2026-08-14** (W7900 48 GB ROCm → RTX PRO 5000 72 GB CUDA). Results
without a GPU field cannot be safely compared across that date, and the field defaults to a
loud `UNRECORDED` rather than to a guess.

## Scoring

Mirrors the OpenCode harness:

| Stage | Scored by |
|---|---|
| `01_read` | marker string echoed in output |
| `02_edit` | `settings.py` contains `fixed` |
| `03_shell` – `06_two_file` | `pytest -q` in the working tree |

Comparison against the OpenCode baseline is deliberately left manual for now — the point is to
*look* at where behaviour diverges, not to reduce it to a single number too early.
