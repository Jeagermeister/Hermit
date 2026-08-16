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
- [x] **Model preflight (R9)** — `src/hermes/ollama/preflight.{h,cpp}`, 15 tests. Six checks
      are defined; a default run emits five, since the inference warmup is opt-in and the
      tools gate is waivable. Each check that runs fails closed: "I could not determine the
      context window" is a failure, not a pass. Findings from building it:
      - **The two context numbers are not the same number.** `/api/show` reports both
        `<arch>.context_length` (what the model was built for) and `num_ctx` (what the
        Modelfile pinned). On `qwen35-agent:latest` they read 262144 and 131072. Reading the
        wrong one is a real mistake rather than a pedantic one — though which one *binds* is
        the opposite of what this bullet first claimed; see below.
      - **Unpinned is undetermined — and the "4096" in R9's evidence is wrong.** This was
        first written as "unpinned is not unknown, it is 4096", following
        [REQUIREMENTS.md](./REQUIREMENTS.md). Measured against Ollama **0.32.9** — the same
        version recorded in the benchmark provenance — an unpinned model loads *far above*
        4096: `qwen3.6:27b-q8_0` came up at 262144. Controlled against the obvious confound:
        a pinned variant with a 262144 architecture and a 131072 pin reports 131072 in
        `/api/ps`, so that field is the allocated context and not an echo of the architecture.

        It is **not** simply "the architectural context", which this bullet also claimed for a
        while: `nemotron-3.5-lightning:30b` reports an architecture of 1048576 and loaded
        unpinned at 262144, capped well below it. There is a server-side ceiling whose value
        no API reports. That makes the unpinned context *less* knowable, not more — which is
        what the conclusion rests on.

        (Provenance covers 7 runs across the two files that record it; the two larger result
        files predate version recording, so "the same version as the benchmarks" is
        established for those 7, not for the whole corpus.)
        The gate still fails an unpinned model, because the server default is set by the
        Ollama release and `OLLAMA_CONTEXT_LENGTH` and is reported by neither `/api/show`
        nor anything else — but it now says "undetermined" instead of naming a number it
        cannot observe. **R9's stated evidence needs the same correction.**
      - **The gate asks about the architecture, not the Modelfile pin.** Since
        [D8](./DECISIONS.md) the client sets `options.num_ctx` on every request, and that
        **overrides the pin upward** — measured directly: a model pinned to 8192, asked for
        32768, loaded at 32768. So the pin decides nothing this client cares about. What no
        request can exceed is the architecture, and that is what is now gated on. The pin is
        still reported, because it is useful for diagnosis; it just no longer enforces.
      - Swept across all 14 models installed on `kitchen-desktop`: **14 pass, 0 fail.** Under
        the earlier pin-based gate the 6 base tags all failed as "unpinned" — and every one of
        those was a false positive, since the client sets the context itself. The
        pinned-variant discipline the benchmark harnesses built (8 `-agent` variants against 6
        base tags) turns out to be an artifact of
        driving Ollama through an endpoint that could not set `num_ctx`.
- [x] **Ollama client** — `src/hermes/ollama/client.{h,cpp}`. Native `/api/tags`, `/api/show`
      and `/api/chat`, non-streamed; verified against a live daemon and clean under ASan/UBSan
      and clang. Tool-call dispatch is deliberately *not* here — it belongs with the agent loop
      in Phase 2, and this reads the text half of a reply only.
      **Settled as [D8](./DECISIONS.md):** native over OpenAI-compatible, because only the
      native endpoint can set `num_ctx` — and an unsettable context window is what falsified
      R9's original evidence. The `num_ctx` clamp is part of that decision: Ollama does no
      admission control on context size, and an oversized request hard-froze `kitchen-desktop`
      on 2026-08-13 rather than failing.
