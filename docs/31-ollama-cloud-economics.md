# 31. Ollama Cloud model economics

This chapter is a different evidence tier from [chapter 12](./12-choosing-a-model.md). That
doc's rankings are measured on Hermit's own `fsops` suite, on named hardware. Nothing below is
measured by Hermit — it's pricing pulled from `ollama.com/pricing` and capability claims pulled
from vendor pages, leaderboards, and press coverage, gathered 2026-09-02. Treat it as a shortlist
to test, not a verdict. Before routing real Hermit work to any model named here, it still has to
clear the [four gates in chapter 12](./12-choosing-a-model.md#the-four-gates) — none of the
benchmarks below test context-floor behavior, tool-definition survival after a tool result, or
system-prompt survival with tools offered. Those are chat-template properties no vendor
benchmark reports.

## Rate card (as published, 2026-09-02)

| Model | Input $/M | Cached input $/M | Output $/M |
|---|---:|---:|---:|
| nemotron-3-super | 0.015 | 0.015 | 0.60 |
| gpt-oss:20b | 0.07 | 0.035 | 0.30 |
| nemotron-3-nano | 0.06 | 0.06 | 0.24 |
| gemma4 | 0.14 | 0.05 | 0.40 |
| gpt-oss:120b | 0.15 | 0.014 | 0.60 |
| glm-5.3-flash | 0.15 | 0.03 | 0.50 |
| nemotron-3-ultra | 0.10 | 0.10 | 3.00 |
| minimax-m2.7 | 0.30 | 0.06 | 1.20 |
| deepseek-v4-flash | 0.44 | 0.014 | 1.32 |
| mistral-large-3 | 0.50 | 0.50 | 1.50 |
| minimax-m3 | 0.60 | 0.12 | 2.40 |
| qwen3.5:397b | 0.60 | 0.60 | 3.60 |
| kimi-k2.6 | 0.95 | 0.16 | 4.00 |
| kimi-k2.7-code | 0.95 | 0.19 | 4.00 |
| glm-5.1 | 1.00 | 0.20 | 3.20 |
| deepseek-v4-pro | 1.32 | 0.044 | 3.96 |
| glm-5.3 | 1.40 | 0.26 | 4.40 |
| glm-5.2 | 1.40 | 0.26 | 4.40 |
| kimi-k3 | 3.00 | 0.30 | 15.00 |

**The cached-input column is not reliably realized on Ollama Cloud today.** Local Ollama does
automatic KV-prefix caching if the model stays loaded, but a user directly benchmarked Cloud and
reported the expected savings aren't there ([ollama/ollama#16714](https://github.com/ollama/ollama/issues/16714),
open, feature-request, unresolved as of the issue's last activity). Budget against the **input**
column, not the cached one, until that issue closes. Re-check it before trusting a cache-based
estimate again.

## Ranked least → most expensive, at real workload volume

Priced against a real month of agentic-coding volume (Claude Code transcripts, 2026-08-02 to
2026-09-01: 80.8M fresh-input-equivalent tokens, 3827.9M context-reprocessing tokens, 22.4M
output tokens), at each model's **full input rate — no caching credit assumed**, since Cloud
caching isn't reliably available (see above). This is the honest number, not the optimistic one.

| Rank | Model | Est. $/mo at this volume | Clears $300? |
|---:|---|---:|---|
| 1 | nemotron-3-super | $72 | Yes — 4x headroom |
| 2 | nemotron-3-nano | $240 | Yes — thin margin |
| 3 | gpt-oss:20b | $280 | Yes — thin margin |
| — | **— $300/mo budget line —** | | |
| 4 | nemotron-3-ultra | $458 | No |
| 5 | gemma4 | $556 | No |
| 6 | glm-5.3-flash | $598 | No |
| 7 | gpt-oss:120b | $600 | No |
| 8 | minimax-m2.7 | $1,200 | No |
| 9 | deepseek-v4-flash | $1,749 | No |
| 10 | mistral-large-3 | $1,988 | No |
| 11 | minimax-m3 | $2,399 | No |
| 12 | qwen3.5:397b | $2,426 | No |
| 13 | kimi-k2.6 | $3,803 | No |
| 13 | kimi-k2.7-code | $3,803 | No |
| 15 | glm-5.1 | $3,980 | No |
| 16 | deepseek-v4-pro | $5,248 | No |
| 17 | glm-5.3 | $5,571 | No |
| 18 | kimi-k3 | $12,062 | No |

**Only three models clear $300/mo at this volume without cutting the volume itself**, and two of
those three have no verified coding/agentic benchmark (see below). This is the real argument for
pointing Hermit's D17 compaction at a Cloud backend before picking a model: compaction shrinks
what gets sent per turn, which is the only lever that actually works given Cloud doesn't
reliably discount repeats. Fix the volume first; the model list gets much more interesting once
it's not paying full input price on a resent-every-turn history.

## Use X for Y

| Situation | Model | Why | Evidence |
|---|---|---|---|
| Default for a cheap, high-volume tool-use loop | `gpt-oss:120b` | Matches/exceeds o4-mini on coding + TauBench tool calling; fits a single H100 server-side | [OpenAI model card](https://openai.com/index/introducing-gpt-oss/) |
| Same, if `gpt-oss:120b` still exceeds budget after compaction | `deepseek-v4-flash` | Terminal-Bench 2.1 82.7, DeepSWE 54.4, native tool calling, **ships an Anthropic-compatible endpoint** — worth checking whether that lets a Claude-style client point at it with less integration work | [MarkTechPost, 2026-07-31](https://www.marktechpost.com/2026/07/31/deepseek-upgrades-deepseek-v4-flash-0731-with-major-agentic-and-coding-gains/) |
| Cheapest usable option, light/simple tasks | `gemma4` | 86.4% agentic tool use, 80% competitive coding at 31B — real benchmarks, not just a low price tag | [startuphub.ai review](https://www.startuphub.ai/ai-news/ai-research/2026/google-gemma-4-review-2026) |
| Absolute cheapest in the table | `nemotron-3-super` | $0.015/$0.015/$0.60 flat — but no coding/agentic benchmark surfaced in this pass. Treat as unverified; run the four gates before trusting it with real work | search turned up architecture specs only, no task benchmarks |
| Token-efficient long agentic loops (fewer reasoning tokens burned per turn — matters most when nothing is discounted) | `kimi-k2.7-code` | Reports 30% fewer reasoning tokens per loop vs. K2.6, purpose-built for this | Moonshot's own benchmarks only — [MarkTechPost](https://www.marktechpost.com/2026/06/12/moonshot-ai-releases-kimi-k2-7-code-a-coding-model-reporting-21-8-on-kimi-code-bench-v2-over-k2-6/) explicitly notes no independent numbers yet |
| Long-context or multimodal work (big repos, images in the loop) | `minimax-m3` | 1M context, native multimodal, SWE-Bench Pro 59.0%, first open-weight to combine all three | [MiniMax model page](https://www.minimax.io/models/text/m3) |
| Frontier capability, budget not the constraint (a specific hard problem, not routine loop traffic) | `kimi-k3` or `deepseek-v4-pro` | k3: #1 Frontend Code Arena (1679 Elo, ahead of GLM-5.2); v4-pro: matches GPT-5.5/Opus 4.7 on agentic benchmarks | [Northflank](https://northflank.com/blog/what-is-kimi-k3-self-hosting), [MindStudio](https://www.mindstudio.ai/blog/deepseek-v4-open-source-frontier-model-review) — use manually, not as a default loop driver at these prices |
| Broad general reasoning / multilingual, not agentic-coding-specific | `mistral-large-3` | Strong general benchmarks, 100+ languages — but flat (no) cache discount and no coding/agentic numbers surfaced, so it's a weaker fit for a loop-shaped workload specifically | [vals.ai](https://www.vals.ai/models/mistralai_mistral-large-2512) |
| Lightweight/latency-sensitive sub-calls inside a larger pipeline (classification, routing, not the main loop) | `gpt-oss:20b` | Similar to o3-mini, edge-deployable, cheapest OpenAI tier | [OpenAI model card](https://openai.com/index/introducing-gpt-oss/) |

## Open question this doc doesn't answer

Whether any of these models' chat templates preserve tool definitions after a tool result, or
preserve the system prompt once tools are offered — the two template gates in chapter 12 that
disqualified models that looked fine on paper before ([hermes-agent-model-constraints]). Nothing
in this pass checked that; it has to be probed by hand per model, same as the local field was.
