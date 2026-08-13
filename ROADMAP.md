# Roadmap

Scope, sequencing, and the things that must be settled before code is worth writing.
Settled decisions have moved to [DECISIONS.md](./DECISIONS.md); the first code landed 2026-08-13.

---

## What this is

A **supervisor for local models doing filesystem work** — not a chatbot, and not a port of
upstream Hermes. It drives Ollama models through bounded sessions, verifies what they actually
did between turns, and re-invokes them with one concrete remaining failure.

That architecture is not a preference. It is what the local-model tournaments concluded.

---

## The evidence this is built on

From `integration-diagnostic/results/RECOMMENDATIONS.md`, run on **`kitchen-desktop`**
through **OpenCode**:

| Finding | Consequence for this project |
|---|---|
| Qwen 9B: **6/6** functional stages; best bounded implementation worker | Primary target model |
| Gemma 12B failed only by selecting a **similarly named test file** | Tools must make targets unambiguous |
| E4B treated **rendered end-of-file annotations as literal content** | File rendering is a tool-design hazard |
| *"Use an external supervisor that checks repository state and reinvokes"* | This is the product |
| *"Break larger work into fresh sessions"* | Startup cost compounds — hence a native binary |
| Historical Q4 **6/6**, matched rerun **4/6**, identical inputs | Run-to-run variance is large |
| Q8 run **erased `tally.py`** | Destructive failure is real; guardrails are not optional |

**Two of these are probably harness artifacts, not model behaviour** — the EOF-annotation
failure and the short-file edit guidance both smell like OpenCode's tool design rather than
anything intrinsic to the model. Separating those is the point of Phase 0.

---

## Phase 0 — Establish which findings actually transfer

> ### ✅ The deadline was met — the runs are on the W7900
>
> An **RTX PRO 5000 (72 GB, CUDA)** replaces Kitchen's **W7900 (48 GB, ROCm)** on
> **2026-08-14**. All 144 diagnostic runs are stamped `kitchen-desktop` / W7900 and dated
> 2026-08-12/13, so they sit on the same hardware as the OpenCode results. **The hardware
> confound is closed.** Anything re-run from here on lands on the new card and is not
> comparable to either.

**Question:** which OpenCode findings are model-intrinsic, and which are tool-design artifacts?
A finding you must design *around* is very different from one you can design *away*.

**Harness of record:** `local-agent-benchmarks/hermes-diagnostic/`. That is what produced the
data — preflight/postflight, three-way classification, controller locking, protected-baseline
hashing. `bench/run_hermes_diagnostic.py` in *this* repo was superseded and has never been run;
don't reach for it.

### Status: ran, and produced a leaderboard rather than the delta

- [x] **Install Hermes Agent on `kitchen-desktop`** — done, v0.20.0.
- [x] **Run on `kitchen-desktop`, not the laptop** — done; every result records the hostname
      and the W7900.
- [x] **Three repeats minimum per model per stage** — done: 8 models × 6 stages × 3 trials =
      **144 runs**, no gaps. 125/144 passed.
- [x] **Build the `num_ctx`-pinned Ollama variants** — done for the *current* cohort. **Not**
      for the tournament tags; see the blocker below.
- [ ] **Re-pull `qwen3.5:9b`** — still not installed on `kitchen-desktop`.
- [ ] **Record the delta, not the score.** ❌ **Not produced.** There is zero model overlap
      between the harnesses: OpenCode ran the `tournament-*:32k` tags, Hermes ran a different
      8-model cohort. `--suite matched` was built and validated but never run.

> ### ⚠ The matched comparison may be infeasible as specified
>
> Hermes hard-refuses any model reporting under 64K context — `MINIMUM_CONTEXT_LENGTH = 64_000`
> in `agent/model_metadata.py`; `bench/fsops/run_fsops.py` enforces its own 65536 floor on top.
> The
> OpenCode tournament tags are pinned at **`num_ctx 32768`**, and `hermes-diagnostic`'s matched
> preflight requires that same 32768. **So `--suite matched` cannot start** — every model that
> did run was at 65536 or above.
>
> Rebuilding the tournament Modelfiles at 64K+ makes them no longer the configuration OpenCode
> measured, so the clean like-for-like comparison may simply be unavailable rather than merely
> pending. Decide deliberately: re-run *both* harnesses at 64K on the new card, or accept that
> the harness delta is unobtainable and rely on the findings below.
>
> Note also that stage `05_recovery` is not like-for-like even in principle — Hermes' `patch`
> fuzzy-matches, so the OpenCode prompt could never fail there and the harness substitutes an
> absent sentinel. A matched run yields 5 comparable stages, not 6.

**What Phase 0 did establish:**

- **Confirmed model-intrinsic:** models skip a prescribed diagnostic sequence and solve
  directly — reproduced under Hermes in 4 runs across 3 models, having also appeared under
  OpenCode. Harness-independent, and exactly what R6/R7 exist for.
- **Confirmed:** run-to-run variance is large. 6 of 8 models were not perfect across three
  identical trials; `qwen35` went 6/6, 6/6, 4/6. The three-repeats rule is vindicated.
- **New, and it moved a requirement:** Hermes' own `read_file` decorates content with `N|`
  line-number prefixes (226 of 240 calls) plus a phantom trailing marker, and a model echoed
  that decoration into its answer and failed the stage. This falsifies the evidence stated for
  **R5** — see [REQUIREMENTS.md](./REQUIREMENTS.md).