- [x] **Toolchain floor** — a `try_compile` feature check for the C++23 library pieces, plus
      version numbers (g++ 12, clang 16) that exist only to turn a template error into one
      sentence. The feature check is the real gate, because the binding constraint is the
      standard *library*: clang here uses libstdc++, so a compiler-version check alone would be
      interrogating the wrong component. **Verified on g++ 15.2 and clang 21.1, both against
      libstdc++. libc++ is untested.**
- [x] JSON handling — nlohmann v3.12.0, in the build with the client (D2)
- [x] **Config + CLI entry point** — `src/hermes/app/config.{h,cpp}`, 78 tests. Four sources
      overlaid field by field — defaults < `--config` file < environment < flags — with each
      field recording which source last wrote it. A new `hermes_app` target, because settings
      that name a model and a base URL cannot live in `core` under D7's layering table, and
      because the MCP-over-stdio frontend has to compose itself from the same settings rather
      than from a second parser. `hermes-cpp config` prints the resolved set. Findings from
      building it:
      - **R1 is a rule about configuration, not just about path resolution.** Its own words are
        "never inherit a working directory, never infer a project or git root," and applying
        that here settles two questions that would otherwise have been taste. The sandbox root
        gets **no default** — not the working directory, not `$HOME`, not a git root, because
        every candidate default *is* an inferred root. And there is **no implicit config-file
        search**: walking up from the working directory looking for `.hermes.json` is the same
        act, so a file is read when it is named and never otherwise.
      - **A relative sandbox root has three different honest answers, one per source.** This was
        not anticipated and is the most interesting thing the work turned up. A path in a *file*
        anchors to that file's own directory — anything else makes one file mean different
        things depending on where the binary was launched. A path typed as a *flag* anchors to
        the working directory, legitimately: it was typed at launch, where that directory is
        whatever the operator is looking at. A path in the *environment* has neither anchor, so
        it is **rejected** rather than resolved against a guess. `load` settles all three before
        anything downstream sees the value, which is also what lets the printed config show the
        directory that will actually be used instead of the two characters somebody typed.
      - **D7's loopback rule now fails at configuration rather than at connect.** `validate`
        calls `ollama::validate_base_url` instead of restating it — there must be exactly one
        answer to "is this URL local?", since that is what keeps sandbox file contents on the
        machine. The gain is when, not whether: `--url http://10.0.0.5:11434` is refused before
        a Client exists.
      - **`nlohmann`'s `is_number_integer` would have accepted `-1` as a clamp of
        18446744073709551615.** Signed, converted to `uint64_t`, it sails past every range check
        as an enormous positive number — for `max_num_ctx` specifically, that is D8's
        machine-freezing value arriving through a typo. `is_number_unsigned` is what makes it a
        type error instead.
      - **Unknown keys are errors.** A typo'd `max_num_ctx` that is silently dropped leaves the
        D8 clamp at its default while the operator believes they raised it. Silently discarding
        a setting somebody wrote down is the failure mode this codebase keeps finding in other
        people's tools.
      - **The printed config is a product surface, not a debugging aid.** Four sources feed
        these settings and two of them (`max_num_ctx`, `minimum_context`) are safety limits
        whose numbers this project has already had to correct in public once. A raised clamp, a
        disabled R9 floor and a waived tools gate are each legal and each earn a marked line:
        nothing is rejected for being unusual, but nothing unusual can be in force silently.

      **Four defects found in review, all reproduced against the binary before being fixed.**
      Recorded because three of them are the same shape as failures this project already has
      requirements about:

      - **`std::ifstream` opens a *directory* successfully on Linux, and then aborts the
        process.** `!file` is false, so the guard passes; libstdc++ throws
        `std::ios_base::failure` out of `basic_filebuf::underflow` on the first read — from
        inside the streambuf, where the stream's exception mask does not gate it —
        `istreambuf_iterator` propagates it, and `main` has no handler. `hermes-cpp config
        --config /etc` dumped core. Pointing at a config *directory* instead of the file inside
        it is an ordinary slip. Fixed by requiring a regular file before opening, which also
        covers a FIFO (where the `open` itself would have blocked forever) and a device node,
        plus a `try`/`catch` so "no exception escapes" is a guarantee rather than a list.
      - **An oversized timeout did not lengthen the wait, it removed it.**
        `std::chrono::seconds::rep` is `int64_t`, so `--chat-timeout 18446744073709551615`
        arrived as **−1**; cpp-httplib casts to `int` milliseconds and hands that to `poll(2)`,
        where a negative timeout means *block forever*. A fail-open on the one setting whose
        job is to bound a wedged daemon — the same shape as R9's original fail-open and the
        sandbox's EACCES bug. Now bounded at both ends, in the overlays and again in
        `validate`.
      - **`HERMES_CONFIG=` set-but-empty read no file and said nothing** — the fail-closed rule
        broken for the single variable that selects every other setting. It was also the one
        setting read through a bare `getenv` rather than the injected lookup, so no test could
        reach it; that is why it survived. Both fixed together, which is not a coincidence.
      - **Two parsers over one command line disagreed about which tokens are values.**
        `--model --config x.json` loaded `x.json` — the config-file scan saw `--config`
        followed by a path, while the flag parser read a model literally named `--config`. A
        file the operator never asked for was applied. The scan now steps over other flags'
        values from a shared table, a flag where a value belongs is rejected outright, and a
        drift test asserts the two still describe the same command line. `--` was added at the
        same time, since a path-resolution tool that cannot name a path beginning with a dash
        has a real gap and the *model* picks those names.

      Also from review, and worth keeping in view: `load` hands back `string_view`s into its
      own arguments. Free from `main`, where they point into `argv`; a trap for the MCP
      frontend, which would naturally build them from `std::string`s that then go out of scope,
      leaving the positional list dangling next to a `Config` that owns its strings and looks
      perfectly healthy. Documented on the declaration.

      **A second review round, against the fixed code, found four more — including one the
      first round's own fix introduced.** Recorded because the pattern is the lesson:

      - **`HERMES_CONFIG` was resolved against the working directory, while
        `HERMES_SANDBOX_ROOT` right beside it was rejected for exactly that.** The fix that
        routed the variable through the testable seam never asked whether the value was
        relative. Same doctrine, opposite treatment, three functions apart — and the longer
        route was worse: a cwd-dependent *file* can itself set a relative `sandbox_root`
        anchored to whichever directory won. An inferred root arriving by the back door. Now
        rejected, with the same message as its sibling.
      - **The D7 loopback check ran for commands that never open a socket.** An
        `HERMES_OLLAMA_URL` exported for some other tool broke `resolve`, which is pure
        filesystem work — and stopped `hermes-cpp config` printing, which is the command whose
        entire job is showing you which value is wrong. Gated on a new `Requirements::ollama`,
        with `render()` marking a non-loopback URL instead. Verified that `preflight` still
        refuses one, since that is the half that matters.
      - **Only `apply_json` had been made transactional; `apply_env` and `apply_flags` still
        left partial state behind on error.** Fixing one instance of a pattern and not its two
        siblings is its own failure mode. All three now commit or do nothing.
      - **The drift guard was not bidirectional, and the test was itself a third copy of the
        flag table.** Now `flags_taking_a_value()` exposes the real table so the test cannot
        drift from it, and `take()` refuses to run for a flag missing from that table — so the
        dangerous direction (a value-taking flag added to the chain and forgotten in the table,
        re-creating the two-parser divergence) fails loudly the first time it is used.

      **One fix collided with another, which is the most useful thing this round produced.**
      Making `find_config_flag` skip only well-formed values — so a `--` terminator stayed
      visible — silently re-opened the `--model --config x.json` hole the previous round had
      closed. The test suite caught it immediately. The right answer was not to make that
      scanner cleverer but to remove its exposure: `load` now parses the flags once against a
      throwaway config before any file is opened, so a bad command line is reported against the
      flag the operator actually got wrong and no file is read on the strength of a line
      already known to be invalid. Two parsers over one command line can only be reconciled by
      ordering, not by making each smarter.
