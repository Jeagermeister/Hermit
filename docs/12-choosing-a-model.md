# 12. Choosing a model

Hermit is model-agnostic in mechanism and extremely opinionated in evidence: every claim below
is a measured result on named hardware, and the numbers do not travel — they rank models
against each other on one machine, they do not promise you a percentage on yours. Choosing a
model appropriate to the machine is an operator responsibility by design
([ROUTING.md](../ROUTING.md) §9).

## The four gates

A model has to clear four gates before its scores mean anything. The first two are checked
live by `hermit preflight`; the other two are properties of a model's **chat template**, which
no model card reports. As of 2026-08-27 they are checked empirically, by hand — two live
chat probes per model, since on current Ollama `ollama show --template` prints a renderer
stub and can no longer answer the question. The measured mechanisms are in
[DECISIONS.md](../DECISIONS.md) § "Still open", and the probes themselves are committed in
the benchmark repository's evidence directory.

1. **Architectural context ≥ the floor** (default 65536). What no request can exceed is the
   context the model was built for — Modelfile pins neither help nor bind, since the client
   sets `num_ctx` per request ([D8](../DECISIONS.md)).
2. **The `tools` capability.** A model that cannot emit a tool call scores zero for a reason
   that says nothing about filesystem ability.
3. **Do the tool definitions survive a tool result?** On stock llama3.2/3.1 templates they
   vanish whenever the last message is a tool result — the exact turn where the model decides
   whether to call another tool. The model then works from memory of the schema, which
   degrades on tools it has not just seen. Template-specific, not architectural.
4. **Does your system prompt survive offering tools?** On `hermes3:8b` it does not — the
   template silently replaces it with its own function-calling boilerplate, so the
   instructions Hermit relies on never reach the model at all.

## The measured field

Filesystem-primitive suite (`fsops`, twelve tasks), sweep 3: 360 runs, five repeats, sampling
pinned, RTX 5080 Laptop 16 GB, 2026-08-26. Full tables and caveats in the
[benchmark repository](./30-benchmarks.md).

| model | params | suite score | median run | reading |
|---|---|---|---|---|
| `gemma4:e4b-it-qat` | E4B | **90%** | **9.4 s** | best in field, second fastest |
| `qwen3.5:9b` | 9.7B | 80% | 63.9 s | accurate, slow (thinking); the supervised baseline |
| `ibm/granite4:7b-a1b-h` | 7B-A1B | 45% | 3.8 s | fastest, not accurate enough alone |
| `llama3.1:8b-q8` | 8B | 33% | 10.9 s | |
| `llama3.2:3b-fp16` | 3.2B | 27% | 4.6 s | the deliberate floor / stress case |
| `hermes3:8b-llama3.1-q8` | 8B | 15% | 6.5 s | fails gate 4 — see below |

**The recommendation, stated at its real width.** `gemma4:e4b-it-qat` is the model the suite
says to build against — better accuracy than `qwen3.5:9b` at roughly a seventh of the wall
clock. But the *supervised* delta (Hermit versus a plain agent harness) has been measured with
`qwen3.5:9b`, where supervision took 74% → 94%; gemma-e4b has not yet been re-checked under
that protocol. So: `qwen3.5:9b` is the proven supervised choice, `gemma4:e4b-it-qat` is the
measured front-runner to try first — and if your results diverge, that divergence is data, not
a mistake.

## What size actually buys

The tiers have been measured separately, and the answer is not "bigger is better" — it is
"bigger moves the problem":

- **3B is the floor, kept on purpose.** `llama3.2:3b` fails in exactly the ways the
  supervisor exists to catch — it is the model that replied `DONE` over an untouched tree. Its
  scores are the guardrail test, not a verdict on the model.
- **4–9B is the working tier for filesystem primitives**, and where supervision earns most:
  per-attempt instability is high, and retry-with-one-concrete-failure is what converts it.
- **27B ceilings the primitive suite.** The E3 experiment ran the same tasks at 27B
  (`qwen3.8:27b`, 72 GB card): every cell at the ceiling — 105/105 runs across the arms,
  supervised or not. At that tier the primitives stop discriminating, and supervision costs
  nothing measurable while still providing containment, backup, and the audit trail.
- **Repository-scale work discriminates again at 30B.** E4/E5 ran four repo-scale tasks
  against a 30B coder and three general ~27–30B models: 55% baseline for the coder, one
  general model near the ceiling — and the interesting failures moved from "can't do the
  operation" to "silently wrong in a file nothing checked" — which is the judge's territory,
  not the tools'.

On memory, the only measured anchors: the entire small field (3B–9.7B, up to Q4/F16) ran at a
65536-token window on a **16 GB** laptop card, and `qwen3.8:27b-q8_0` measured **27.16 GiB**
resident on the 72 GB card. Between those anchors, size your quantization to your VRAM and let
`preflight` and a smoke run tell you the rest — an oversized context request is not gracefully
refused by Ollama, which is why Hermit clamps it ([D8](../DECISIONS.md), and see
[troubleshooting](./18-troubleshooting.md) before raising the clamp).

## Thinking models

The early sweeps read as "thinking is what makes these models work": thinking models were
4–15× slower and also the only accurate ones. Sweep 3 corrected the dichotomy — `gemma-e4b`
is accurate *and* fast — and also disclosed that no sweep has yet genuinely controlled the
reasoning level (the flag never reached the wire in the harness those sweeps drove). Treat
"thinking vs not" as an open cost question, not a settled law. What is settled: budget for it
with `--budget`, because a thinking model that exhausts its token budget mid-think returns
empty content with no error.

## Models to avoid, by name

- **`hermes3:8b`** — discards your system prompt whenever tools are offered (gate 4).
  Measured, with the template mechanism identified. Anything you attribute to a system prompt
  on this model is attributable to something else.
- **Stock `llama3.2` / `llama3.1` instruct tags** — lose their tool definitions after a tool
  result (gate 3). Usable, measurably degraded; prefer a family whose template keeps the
  definitions.
- **`phi3.5:3.8b`** — no `tools` capability at all.
- **`llama3-groq-tool-use:8b`** — 8192 architectural context; no setting can raise it.

## The judge model

`satisfies:` expectations are decided by a model ([chapter 14](./14-expectations.md)). The
default judge is the working model in a fresh session; `--judge-model` brings a different one.
The measured guidance: general models judging these criteria produced **zero false unmets in
120 runs** (and 70/70 verdict agreement at 27B), while a code-specialised 30B judge
confabulated unmet verdicts on 14 of 40 — the failure was the judge model, not the mechanism.
If your worker is a coder, bring a general judge.