- **Still untested:** the E4B rendered-annotation finding and the short-file edit guidance —
  the two the phase existed to adjudicate. E4B was never run under Hermes. The `read_file`
  finding above answers the *same question* by a different route, but not for that model.

> Phase 1 does not depend on any of this and can run in parallel.

---

## Phase 1 — Foundations (no blockers)

- [x] **Sandbox root and path resolution (R1)** — `src/hermes/core/sandbox.{h,cpp}`, 42 tests.
      `SandboxPath` is constructible only by `Sandbox::resolve`, so any code taking one is
      R1-correct by construction. Resolution is POSIX-order (components walked, symlinks
      expanded as met), which is what makes `..` after a symlink mean what the OS means.
- [ ] **Model preflight (R9)** — context window and `tools` capability checked before the first
      request; fail loudly at startup. `hermes-diagnostic`'s 10-check live preflight is a
      working reference implementation worth reading.
- [ ] Ollama client over the OpenAI-compatible endpoint (`/v1/chat/completions`).
      **Verified working 2026-08-12:** `gemma4:12b` emits clean structured `tool_calls`.
- [ ] JSON handling
- [ ] Config + CLI entry point
- [ ] Session/history model
- [ ] **Wall-clock budgets (R8)** — per turn and per session, a timeout recorded as a failure
      rather than dropped from the denominator. Phase 0 strengthens this considerably: 11 of 19
      failures were 300 s timeouts from a single model, and `nemotron35-lightning` burned
      3,826 s against `gemma26-a4b-q8`'s 642 s.

### Decisions to settle first

These are hard to reverse and benefit from being argued out before code exists:

- [x] **Concurrency model** — blocking and single-threaded ([D1](./DECISIONS.md))
- [x] **HTTP library** — cpp-httplib, pinned ([D7](./DECISIONS.md)). Provisional until the
      Ollama client exists; loopback only, so no TLS and streaming optional.
- [x] **Dependency posture** — FetchContent, pinned ([D3](./DECISIONS.md)). Settled alongside
      these, though it was not on the original list.
- [x] **Sandbox as a capability type** ([D6](./DECISIONS.md)) — decided *during* implementation
      rather than before it, and revised once when review caught the resolution order.
- [x] **JSON library** — nlohmann, pinned v3.12.0 ([D2](./DECISIONS.md))
- [x] **Tool interface shape** — virtual dispatch for dispatch, a declarative `Args` struct for
      schema and parsing ([D4](./DECISIONS.md)). No static reflection exists on this toolchain,
      which is what ruled out full compile-time generation.
- [x] **Constrained decoding** — on from the start ([D5](./DECISIONS.md)). Fixes malformed tool
      calls, not wrong ones; the supervisor handles the rest.

---

## Phase 2 — Core loop and minimal tools

- [ ] Agent loop: history, tool dispatch, bounded turns
- [ ] `read`, `write`, `list` — enough to prove the loop end to end
- [ ] `edit` — hardest to get right; patch application is where harnesses usually fail
- [ ] **Read-back after every write (R5)** — attaches here, to `write` and `edit`: any write
      whose content must match exactly is read back and compared before the turn succeeds.
      Cheap insurance regardless of whether the contamination is the tool's or the model's,
      which is exactly why the requirement outlived the correction to its evidence.
- [ ] `move`, `search`

---

## Phase 2.5 — Frontends: human and machine

Settled in [D7](./DECISIONS.md): local inference only, driven both by a person and by a larger
model calling this as a tool.

- [ ] **Close the TOCTOU race before the programmatic frontend ships.** `openat(O_NOFOLLOW)`,
      one component at a time. D6 accepted the race against a "confused 3B model" threat model;
      a callable frontend changes that model, so this is a gate rather than cleanup.
- [ ] **Decide the hardlink answer** — device/inode comparison against the root, or accept it
      explicitly. No path-based check can catch it.
- [ ] **MCP server over stdio.** No listener, no port, no auth. Thin: transport only, over the
      same core the CLI drives.

## Phase 3 — The supervisor (the actual product)

- [ ] **State verification.** After each turn, check what the model *claims* against what the
      filesystem *shows*.
- [ ] **Re-invocation** with one concrete remaining failure, per the tournament recommendation.
- [ ] **Guardrails** — dry-run, backup-before-mutate, undo. The erased `tally.py` is the argument.
- [ ] **Bounded sessions** — fresh session per unit of work rather than one long autonomous run.

---

## Explicitly out of scope

Tracked in `parity.tsv` as `OUT_OF_SCOPE` so upstream drift there is ignored rather than
silently accumulating:

`hermes_cli/` · `gateway/` · `tui_gateway/` · `acp_adapter/` · `plugins/` · `skills/` · `cron/`

Upstream is ~870k lines of non-test Python. A wholesale port is not the goal and never was.

---

## Open questions

- **Test oracle.** Upstream ships 2,889 test files. Are any worth adapting as a behavioural
  spec, given this is not a port and the behaviour is only selectively shared?
- **Context strategy.** Local models have far less context than cloud models. Agentic file work
  consumes it quickly, so what gets sent, and what gets summarised, is a first-class design
  problem rather than an optimisation.
- **Which models, on which machines.** A full list exists; the tournament harnesses already
  encode part of it. **72 GB changes this question** — the tournaments used 9B–12B because that
  is what fit in 48 GB. A 70B-class model at Q4 becomes viable, including upstream's own
  Hermes 4 70B. Whether a supervisor architecture is still the right answer when the model is
  6× larger is an open question, not a settled one: the bounded-session finding came from
  watching *small* models drift.