- [x] **Session/history model** — `src/hermes/supervisor/session.{h,cpp}`, 49 tests, plus a
      `hermes-cpp session` harness for the one property a unit test cannot settle. A new
      `hermes_supervisor` target: [D7](./DECISIONS.md)'s table puts bounded sessions in the
      supervisor layer, and this is the first code that belongs there. It drives no socket
      itself — `prepare()` returns a `ChatRequest` and `record()` consumes a `ChatReply` — so
      every policy decision in it is testable offline, for the same reason D8's clamp is.

      **The premise turned out to be worse than assumed, and it was measured rather than
      taken on trust.** Six messages carrying one codeword each, sent whole at three window
      sizes, Ollama 0.32.9, `kitchen-desktop`. Columns are [system, u1, a1, u2 (40 KB), a2]:

      | model | `num_ctx` | `prompt_eval_count` | codewords returned |
      |---|---|---|---|
      | gemma31-agent | 32768 | 9005 | sys ✓ u1 ✓ a1 ✓ u2 ✓ a2 ✓ |
      | gemma31-agent | 2048 | **64** | sys ✓ u1 ✗ a1 ✗ u2 ✗ a2 ✓ |
      | gemma31-agent | 1024 | **64** | sys ✓ u1 ✗ a1 ✗ u2 ✗ a2 ✓ |
      | qwen35-agent | 32768 | 9018 | sys ✓ u1 ✓ a1 ✓ u2 ✓ a2 ✓ |
      | qwen35-agent | 2048 | **70** | sys ✓ u1 ✗ a1 ✗ u2 ✗ a2 ✓ |

      - **It is silent, and it keeps the system prompt.** No error, no warning, no flag: a
        9005-token prompt became 64 and the reply looked entirely healthy. The surviving
        system prompt is what makes it dangerous — the model still *sounds* correctly
        configured, having forgotten everything it did. This is R6's shape with the client
        as the party being misled, and it is the whole argument for the class.
      - **It discards a contiguous middle, not the oldest turn.** `u1` and `a1` are a few
        tokens each and would have fitted with room to spare. They went anyway.
      - **It does not use the window it has** — 64 tokens kept out of 2048 available, and
        nothing packed back in. So the cost of letting the server handle overflow is not
        "lose the oldest turn", it is "lose almost everything". Cross-family, so not a
        gemma quirk.
      - **`prompt_eval_count` reports the whole prompt, not the newly-evaluated part.**
        Checked because the opposite would have made a prefix-cache hit indistinguishable
        from a discard and killed the detection outright: the same prompt sent twice read
        2127 then 2127, and a two-message continuation 2143 then 2143. So a shortfall
        against it is real evidence, and `record()` verifies every prompt was received
        whole — R5's read-back discipline applied to the prompt rather than to a file.
      - **Four characters per token is not a safe assumption for an agent.** It is an
        average over English prose, and this sends paths, code and JSON:

        | content | gemma31-agent | qwen35-agent |
        |---|---|---|
        | english prose | 5.88 | 5.96 |
        | source code | 2.85 | 3.11 |
        | JSON | 2.52 | 3.11 |
        | filesystem paths | 2.40 | 2.83 |
        | base64-ish | 1.63 | 1.36 |

        A 4.0 assumption under-counts real traffic by 40% and a base64 blob by nearly 3×,
        and under-counting is the direction that ends in a discard. There is no tokenizer
        in this process and Ollama exposes no endpoint for one, so the estimate starts
        pessimistic and is only ever revised *downwards*.
      - **Pessimism has a price, and the manual harness is what showed how steep.** This is
        the second time `src/main.cpp` has earned its keep. The first live run estimated 935
        tokens for a prompt Ollama evaluated at 268 and **dropped five turns it did not need
        to drop** — a supervisor discarding history it could have kept, failing in the same
        direction as the server and only more politely. The fix is that pessimism is only
        *necessary* for content the model has not seen yet: every prompt already sent has an
        exact count in `prompt_tokens`, so the measurement is attributed back onto the
        messages it covered and they stop being guesses. The same run afterwards: **zero
        turns dropped**, and the model answering "red, blue, and yellow" where it had
        previously been able to recall only the last one. The guess now applies to one new
        message instead of compounding across the whole conversation.
      - **A reserve smaller than the generation budget reserves nothing.** `reply_reserve`
        keeps the prompt from filling the window; a larger `num_predict` just means the
        model generates past it and the server shifts the window — the same silent discard
        arriving from the other end. `open()` refuses the combination rather than leaving a
        setting that looks protective and is not.

      Dropping, not summarising, is the interim policy: **Context strategy** below is still
      an open question and quietly answering it here would be the wrong place. What the
      class does guarantee is that the choice is *this code's* and that it is counted —
      `dropped()` is a fact about the run, not an implementation detail.

      **Two review rounds found five more defects, and one of them was a measurement the
      reviews asked for rather than a bug either of them found.**

      - **The reply was priced with the ratio from before its own calibration.** `record()`
        built the assistant turn, *then* tightened the chars-per-token figure, and
        `re_estimate()` could not reach the new turn because it walks the history the reply
        had not joined yet. The stale figure is too *low*, so the next `prepare()` admitted
        a prompt larger than the session's own belief — under-counting, the unsafe
        direction, landing on the very first turn because that is when the calibration jump
        is biggest. Fixed by ordering: the reply is priced last.
      - **The pessimistic constant was not pessimistic.** It sat at 2.0 and the comment
        called it "near the base64 worst case", while the table in the same header measures
        base64 at **1.36** — a 47% under-count, of exactly the content an agent reading
        files runs into. Now 1.3, below every measured figure. Attribution is what made
        that affordable: the guess covers one unsent message instead of the whole
        conversation, so the price of proper pessimism is a message occasionally refused
        as too large — loud and recoverable — rather than a silent collapse.
      - **`num_predict` could be handed a negative number, which Ollama reads as
        "unlimited".** `max_num_ctx` is deliberately unbounded above (D8 invites raising it
        for a bigger card), the harness derived a reply reserve from it, and
        `--max-num-ctx 10000000000` cast to **−1794967296**. The setting whose job is to
        bound generation would have removed its bound. This is the third appearance of the
        same shape, after R9's original fail-open and the `--chat-timeout` overflow that
        meant "wait forever"; bounded at both ends now, and `Session::open` refuses a
        non-positive budget outright.
      - **Nothing stopped the planning window exceeding the model's architecture.** The
        defaults cohere only because R9's `minimum_context` floor and D8's `max_num_ctx`
        clamp are *independently* 65536. Raise the clamp — which D8 explicitly invites —
        and a session plans against tokens the model cannot hold, discovering it from the
        collapse afterwards. `SessionOptions::architecture_context` is now a third ceiling
        and the harness reads it from `/api/show`. It fires on the case above: the same
        `--max-num-ctx 10000000000` now yields a 262144 window, gemma31's actual
        architecture, instead of a wrapped generation budget.
      - **The "planned window and sent window can never disagree" claim was convention, not
        construction.** `Session::open` took a `ClientOptions` that nothing tied to the
        `Client` a caller would actually dial, and `Client` exposed no way to check. Now it
        takes the `Client` itself and reads `max_num_ctx()` from it; the options overload
        survives as a documented testing seam.

      **The measurement the reviews prompted is the most useful thing here.** Everything
      above had overflowed the window by 4–100×. Whether a *marginal* overshoot is trimmed
      proportionally was unmeasured, and it decides whether a collapse-shaped detector is
      the right shape at all. Swept across a 4096-token window:

      | prompt | evaluated | kept | |
      |---|---|---|---|
      | 4052 | 4052 | 100% | fits |
      | 4496 | **44** | 1.0% | 1.10× over |
      | 5385 | **44** | 0.8% | 1.31× over |
      | 8940 | **44** | 0.5% | 2.18× over |

      **It is a cliff, not a slope.** Ten percent over costs the same as 118% over. There is
      no proportional-trim regime, so one token past the window is the whole loss — which
      makes prevention the only strategy that helps and confirms the detector should key on
      collapse. It also retires the worry that a tighter estimate had narrowed detection:
      there is no marginal case to miss.

      Known and accepted, in the manner [D6](./DECISIONS.md) treats its own TOCTOU race: a
      measurement is shared across messages *by character count*, and cost per character
      spans 1.36 to 5.96. The total stays right, which is what `prepare()` reads, so this is
      harmless while every priced turn is present; it stops being harmless when one is
      dropped, and the next reply re-anchors the total. Bounded to the turns between a drop
      and the following exchange, with the cliff detector behind it. Fixing it properly
      needs a per-message token count that no Ollama endpoint exposes.
- [ ] **Wall-clock budgets (R8)** — per turn and per session, a timeout recorded as a failure
      rather than dropped from the denominator. Phase 0 strengthens this considerably: 11 of 19
      failures were 300 s timeouts from a single model, and `nemotron35-lightning` burned
      3,826 s against `gemma26-a4b-q8`'s 642 s.

      **The Ollama client turned up a trap for this — though not the one first recorded
      here.** An earlier revision of this bullet claimed `eval_count` *omits* thinking tokens
      and therefore undercounts. **That was wrong, and is retracted.** Re-measured on
      `gemma31-agent`, prompt `"Give me a city and its country."`, `num_ctx` 32768,
      `temperature` 0, varying only the budget:

      | `num_predict` | `done_reason` | `eval_count` | thinking | content |
      |---|---|---|---|---|
      | 20  | length | 20  | 74 ch  | 0 ch  |
      | 80  | length | 80  | 274 ch | 0 ch  |
      | 300 | stop   | 148 | 487 ch | 12 ch |

      The first two rows settle it: content is empty while `eval_count` reads 20 and 80, which
      is impossible if the count covered content alone. `eval_count` **includes** thinking, as
      does the budget, and the two agree. Identical on `/v1/chat/completions` (148 for the same
      generation), so it is Ollama's accounting rather than an API-shape artifact.

      The real hazard survives the correction and is simpler than the one claimed: **a budget
      too small for the thinking returns empty content with no error.** The model reasons
      first, exhausts the budget, and writes nothing. Two reliable tells —
      `done_reason == "length"` (exposed as `ChatReply::truncated()`) and
      `eval_count == num_predict`. R8's budgets should still be wall-clock, but for the reason
      originally given in the requirement rather than for the token-accounting one.

- [ ] **Substrate probe (D11)** — probe the filesystem under the sandbox root and record which
      guarantees actually hold, rather than assuming them. [ROUTING.md](./ROUTING.md) §4's
      `dev:ino:size:mtime:ctime` identity tuple is shared by `list`, the staleness guard and
      `edit`'s fail-closed check, and **every component of it is substrate-dependent and was
      unchecked**. Lives in `hermes_core` beside `Sandbox`, needs no new link edge, and is
      implementable before ROUTING.md §12 step 1. See
      [D11](./DECISIONS.md#d11--the-substrate-is-probed-not-assumed).

### Decisions to settle first

These are hard to reverse and benefit from being argued out before code exists:

- [x] **Concurrency model** — blocking and single-threaded ([D1](./DECISIONS.md))
- [x] **HTTP library** — cpp-httplib, pinned v0.53.0 ([D7](./DECISIONS.md)). No longer
      provisional: the Ollama client exists and the library needed only five configuration calls
      and one RAII wrapper to scope timeouts per request. Loopback only, so no TLS and streaming optional.
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
- [ ] `read`, `write`, `list` — **the tools exist as of 2026-08-16** ([ROUTING.md](./ROUTING.md)
      §12 step 3); the "prove the loop end to end" half of this bullet stays open with the
      agent-loop bullet above, which is why the box is not ticked
- [x] ~~`edit`~~ — done 2026-08-16, with the exactly-once occurrence rule and the
      observed-state gate recorded in [ROUTING.md](./ROUTING.md) §4; "hardest to get right"
      held up, which is why its semantics were argued before its code
- [x] ~~**Read-back after every write (R5)**~~ — implemented 2026-08-16 exactly as written:
      `write` and `edit` read back and byte-compare before the turn succeeds, and the
      read-back's stat becomes the recorded observation.
- [x] ~~`move`, `search`~~ — `move` done 2026-08-16 (`RENAME_NOREPLACE`, R3 both ends);
      `search` was split into `find` and `grep` when the tool surface was settled and both
      exist; [ROUTING.md](./ROUTING.md) §4 is the decided list
- [ ] **Do not foreclose the second backend ([D9](./DECISIONS.md)).** vLLM is decided in
      principle and **deferred** — Kitchen's default runtime is Ollama, so nothing is blocked
      today. The Phase 2 obligation is only to avoid adding new per-request assumptions to
      the client. The implementation, and its two gates (per-backend preflight; where the
      context window lives), are recorded in D9 and become schedulable if Kitchen's default
      flips to vLLM.

---

## Phase 2.5 — Frontends: human and machine

Settled in [D7](./DECISIONS.md): local inference only, driven both by a person and by a larger
model calling this as a tool.

- [ ] **Clear D7's gate — two conditions since 2026-08-15, and this bullet used to name one.**
      Kernel confinement ([D10](./DECISIONS.md)) for containment, *and* `openat(O_NOFOLLOW)`
      component-walking for in-root correctness. D6 accepted the race against a "confused 3B
      model" threat model; a callable frontend changes that model, so both are a gate rather
      than cleanup. Confinement alone is not enough — an in-root redirection is invisible to
      the kernel and is D6's own worked example. [ROUTING.md](./ROUTING.md) §12 step 4 carries
      the implementation detail, including the grant set and the denied-write probe.
- [x] ~~**Decide the hardlink answer**~~ — decided with D10, recorded under "Still open" in
      [DECISIONS.md](./DECISIONS.md): creation is blocked by the one-writable-root rule; a link
      planted before the sandbox starts is accepted explicitly, with the threat-model reasoning
      written down.
- [ ] **Kiro is the named caller, and it decides the platform question.** Kiro consumes MCP
      over stdio exactly as specified below, so no new frontend shape is needed — but Kiro CLI
      runs natively on Windows, where a Linux binary is not spawnable. A "Power" is the wrong
      vehicle and that is settled. [ROUTING.md](./ROUTING.md) §8 carries the integration detail;
      [SCOPE.md](./SCOPE.md) § Platforms carries the Windows scope decision.
- [ ] **MCP server over stdio.** No listener, no port, no auth. Thin: transport only, over the
      same core the CLI drives.

## Phase 3 — The supervisor (the actual product)

- [ ] **State verification.** After each turn, check what the model *claims* against what the
      filesystem *shows*.
- [ ] **Re-invocation** with one concrete remaining failure, per the tournament recommendation.
- [ ] **Guardrails** — dry-run, backup-before-mutate, undo. The erased `tally.py` is the argument.
      *Where* backups live was settled 2026-08-15 — never granted to the confined child, per
      [D10](./DECISIONS.md) and [ROUTING.md](./ROUTING.md) §11 — so what remains here is
      retention and how undo is invoked, which is still the load-bearing half.
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
