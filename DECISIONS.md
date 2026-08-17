# Decisions

The hard-to-reverse choices [ROADMAP.md](./ROADMAP.md) asked to have argued out before code
exists. Each records what would overturn it, so a later disagreement can be settled on evidence
rather than re-litigated from scratch.

Settled 2026-08-13, at the point the first code was written — except **D6**, which was decided
during implementation and revised once when review caught the resolution order. It is recorded
here anyway: it is hard to reverse, which is the bar for this document.

---

## D1 — Concurrency: blocking and single-threaded

One request in flight at a time. No event loop, no thread pool, no async runtime.

**Why.** The supervisor is blocked on the model essentially all the time — thinking models
measured at 65–107 s per turn against 6–17 s for non-thinking ones. Concurrency buys nothing
against a workload that is one synchronous inference after another, and it costs the thing this
architecture depends on most: the ability to say exactly what the filesystem looked like before
and after a turn. R3 through R6 are all state-comparison requirements, and comparing state is
dramatically harder when something else might be mutating it.

**What would overturn it.** Running several independent sessions in parallel to raise throughput.
Note the answer would then be *process*-level parallelism — separate sandbox roots, one session
each — not threads inside one session. The bounded-session architecture already points that way.

## D2 — JSON: nlohmann/json, pinned v3.12.0

**In the build since 2026-08-13**, arriving with the Ollama client as planned. Nothing has
argued against it. The surface used so far is `find`/`is_*`/`get`/`items`/`array`/`push_back`,
`parse` with `allow_exceptions=false` plus `is_discarded()`, and `dump` with
`error_handler_t::replace` — that last one load-bearing, since the throwing default turns a
non-UTF-8 file into a `std::terminate`. Request building is as pleasant as expected.

**Why.** Ergonomics matter here and throughput does not. The JSON payloads are chat messages and
tool calls measured in kilobytes, arriving at best once every several seconds. simdjson solves a
problem this project does not have, and pays for it with a read-only DOM that is awkward for the
request *building* that makes up half the work.

**What would overturn it.** Nothing plausible at this scale. If parsing ever showed up in a
profile, it would mean something else had gone very wrong.

## D3 — Dependencies: FetchContent, pinned

Third-party sources are fetched at configure time at pinned tags. Only the toolchain comes from
the system.

**Why.** This builds on two machines — the MSI laptop and `kitchen-desktop` — and results are
compared across them. Phase 0 separates harness effects from *model behaviour*; hardware is a
confound it has to hold constant, not something it measures. A dependency that differs silently
between the two machines is one more uncontrolled variable in exactly the place that cannot
afford them. Pinning also means a fresh clone builds with no `pacman` step.

**Cost, accepted.** A network fetch on first configure per build directory, and the pins need
occasional manual bumping.

**What would overturn it.** Building somewhere without network access at configure time, or the
dependency set growing large enough that per-build-directory fetches dominate the build. Either
would push toward vendored sources or system packages with a version check.

## D4 — Tool interface: virtual dispatch, declarative arguments

A plain virtual base class for dispatch; an `Args` struct with field descriptors underneath it
for schema generation and parsing.

**Why not full compile-time generation.** Verified on this toolchain: neither GCC 16.2.1 nor
clang 22.1.8 defines `__cpp_reflection` under `-std=c++26`. Without static reflection, field
*names* cannot be recovered from a struct — and a JSON schema is mostly field names. So the
descriptor list has to be written by hand either way, and generating dispatch as well would buy
template-heavy diagnostics for no additional safety.

**Why not hand-written schemas.** Two properties are worth machinery:

- **R1 becomes structural.** Parsing an argument produces a `SandboxPath`, which only
  `Sandbox::resolve` can construct. A tool physically cannot name a file outside the root, so
  the check cannot be forgotten at tool #37.
- **R2 drift becomes unrepresentable.** The schema sent to Ollama as `format` and the parser
  that reads the reply come from one declaration. Hand-maintained, they disagree eventually —
  a field added to one and not the other fails silently, which is the worst way to fail.

**What would overturn it.** A toolchain with working static reflection; the descriptor lists
would then be deletable, and D4 collapses into the simpler full-generation design.

**Implementation note (2026-08-16).** `tool.h` landed the descriptor half as declared, with one
divergence recorded here so it is a decision rather than drift: arguments arrive in a shared
name-keyed `ToolArgs` rather than a per-tool `Args` struct, so a mistyped field name inside a
tool body is a null at runtime — caught by that tool's first test, and by `Tool::invoke`'s
spec-identity check when the mismatch is cross-tool — where the literal `Args`-struct shape
would have made it a compile error. The descriptor list remains the single declaration, and the
R1 property (a Path argument exists only as a `SandboxPath`) is unaffected. Per-tool structs
remain what static reflection would enable.

## D5 — Constrained decoding: on, from the start

Tool arguments are schema-constrained at decode time via Ollama's `format` parameter.

**Why.** `llama3.2:3b` emitted `"['echo', 'HERMES-OK > hello.txt']"` — a Python list where a
string belongs — and separately emitted an entire tool call as prose that was never parsed as a
call. Those are structural failures, and a schema pinning `command: string` makes that specific
class of them impossible.

**Stated honestly:** this fixes malformed calls, not wrong ones. A well-formed call to delete the
wrong file is unaffected. That is what the supervisor is for, and constrained decoding should not
be sold as doing more than it does.

**What would overturn it.** Evidence that constraining `format` measurably degrades tool-call
*quality* on the models actually used — a real risk, since constrained decoding narrows the
distribution the model samples from. Worth measuring on the fsops harness rather than assuming
either way.

> **⚠ Overturned in part, 2026-08-17, by the measurement this paragraph asked for. See
> [D12](#d12--tool-calls-are-native-format-is-never-sent-alongside-tools).**
>
> It was measured on the fsops model set, and the degradation is not marginal: adding `format`
> alongside `tools` takes four of seven models from a correct call to no usable call at all,
> `llama3.2:3b` — the source of the evidence above — included.
>
> **What this decision got right** is that malformed tool arguments are a real failure class
> worth a structural answer. **What it got wrong** is the mechanism: `format` is not how tool
> arguments get constrained, because it is not composable with tool calling. D12 keeps the goal
> and changes the means, and `format` keeps its job for structured *replies* with no tools
> offered — Tier 1's shape — where it was re-measured working.

## D6 — The sandbox is a capability type, and resolution is POSIX-order

`Sandbox::resolve` is the only way to construct a `SandboxPath`. Tools take `SandboxPath`, not
`std::filesystem::path`. Resolution walks components and expands symlinks as it meets them.

**Why a capability type.** R1's failure was not that someone wrote a bad check — it was that two
tool families each resolved paths their own way. A check that must be *remembered* will be
forgotten around tool #37. Making the checked path the only nameable path removes the question:
a tool physically cannot address a file outside the root. `contains()` is private for the same
reason — it assumes an already-resolved path and fails *open* on anything else, so it must not
be reachable by a caller looking for a convenient safety check.

**Why POSIX-order rather than textual normalisation.** The first implementation normalised
`..` textually and then expanded symlinks, which is wrong in both directions and was caught in
review. With `link -> a/b`, `link/../deep.txt` collapsed to `root/deep.txt` — right filename,
wrong directory, which is R1's own failure reproduced inside R1's fix. And when the link pointed
*outside* the root, textual collapse silently rebased the path back inside it, onto a real and
different file, which R1 forbids outright. Fuzzing found 964 of 128,742 accepted paths diverging
from `realpath(3)` this way.

Resolving in the kernel's order fixes both without rejecting anything legitimate: correct where
correctness is possible, rejection only where the path genuinely leaves the root.

**One real hole, found by review and closed.** `walk()` treated *every* `symlink_status` error
as "this name is not there yet", which merges "absent" with "I could not find out". On EACCES it
returned a path with an unexpanded symlink still in it, and `contains()` — which assumes a fully
resolved path — accepted it. A link inside an unreadable directory pointing out of the root
produced a `SandboxPath` reporting `sub/esc/secret.txt` that read a file outside the root. A
model can create that state itself: `chmod 000` on a directory it owns.

Now only `no_such_file_or_directory` and `not_a_directory` mean absent; anything else is a
`FilesystemError`. This is what makes `contains()`'s precondition — and therefore this
decision's central claim — actually true rather than merely intended. Note the fail-open was in
*error handling*, not in the path algebra: 7.3 million differential inputs against `realpath(3)`
found no divergence in the resolution logic itself.

**Known and accepted.** A TOCTOU window remains between `resolve` and `open` — closing it needs
`openat(O_NOFOLLOW)` component-walking, and [D7](#d7--local-inference-only-two-ways-in-human-and-machine)
makes that a gate on shipping the programmatic frontend. Hardlinks to outside files placed
inside the root are also accepted; no path-based check can catch that. Paths are bounded at
`PATH_MAX`, since resolution allocates roughly 60x the input.

**Partially answered by [D10](#d10--kernel-confinement-for-shell-landlock-vendored-one-writable-root),
2026-08-15.** Kernel confinement makes the TOCTOU window non-exploitable *as an escape* — the
open is denied whatever was swapped in — and blocks the creation of an escaping hardlink. It
does neither of the things this section is actually about. **A swap that redirects to a
different file inside the root is permitted**, which is this decision's own worked example
above: right filename, wrong directory. So `O_NOFOLLOW` is not made optional by D10; the two
mechanisms cover disjoint failure modes.

**What would overturn it.** Untrusted input, which promotes the TOCTOU race and the hardlink
gap from documented to blocking.

## D7 — Local inference only; two ways in, human and machine

**Backend: local inference, and nothing else.** No cloud inference provider, ever, unless this
decision is explicitly overturned.

> **Amended 2026-08-14 by [D9](#d9--two-local-backends-ollama-and-vllm): two local backends,
> Ollama and vLLM.** This clause originally read *"local Ollama, and nothing else"*, and is
> widened here on the record because the sentence above requires exactly that rather than a
> convenient reading. **The cloud prohibition is untouched, and it is the part doing the
> work** — vLLM qualifies because it is local, not because the bar moved. Every reason given
> below still holds verbatim: no credential pool, no egress proxy, no TLS, nothing reachable
> from off the machine.

**Frontends: both a human CLI and a programmatic caller.** The same core is driven by a person
at a terminal and by a larger model invoking it as a tool.

**Why local-only stays local-only.** The realistic programmatic caller *is* a large cloud model
— Claude or ChatGPT invoking this as a tool. If the caller is already the big model, putting a
second cloud model behind this tool pays twice for the same capability. It would also re-import
precisely what [SCOPE.md](./SCOPE.md) discarded as meaningless against local Ollama:
`credential_pool.py` (3,178 lines of key rotation) and `iron_proxy.py` (2,494 lines of egress
firewall for cloud credentials). Staying local keeps credentials, TLS and egress policy out of
the codebase entirely.

It also bounds the blast radius. A confused local model with filesystem access is a contained
problem; the same code path reachable by an arbitrary remote endpoint is not.

**Why both frontends, and why that is not scope creep.** [README.md](./README.md) has said from
the start that the job is filesystem work "either directly or when called as a tool by a larger
model" — this records a commitment already made rather than adding one. The two surfaces share
everything that matters and differ only in transport.

**Expected shape.** A local MCP server over **stdio** — a subprocess speaking JSON-RPC on
stdin/stdout — not an HTTP listener. No port, no auth, no TLS, nothing reachable from off the
machine. Note this is the opposite direction from upstream's `mcp_tool.py`, which is an MCP
*client* and stays discarded; being a *server* is not covered by that decision.

This is also where the native-binary argument pays off hardest. A caller invoking a tool pays
process startup on every call, and bounded sessions mean many calls: ~10 ms for a static binary
against 1–3 s for a Python interpreter. Speed here is a product feature, not a benchmark number.

**Layering, so the surfaces stay thin:**

| layer | contains | knows about |
|---|---|---|
| **core** | sandbox, filesystem ops, verification (R1, R3, R4, R5) | no model, no network |
| **supervisor** | drives the local backend; bounded sessions, retry (R6, R7) | the only layer with an HTTP client |
| **frontends** | CLI today, MCP-over-stdio next | neither of the above, beyond a small API |

**Amended 2026-08-13, when the supervisor layer got its first code.** The table has three
rows and the tree has four directories: `src/hermes/ollama/` is the transport, and it sits
*under* the supervisor rather than inside it. "The only layer with an HTTP client" is
therefore loose — `hermes_supervisor` links `hermes_ollama` for the request and reply types
but never httplib, which stays `PRIVATE` to the transport target and absent from its headers
behind a pimpl. The commitment the row was making still holds, and holds more strongly than
written: nothing above the transport can reach HTTP even by accident. Recorded rather than
silently re-drawn, because a layering table that is approximately true is the kind of document
this project has already had to correct once.

A consequence worth naming: the **core is useful with no model at all**. Called by Claude, this
is a verified, reversible filesystem toolkit and R6/R7 barely matter. Driven locally, the
supervisor is the product. Same core, two products — which is the strongest argument for the
split above.

**⚠ Consequence for [D6](#d6--the-sandbox-is-a-capability-type-and-resolution-is-posix-order),
and it is a real one.** D6 accepts a TOCTOU race and a hardlink gap on the grounds that "the
threat model is a confused 3B model, not a local attacker," and says to revisit "if untrusted
input ever reaches this API." A programmatic caller moves that line: paths derived from a
document someone pasted into a chat are prompt-injection-influenced input reaching a filesystem
API. Not an attacker with shell access, but no longer a confused 3B model either.

**So closing the TOCTOU race — `openat(O_NOFOLLOW)`, component at a time — is a gate on shipping
the programmatic frontend, not optional cleanup.** Roughly a day's work. The hardlink gap cannot
be closed by path checking at all and needs a separate answer (device/inode comparison against
the root, or accepting it explicitly).

**Restated as two conditions, 2026-08-15.** [D10](#d10--kernel-confinement-for-shell-landlock-vendored-one-writable-root)
supplies containment but not correctness, so the gate is both of the following and neither
alone:

| Condition | Mechanism | What it establishes |
|---|---|---|
| Containment | Landlock ruleset, forked child, one writable root | Nothing outside the root is reachable, race or no race |
| In-root correctness | `openat(O_NOFOLLOW)` component-walking | The path that was resolved is the path that is opened |

The temptation is to read the first as satisfying the gate, because "an escape is no longer
possible" sounds like the whole of it. It is not: prompt-injection-influenced input that
redirects a write from one in-root file to another is the failure this section describes, and
kernel confinement does not see it. **Both, or the frontend does not ship.**

**HTTP client: cpp-httplib, pinned.** Under this decision the client only ever talks to a
loopback backend (Ollama, and from [D9](#d9--two-local-backends-ollama-and-vllm) vLLM): no
TLS, no auth, no proxies, and headless operation makes streaming optional.
Header-only pins cleanly under D3 and adds no system dependency. libcurl would only earn its
keep against cloud backends, which this decision forecloses.

**No longer provisional, as of `src/hermes/ollama/client.cpp` (2026-08-13).** Pinned at v0.53.0
and exercised against a live daemon on `kitchen-desktop`: `/api/tags`, `/api/show` and a real
chat completion, clean under ASan and UBSan. (The chat endpoint was `/v1/chat/completions`
at the time; D8 later moved it to `/api/chat`, which does not change what the library
was asked to do.) It needed five configuration calls -- three timeouts plus
`set_follow_location(false)` and `set_keep_alive(true)` -- and one RAII wrapper to scope the
timeouts per request, and OpenSSL, Brotli and zlib are all forced
off — none has anything to do over loopback. Its one imposition is being header-only and large,
which is why it is confined to that single translation unit behind a pimpl rather than being
allowed into a header.

Recorded as low-stakes on purpose: it is one file behind a small interface, swappable in an
afternoon — unlike D4, which fifty tools inherit. It should not absorb more thought than it has.

**What would overturn it.** A caller that must reach this over a network rather than by
launching it (which changes the whole security posture, not just the transport), or a local
model good enough that the supervisor stops earning its keep — in which case the core survives
and the supervisor layer is what gets reconsidered.

## D8 — Native `/api/chat`, and the `num_ctx` clamp that has to come with it

The client speaks native Ollama throughout. `/v1/chat/completions` was used first and was
dropped on 2026-08-13.

**Why.** The OpenAI-compatible endpoint cannot set `num_ctx`. That made every model's context
window a property of its Modelfile *and* of the Ollama server's own default — and that default
turned out to be reported by no API and to have changed between releases. It is what falsified
R9's original evidence: "Ollama's default `num_ctx` is 4096" is false on 0.32.9, where an
unpinned model loads far above that instead — by a rule no API reports, and not simply the
architectural context either (`nemotron-3.5-lightning:30b` has a 1048576 architecture and
loaded unpinned at 262144). **Setting the context per
request replaces an unknowable with a stated value**, which is worth more than the compatibility.

Two smaller gains came with it, both measured against the daemon directly:

- Tool-call arguments arrive as a structured object rather than a JSON string needing a second
  parse — removing a whole failure class, and one D5 was partly written to mitigate. Measured
  against the daemon directly with `curl`, **not** through this client, which does not yet
  send or parse tool calls; that lands in Phase 2.
- `format` takes a JSON schema directly, without the `{"type":"json_schema", …}` wrapper.

**The cost, and it is not theoretical.** Being able to set the context means being able to set
it past what the GPU can hold. **Ollama performs no admission control on `num_ctx`**: an
oversized request is not rejected, not reduced, and not failed — it deadlocks the driver.
Requesting 262144 on a 29 GB model against a 48 GB W7900 hard-froze `kitchen-desktop` on
2026-08-13, with no OOM kill and no GPU reset in the logs; the kernel simply stopped.

So the clamp (`ClientOptions::max_num_ctx`, default 65536) is **part of this decision, not an
implementation detail**. Stated precisely, because that number is doing safety work: 65536 is
what the `bench/fsops` runs used **on the MSI laptop's 16 GB RTX 5080**, not on this 48 GB
W7900. It is a value known to work on a smaller card, not one derived from this one. Nothing in
the API reports free VRAM, so the client cannot compute a safe value; it can only refuse to
exceed one it was given — the clamp guarantees "at most 65536", never "fits". The clamp is tested offline, deliberately: a test that verified it by
sending an oversized request would be performing the exact action it exists to prevent.

**What would overturn it.** Ollama gaining real admission control on context size, which would
make the clamp unnecessary rather than merely conservative. Or a tool-call format divergence
that made the native shape harder to consume than the OpenAI one — the opposite of what is true
today.

---

## D9 — Two local backends: Ollama and vLLM

**Decided 2026-08-14 in principle; implementation deferred — see Sequencing.** Hermes drives
local Ollama *and* local vLLM behind one interface.
This amends D7's backend clause, which required an explicit overturn to widen. **It does not
touch D7's prohibition on cloud inference** — vLLM is admitted because it is local, and for
no other reason.

**What forced it.** Kitchen is due to move to an RTX PRO 5000 (72 GB, CUDA, single-GPU) on
2026-08-14, the same day Qwen3.8-27B's open weights were scheduled to land. **Both are stated
here as scheduled rather than done**: the cited source marks the hardware "arriving", the
model section "unverified", and no local tag exists. Specs, and an unresolved question about
whether the config those specs came from is even this model's:
`gitea-selfhost/AI-FABRIC.md § The Qwen family on Kitchen`.

That model is multimodal and uses hybrid Gated DeltaNet attention. Multimodal GGUFs ship a
separate `mmproj` projector, and Gated DeltaNet needs very recent llama.cpp operators that
Ollama lags.

> **Correction, made before this decision was a day old.** The first draft of D9 argued that
> Ollama "cannot load the model at all", making the R9 `tools` gate unevaluable, and treated
> that as the forcing argument. **The cited source refutes it**: `AI-FABRIC.md` records a
> live `qwen3.6:27b` Ollama tag and gives `qwen3.6` a "vision" role. The likely reality is
> that such models load **text-only, without the projector** — capability loss, not load
> failure. If it loads, `ollama show` runs and the gate *can* be evaluated. The strong form
> of the argument does not survive, and is recorded here rather than quietly deleted because
> the decision was taken while it was believed.

**What survives, and is sufficient.** Not "unreachable", but *late, degraded, and not ours to
schedule*: vision is lost on the Ollama path, Gated DeltaNet arrives late and slow, and the
official FP8/NVFP4 checkpoints are unusable. Generalised — GGUF conversion and Ollama's
template layer both sit downstream of the model authors, so binding to one backend means
**this project's reachable model set is set by someone else's release cadence**. That is the
durable reason; Qwen3.8-27B is the occasion that exposed it.

Qwen3.8-27B is the occasion, not the whole reason. The general form is that GGUF conversion
and Ollama's template layer both sit downstream of the model authors, and every new
architecture arrives there late. Binding to one backend means the project's reachable model
set is set by someone else's release cadence.

**Why D7's reasoning survives intact.** Every justification in D7 concerns *cloud* — the
credential pool, the egress proxy, TLS, off-machine reachability, the blast radius of a
remote endpoint. vLLM reintroduces none of it. `credential_pool.py` and `iron_proxy.py`
stay discarded for the same reason as before. Widening "local Ollama" to "local inference"
costs nothing D7 was protecting, which is why this is an amendment and not a reversal.

**Relationship to D8, which is the subtle part.** D8 chose native `/api/chat` because
`options.num_ctx` is per request. **vLLM has no per-request context override at all** —
`--max-model-len` is fixed when the server launches. This does *not* reopen what D8 closed.
D8's real objection was that the effective context became an unknowable value reported by no
API and varying between releases; a launch flag is a **stated** value, which is what D8
wanted. Context moves from per-request to per-server, and the `fsops-*:64k` Modelfile
pattern becomes a launch argument. Both designs replace the unknowable with something
written down.

**What this costs, accepted rather than elided.** D8 recorded three gains from the native
endpoint, and D9 above addresses only the first. The other two are **reopened on the vLLM
path**, and on exactly the surface where the second backend lands:

- **Tool-call arguments come back as a JSON string needing a second parse.** D8 called this
  "removing a whole failure class, and one D5 was partly written to mitigate". The OpenAI
  schema puts `function.arguments` back as a string. The failure class returns.
- **Structured output goes back through the `{"type":"json_schema", …}` wrapper** that D8
  recorded escaping.

Both are accepted, in the manner D6 accepts its TOCTOU race: written down, scoped to one
backend, and not traded away silently. The Ollama path keeps both gains.

> **D8's "what would overturn it" did not anticipate this case.** It listed Ollama gaining
> admission control, or a tool-call format divergence. It did not list *a model Ollama
> cannot load*. D8 is not overturned — native `/api/chat` remains correct for the Ollama
> backend — but its list read as exhaustive and was not. Recorded because that is the
> reusable lesson.

**What it buys beyond reach.** vLLM performs precisely the admission control whose absence
D8 had to engineer around: it preallocates the KV pool at startup and refuses to launch when
the cache will not fit, rather than accepting the request and deadlocking the driver. The
`max_num_ctx` clamp exists because "nothing in the API reports free VRAM, so the client
cannot compute a safe value."

Two precisions the first draft skipped. **The clamp does not carry over**: it is applied in
`build_chat_options` while assembling Ollama's `options` object (`client.cpp:266`), and an
OpenAI-shaped request has no such field. On a vLLM client the clamp is *absent* unless
deliberately reimplemented against `max_model_len`. And **the 2026-08-13 freeze was an
amdgpu/ROCm failure mode on the W7900** (`client.h:131`); that card is leaving, so the swap
alone makes it unreachable whichever runtime runs. Two variables changed at once — do not
credit the backend with what the hardware also did. vLLM's guarantee is narrower and still
worth having: it refuses to launch when the KV pool will not fit at the configured
`gpu_memory_utilization`, which is admission control Ollama does not perform.
It also unlocks the official FP8 and NVFP4 checkpoints — of the three runtimes considered
(Ollama, llama.cpp, vLLM), only vLLM has an NVFP4 tensor-core path. Other stacks outside that
comparison, such as TensorRT-LLM, also do.

**The shape of the work.** Not a base-URL swap — and **there is no existing interface to sit
behind**. `hermes::ollama::Client` has no virtual functions, is constructed by a static
`open()` returning a concrete type, and `hermes_supervisor`, `hermes_app` and the binary all
link `hermes_ollama` directly (`CMakeLists.txt:164,176,181`). So the work is: introduce the
abstraction, lift the shared request/reply types out of `namespace hermes::ollama`, and
re-plumb **four** CMake targets. `src/hermes/ollama/` and the `ollama.*` config namespace
(`config.cpp:284,701`) become misnomers; D7's own layering amendment, which is built on
`src/hermes/ollama/` being *the* transport singular, becomes approximate and needs revisiting
with it. The wire mapping itself is known and small: `format` → `guided_json`, `thinking` →
`reasoning_content` (`--reasoning-parser`), tool calls → `--enable-auto-tool-choice
--tool-call-parser`.

> **Corrected 2026-08-15 — the target count was three.** It counted only the top-level
> `CMakeLists.txt`. `tests/CMakeLists.txt:18` links `hermes_ollama` as well, and the test target
> has to move with the others or the suite builds against the old shape. Recorded rather than
> silently amended, because the undercount came from citing three line numbers in one file and
> then trusting the citation instead of the question.

**Two gates, both to be settled before the second client is written.**

**Gate 1 — preflight has no vLLM equivalent (R9).** `preflight.h:75` reads the `tools`
capability from Ollama's `/api/show` `capabilities` array; `ContextWindow` and
`ModelMetadata` come from the same proprietary endpoint. vLLM's OpenAI server exposes none
of it. And `preflight.h:18` states the policy: every check **fails closed**, so "I could not
determine it" is a failure, not a pass. Left alone, *every* vLLM model fails preflight. The
checks therefore need per-backend implementations, and the fail-closed policy must survive
the change rather than being quietly relaxed to accommodate a backend that cannot answer.
This gate is the same class as the one below and was missed on the first pass.

**Gate 2 — where the context window lives.** `ClientOptions` and the `max_num_ctx` clamp both
assume context is a per-request property; under vLLM it is a property of the running server.
This reaches further than one field: `session.cpp:93` seeds `echo.max_num_ctx` from
`client.max_num_ctx()`, `:104` computes the window as `min(options.num_ctx, max_num_ctx)`,
and `main.cpp:186` seeds the request from the same value. Against vLLM, `max_num_ctx()`
would report 65536 while the server's real window is `--max-model-len` — possibly smaller —
and a budget planned against the larger of the two walks into the overflow cliff recorded at
`ROADMAP.md` (1.10x over → ~1% of the prompt survives).

**This is a plumbing problem, not an unknowability problem**, and the distinction points at
the answer. An earlier draft said silently ignoring a request-level `num_ctx` would "rebuild
the unknowable-context problem D8 exists to prevent". That is too strong: vLLM *reports*
`max_model_len` per model on `GET /v1/models`, so the effective window is queryable at
runtime — strictly better than Ollama, where D8's whole complaint was that it was "reported
by no API". vLLM also rejects an over-length prompt with a 400 rather than silently
truncating. So the third option, absent from that draft and better than either no-op or
error, is: **read `max_model_len` at startup and populate the same budget field the clamp
feeds.** That is closer to D8's intent than refusing to model the difference.
**Settle it before writing the second client, not during.**

**Sequencing — deliberately deferred, and this is part of the decision.** Kitchen's default
runtime is **Ollama**, settled 2026-08-14; vLLM is the situational backend for models built
and trained to run on it. That answer is what sets the timing, because D7 enforces loopback:
Hermes runs on the same box as its backend, so an Ollama-only Hermes is only *blocked* when
Kitchen is toggled to vLLM. While Ollama is the default, it is not blocked.

So the implementation does **not** land in Phase 2. Phase 2's job is the core loop and
minimal tools, and the agent loop does not exist yet; building a second transport before the
first one has a working loop is the wrong altitude. **The Phase 2 obligation is narrower:
do not foreclose this.** When the client is touched, do not add new per-request assumptions
beyond those already recorded — that is cheap now and expensive to retrofit.

Revisit when either of two things is known: whether the Qwen family actually loads and
reports `tools` under Ollama, or whether Kitchen's default flips to vLLM. **If the default
flips, this becomes blocking work rather than deferred work** — that is the trigger to watch,
and it is a decision about Kitchen, not about this repo.

**What would overturn it.** Ollama gaining timely support for the architectures that forced
this — multimodal `mmproj` loading and current llama.cpp operators — which would make the
second backend redundant rather than merely unused. Or the two-backend interface proving to
cost more in the core than the reachable-model gain is worth, measured rather than assumed.

## D10 — Kernel confinement for shell: Landlock, vendored, one writable root

**Decided 2026-08-15.** The shell tool runs under a Landlock ruleset installed in a forked
child immediately before `execvp`; the parent stays unrestricted. The mechanism is vendored
from `landlock-run` (BSD-3-Clause, ~300 lines of C11 over the raw kernel UAPI) rather than
shipped as a helper binary, so [D3](#d3--dependencies-fetchcontent-pinned)'s single-artifact
property survives.

**Where the source is.** Recorded precisely because it is not vendored yet and is not findable
by guessing — it lives several directories inside an unrelated TypeScript monorepo:

| | |
|---|---|
| repo | `https://github.com/deepseek-ai/deepseek-harness.git` |
| path | `native/landlock-run/packages/entry/src/main.c` |
| commit | `6e05cb7ff5bc9834fcf303800264fa3cdb3724e8` (2026-08-13) |
| sha256 | `c2d6f330e31924ccba7c9b70416e05b427bb7aacfc21dcd8d610cf22c20bd53a` |
| license | BSD-3-Clause — *not* the MIT of its parent repository |

The file is self-contained: it defines the Landlock UAPI locally rather than including
`<linux/landlock.h>`, deliberately, so the audit surface is that one file plus the kernel's
stable syscall contract. Nothing else in that monorepo is needed. A blobless clone costs
seconds; do not treat the absence of a local checkout as a blocker.

**What forced it.** [ROUTING.md](./ROUTING.md) §4 records that `shell` is "the one tool that
cannot be R1-correct by construction": every other tool takes a `SandboxPath`, which only
`Sandbox::resolve` can build, while a shell command is an opaque string with no path argument
to resolve. That is true of *path-based* containment and false of *kernel* containment.
Landlock is an allow-list evaluated by the kernel at open time, and the ruleset is inherited
across `execve`, so the command and every process it spawns are confined.

Verified on Kitchen (`cachyos-kitchen-pc-x8664`, RTX PRO 5000), kernel 7.1.8: ABI 9, `landlock`
active in `/sys/kernel/security/lsm`, a `setsid --fork` grandchild confined, and `no_new_privs`
propagating to grandchildren.

**Exactly one writable directory: the sandbox root.** `LANDLOCK_ACCESS_FS_REFER` is the opt-in
that *permits* reparenting across rule hierarchies — not, as it first appears, the mechanism
that forbids it — and the launcher grants it with every `--rw`. Two writable directory roots
therefore let a confined shell hardlink or rename between them, which was measured rather than
inferred. With one root there is nothing to pair with. Two consequences, both load-bearing:

- **Temp space lives inside the root.** A `--rw /tmp` rebuilds the pair, and a `rename` out of
  the verified tree reads to R3's hash-diff as a *deletion* — a state change the model can make
  that verification then misreads.
- **R4's backups are never granted to the child.** R4 is a supervisor concern: ROUTING.md §6
  puts snapshot, hash-diff and poll at the per-turn layer, which is the unrestricted parent. So
  backups need no grant, no broker and no read-only path, and may live wherever is convenient
  including outside the root. This answers what ROUTING.md §11 recorded as undesigned.

`--rw /dev/null` is safe despite the one-root rule, because the launcher narrows non-directory
grants to file-compatible bits and `REFER` is not among them. It looks like an exception and is
not — worth a comment at the vendoring site, since the next reader will ask.

**The grant set, and why each entry is there.** Landlock denies everything not granted, so this
is not tuning: omit `/usr` and the launcher cannot `execvp` at all. Each row below was measured
by removing it.

| Grant | Why |
|---|---|
| `--rw <sandbox root>` | The work. The only writable *directory*, per above |
| `--ro /usr` | `ld.so` and the shared libraries; covers `/lib64` by symlink resolution, and locale under `/usr/lib/locale`. Without it: `exec failed: Permission denied` |
| `--ro /etc` | Ordinary tooling reads it — `nsswitch.conf`, `passwd`, `gitconfig`. Also required for name resolution, **including `localhost`**: `getaddrinfo("localhost")` fails without `/etc/hosts` while `127.0.0.1` succeeds |
| `--ro /proc` | Self-inspection. Without it `/proc/self/maps` and friends are denied |
| `--rw /dev/null` | Denied unless granted explicitly; a great deal of tooling redirects to it |
| `--ro /dev/urandom` | Same — denied unless named |

Temp space is inside the sandbox root, per above. Note that `mkstemp` and any `O_TRUNC` open
need `MAKE_REG` **and** `TRUNCATE`, so a grant that omits truncate breaks file creation in a way
that reads as a permissions bug rather than a policy one.

This is the **child's** grant set. The parent is unrestricted, so its Ollama connection, its
config reads and its R4 backup writes are unaffected and must not be granted here — granting
them would be the second writable root this decision exists to prevent.

**What this does not close.** Stated at length because the mechanism's own documentation invites
the opposite reading.

- **In-root redirection.** Landlock catches escapes *out of* the root.
  [D6](#d6--the-sandbox-is-a-capability-type-and-resolution-is-posix-order)'s worked example —
  `link -> a/b`, `link/../deep.txt` resolving to `root/deep.txt`, "right filename, wrong
  directory" — is entirely inside the root and is permitted. So were both destructive `05_copy`
  incidents. `openat(O_NOFOLLOW)` remains required; D7's gate is now stated as two conditions.
- **Pre-existing hardlinks.** Creation is blocked by the one-root rule. A link planted inside
  the root before the sandbox starts remains reachable by its in-root path, and writable
  through it.
- **Egress, and it is not small.** Only `handled_access_fs` is set. `handled_access_net` and
  `scoped` are zero, and `FS_RESOLVE_UNIX` (ABI 9) sits outside the launcher's `MAX_ABI 5`
  mask. A confined process reached the public internet with DNS and TLS working, and can talk
  to pathname unix sockets. **Accepted while `shell` stays off the MCP surface** — see
  ROUTING.md §8. If that changes, the net bits are governed first.
- **"Everything else is denied" is false.** Landlock governs only what it hooks. `chmod`,
  `chown`, `utimes`, `stat` and `access` are not hooked: a confined process changed both the
  mode and the mtime of a file it held no grant on. This is why the observed-state guard in
  ROUTING.md §4 keys on `ctime`, which cannot be set directly, rather than on `mtime` alone.
- **Descriptors opened before the restriction keep working, and they are inherited.** Measured:
  `read` and `write` on a file descriptor opened before `restrict_self` succeed even when the
  path is outside every grant, and an established TCP connection survives intact. Re-`open`ing
  the same path is denied — it is the *open* that is hooked, not the descriptor.
  **This is an implementation obligation, not a footnote.** Any descriptor the parent holds when
  it forks — the Ollama socket, the config file, a log — is inherited by the confined child and
  is a hole in the sandbox that no grant describes. **Set `O_CLOEXEC` on everything the parent
  opens**, and audit `/proc/self/fd` before the first spawn rather than trusting that.
  One asymmetry is worth knowing because it is unintuitive: a held `O_PATH` *directory*
  descriptor does **not** launder `openat` — the kernel re-derives the ancestry — so only
  already-open file descriptions carry rights forward.

**"Fully enforced" is a claim about the binary, not the kernel.** The launcher reports it
whenever `abi >= MAX_ABI`, which on an ABI-9 kernel is unconditional while four ABI levels go
ungoverned. Where the tool surface gates on enforcement it gates on our own probe and our own
vocabulary, never on that string.

**The probe attempts a denied write.** Upstream's probe runs `true` and checks for exit 0,
which establishes that the profile was *accepted* — not that a forbidden access is *refused*.
Ours attempts a write outside the grant and requires `EACCES`. This is the same argument R9's
preflight already makes about asking the daemon rather than inferring from a version.

**Cost, measured rather than assumed.** Setup is 5 fixed syscalls plus 4 per granted path.
Steady state is +250–390 ns per `open()`, roughly 40–55%. That is real and here it is
structurally irrelevant: only the shell child is confined, while R3's hash-diff, R4's snapshot
and R6's polls all run in the unrestricted parent.

**Why not restrict the supervisor itself.** Proposed, and rejected on measurement — recorded
because it is the intuitive design and will be proposed again. `landlock_restrict_self`
restricts the **calling thread**, not the process: with `flags = 0`, threads that already exist
stay entirely unconfined. [D1](#d1--concurrency-blocking-and-single-threaded) describes the
agent loop, not the process, and a threaded resolver inside the HTTP client is enough to leave
the hole open. `LANDLOCK_RESTRICT_SELF_TSYNC` fixes it and needs ABI 8, which a `MAX_ABI 5`
build never requests. Restriction is also monotonic, permanent and capped at 16 layers, so a
single startup grant would have to be the union of everything any tool might ever need —
strictly weaker confinement than a per-command grant, bought for sub-microsecond savings per
spawn.

**Attribution.** `landlock-run` is BSD-3-Clause while its parent repository is MIT. The
copyright notice, the conditions and the non-endorsement clause travel with the vendored code,
in the manner the README already requires for anything closely transliterated.

**What would overturn it.** A confinement mechanism that also governs in-root redirection,
which would collapse this and D7's `O_NOFOLLOW` condition into a single condition. Or a port to
a platform with no Landlock equivalent, which makes the mechanism per-platform and forces the
tool surface to gate on the probe result rather than on presence.

---

## D11 — The substrate is probed, not assumed

**Decided 2026-08-16.** Every guarantee this codebase makes is a claim about *a particular
filesystem*. R1's containment, R3's hash-diff, R4's snapshot, R5's read-back and
[ROUTING.md](./ROUTING.md) §4's identity tuple all rest on POSIX semantics that `ext4` provides
and other substrates do not. **A `Sandbox` therefore probes the ground it is rooted on and
records which guarantees hold, rather than assuming them.**

**What forced it.** ROUTING.md §4 settles `dev:ino:size:mtime:ctime` as the single identity
currency shared by `list`, the staleness guard and `edit`'s fail-closed check — and its
justification is explicitly semantic:

- `dev:ino` "catches a file that was unlinked and recreated, or a symlink retargeted to a
  different file — replacement with byte-identical content, which a hash cannot see at all."
- `ctime` "is not settable directly", which is exactly what makes it trustworthy where `mtime`
  is forgeable — a property [D10](#d10--kernel-confinement-for-shell-landlock-vendored-one-writable-root)
  established by measuring a confined process calling `utimes`.

**Both properties are substrate-dependent, and neither was checked anywhere.** On a 9p/DrvFs
mount — WSL2's view of a Windows drive, which is where a Windows developer's repository actually
lives — inode stability is not guaranteed and the metadata is emulated. On NFS and CIFS, inode
reuse and coarse timestamp granularity are both live concerns. **The failure is silent and it
lands on the one mechanism designed to fail closed:** an identity tuple that cannot be trusted
turns `edit`'s stale-target guard into a spurious refusal at best, and a pass at worst.

**Why a probe rather than a mount-type table.** The same argument D10 makes about Landlock and
R9 makes about the daemon. A `statfs` `f_type` lookup is a version-string check by another name:
it reports what the kernel *calls* the mount, not whether `link()` works on it, and it cannot
describe a substrate nobody has enumerated yet. Attempt the operation; read the result.

**What is probed.** Each is a falsifiable operation inside the sandbox root, never an inference:

| Property | Probe | What it guards |
|---|---|---|
| case sensitivity | create `a`, `stat("A")` | `edit` targeting, D6 resolution |
| `dev:ino` stability | stat, close, reopen, compare | the identity tuple |
| hardlink support | `link()`, read `errno` | D10's one-writable-root reasoning |
| symlink + `O_NOFOLLOW` | create a link, open with the flag | D6 resolution, D7's gate |
| `ctime` behaviour | `chmod` and confirm it moved; attempt to set it directly | the staleness guard |
| `O_TRUNC` / `mkstemp` | create, then truncate | D10 records this as a trap already |
| `EACCES` fidelity | attempt a denied read | R1's fail-closed resolution |

**Fail closed, per R9's policy.** "I could not determine it" is a failure, not a pass. A
substrate that cannot answer is one Hermes declines to root on, with the reason reported **as
data** — the same vocabulary ROUTING.md §8 already established for the confinement probe, and
for the same reason: the difference between substrates becomes something the binary reports,
never a second build or a stripped tool surface.

**Where it lives.** `hermes_core`, beside `Sandbox`. It touches no model and no network, so it is
Tier 0 by ROUTING.md §2 and needs no new link edge — it is implementable before the two edges in
ROUTING.md §12 step 1.

**This is a Linux feature first, not a Windows one.** It answers `/mnt/c` under WSL, network
mounts, `tmpfs`, and an overlayfs inside a container, uniformly and today. Its effect on WSL is
worth stating plainly because it is the case that prompted it: **WSL becomes usable on its Linux
side and explicitly refused on its Windows side**, rather than under-delivering silently across
a boundary already known to be lossy.

**What would overturn it.** A probe expensive enough to matter at sandbox construction — unlikely,
since it is a handful of syscalls once per root, against the 250–390 ns per `open()` D10 already
accepted in steady state.

---

## D12 — Tool calls are native; `format` is never sent alongside `tools`

**Decided 2026-08-17**, building Phase 2's agent loop, from measurement rather than reasoning.

Tool calls go over Ollama's native `tools` array and come back in `message.tool_calls`. The
`format` schema is **never** sent in the same request. On `ChatRequest` the two are a
`std::variant`, so sending both is unrepresentable rather than merely discouraged.

**Why, and this is the whole argument.** `format` and `tools` are not complementary. Measured on
`cachyos-x8664` (RTX 5080 Laptop 16 GB), Ollama 0.32.9 — the same version every other figure in
this file was taken on — temperature 0, two repeats each, one `write` tool offered against a
`format` schema describing that very same call:

| model | `tools` alone | `tools` + `format` |
|---|---|---|
| `qwen3.5-9b` | correct | correct — `format` ignored, `eval_count` identical |
| `qwen3.5-4b` | correct | correct — `format` ignored, `eval_count` identical |
| `gemma4-e4b` | correct | correct |
| `llama3.2-3b` | correct | **corrupt arguments** |
| `llama3.1-8b` | correct | **corrupt arguments** |
| `hermes3-8b` | correct | **no call at all** |
| `granite4-7b` | correct | **no call at all** |

Seven of seven models emit a correct call with `tools` alone. **Four of seven break when `format`
is added**, in two distinct ways:

- **Corrupt arguments.** The model generates JSON conforming to the `format` schema, and Ollama's
  template tool-parser then treats that whole object as the call's arguments — `{"tool": "write",
  "path": "hello.txt", "content": "HERMES-OK"}`. Well-formed, dispatchable, and wrong. With a
  schema *unrelated* to the tool the corruption is total: the arguments came back as
  `{"quokka": 0, "verdict": "{\"name\":\"write\",\"parameters\":{...}"}` — the real call
  buried inside a string.
- **No call at all.** The schema-conforming JSON lands in `content` and `tool_calls` is empty.

**The second failure is R2's own failure, reintroduced by R2's own mechanism.** R2 was written
because `llama3.2:3b` *"emitted a whole tool call as plain prose that was never parsed as a call"*.
That is precisely what `hermes3-8b` and `granite4-7b` do here when `format` is added. And the model
whose misbehaviour supplied R2's evidence — `llama3.2:3b` — is itself one of the four that break.

**So D5's stated overturn condition is met, by its own terms.** D5 asked for *"evidence that
constraining `format` measurably degrades tool-call quality on the models actually used"*, and
called that a real risk worth measuring on the fsops harness rather than assuming either way.
Measured: 100% → 0% on four of the seven models in that harness.

**What survives of D5, and it is not a consolation prize.** `format` works exactly as documented
when no tools are offered — the same run confirmed a clean schema-constrained reply on every model
probed. So D5 is *retargeted*, not overturned: `format` is the mechanism for a structured **reply**,
which is what Tier 1's `triage` and `summarize` will want, and it has no business on the
tool-argument path.

**What replaces it there.** R2's intent — arguments that are structurally valid — is met by two
things that do not fight the model:

1. The model's own trained tool-call channel, which is what the 7-of-7 column measures.
2. `parse_args` failing closed against the declaration, which is where the schema leakage above
   is caught: `tool` is refused as an unknown argument, by name, in a message the model can act
   on. There is a test asserting exactly that, so the D12 story stays executable.

Note what is *not* claimed. This does not make arguments correct, only well-formed — the same
limit D5 stated about itself. A well-formed call to delete the wrong file is untouched, and that
is still what the supervisor is for.

**What would overturn it.** A future Ollama that honours `format` and `tools` together coherently
on every model in use — in which case the variant becomes one line, and the table above becomes
the reason it existed. Re-measure before believing a release note; this behaviour is not
documented either way, which is why it had to be probed.

---

## Still open

### ~~Which chat endpoint~~ — settled as D8

### ~~The hardlink gap~~ — halved by D10, and the remainder is now decided

D7 made this concrete rather than theoretical. A hardlink inside the root pointing at an outside
file is accepted, and no path-based check can detect it — `resolve()` is doing its job correctly
and still lets it through.

[D10](#d10--kernel-confinement-for-shell-landlock-vendored-one-writable-root) splits it in two
and answers both halves, which is why this is no longer open:

- **Creation is blocked.** Cross-hierarchy link and rename are denied in every Landlock domain
  unless `REFER` is granted on both ends, and the one-writable-root rule means there is no
  second end. A confined shell cannot build the escape.
- **A pre-existing link is accepted, explicitly.** Landlock evaluates the ancestry of the path
  actually walked, so a link planted inside the root before the sandbox starts is readable *and
  writable* through its in-root name. Device/inode comparison against the root would catch it
  and is not being done: it costs a `stat` on every resolution to defend against an actor who
  already had write access to the root beforehand — which is not this project's threat model,
  and which R4's snapshot and R3's hash-diff would surface after the fact regardless.

Revisit only if the threat model widens to an actor with prior filesystem access, which is a
larger change than this entry.

### ~~The trim loop and tool results~~ — closed 2026-08-17, when Phase 2 made it real

**Noticed 2026-08-15**, from Prime Agent's compaction rules. Their cut-point selection never cuts
at a tool result, because a result must stay with the call that produced it.

`Session::prepare()`'s trim loop erases the first unpinned turn one at a time, and
`pin_latest_user()` pins only the system message and the most recent user turn — an assistant
turn is explicitly unpinned. **Today this cannot bite**: `ollama::ChatMessage` is `{role,
content}`, nothing in `src/` defines or parses a tool call, and the only roles reaching `turns_`
are `system`, `user` and `assistant`.

It bites the moment Phase 2 adds tool messages. A trim that drops a result while keeping its
call leaves the model looking at an orphaned request, and the reliable response to that is to
re-issue it — which is the repeat-call loop the supervisor exists to break. Recorded here rather
than fixed, because there is nothing yet to fix.

**Phase 2 arrived, and this is now fixed rather than noted.** The unit of dropping is a *group*:
an assistant turn carrying `tool_calls` together with every `tool` turn immediately following it.
`prepare()` erases groups, never turns, so neither half of the split can happen. `dropped()` still
counts turns, so the number stays comparable with a tool-free session.

Two things this entry did not anticipate, both found while fixing it:

- **`record()` was dropping `reply.tool_calls` on the floor.** The assistant turn went into
  history as `{role, content}` only, so the call was never *in* history to be orphaned — the
  model would have seen results with no visible request from the very first turn, before any
  trimming. Grouping alone would not have helped; the calls had to be carried too.
- **An oversized tool result cannot be refused by the Session, and is silently dropped.**
  `pin_latest_user` pins only `system` and the latest `user`, so a result is *always* droppable;
  a result too large for the whole budget therefore takes its own call down with it, and the
  model — seeing neither — re-issues the same call and gets the same result. Coherent history, no
  progress. The loop substitutes a refusal naming the size before such a result reaches history
  (`result_is_hopeless`), which converts it into one line the model can act on. The underlying
  mismatch is left open and named: `read`'s cap is a 16 MB filesystem-safety limit with no
  relation to any context window, and ROUTING.md §9 makes it configuration, so wiring the cap to
  the window is a composition decision nobody has made yet.

### Tool definitions vanish from some templates after a tool result

**Measured 2026-08-17**, building the agent loop, and it is a model-selection criterion
rather than a bug in anything this repo owns.

Ollama 0.32.9, one `read` tool offered, comparing the prompt with and without the `tools`
array so the difference is the definitions themselves:

| model | tools cost, user last | tools cost, tool result last | |
|---|---|---|---|
| `qwen3.5-9b` | +267 | +267 | kept |
| `qwen3.5-4b` | +267 | +267 | kept |
| `hermes3-8b` | +208 | +208 | kept |
| `granite4-7b` | +162 | +162 | kept |
| `gemma4-e4b` | +55 | +55 | kept |
| `llama3.2-3b` | +133 | **+31** | **lost** |
| `llama3.1-8b` | +146 | **+42** | **lost** |

On the two stock Meta llama3.x instruct templates the definitions are not rendered when
the last message is a `tool` result — exactly the turn on which the model must decide
whether to call another tool. It is a property of the **template, not the architecture**:
`hermes3-8b` is llama3.1 underneath and keeps its definitions, because Nous ships its own
template.

**The failure it produces was seen before it was explained.** A live `hermes-cpp agent`
run on `llama3.2-3b` sent an 832-token prompt on turn 1 and a **160**-token prompt on turn
2 — smaller, while history had grown — and the model answered with a tool call written as
prose: `{"name": "read", "parameters": {"paths": "['colors.txt', 'count.txt']"}}`. A
Python-style list inside a string, which is verbatim the failure R2 was written about. The
672-token drop is about eight tool definitions, which is what was being offered.

Worth noting against [D5](#d5--constrained-decoding-on-from-the-start): `format` could not
have fixed that call. It was not a constrained generation gone wrong — it was prose, on a
turn where the model had not been told any tools existed.

**Three options, none chosen:**

1. **Select models by this property.** Cheapest, and it folds into the open
   "which models, on which machines" question in ROADMAP.md — this is a concrete criterion
   where that question previously had only size and speed.
2. **Detect it in R9's preflight.** Two requests per model at startup, differing only in
   whether a tool result is last, comparing the token delta. That is the shape of the
   measurement above, so it is known to work; the cost is two extra generations per run
   and a gate that refuses models the fsops harness has real scores for.
3. **Append a synthetic user turn after the results**, which restores the definitions
   (measured: the +133 returns). **Rejected on inspection, not deferred**: it puts words in
   the user's mouth in history, and `pin_latest_user` would then pin the synthetic nudge
   instead of the real instruction — so the one message the trim must never drop becomes a
   fabricated one. A worse failure than the one it fixes.

Revisit when model selection is settled, which is where this belongs.

### Test oracle

Upstream ships 2,889 test files. Whether any are worth adapting as a behavioural spec is still
open, and is mostly a question about how much behaviour is genuinely shared — which the
module-level scope work suggests is not much.

### A bounded-execution primitive, or per-site timeouts

**Noticed 2026-08-14**, reviewing upstream `v2026.8.13..main` (185 commits, 21 touching
in-scope paths). Upstream landed `agent/deadline.py` — 471 lines plus 401 lines of tests —
and explicitly framed it as *"one shared foundation for the timeout/hang backlog instead of
per-incident site-local fixes"*. Four follow-up commits already build on it, and the commit
message says later phases migrate tool execution, MCP, and subprocess call sites onto it.

That is a direct statement about this project's subject matter. This repo's one-line
description is a supervisor for local models doing **bounded** filesystem work; upstream has
just concluded that bounding needs to be a primitive rather than a property of each call site.

The parts that look load-bearing rather than Python-specific:

- **A deadline that survives a blocked executor.** Upstream uses a thread timer because an
  async deadline is worthless when the event loop is the thing that wedged. D1 makes this
  blocking and single-threaded, so the shape differs — but the failure mode does not, and a
  timeout enforced by the same thread doing the work cannot fire.
- **Whole-tree process termination.** Killing the direct child leaves orphans. This is
  squarely a `terminal_tool` / `process_registry` concern and both are already in scope.
- **Our deadline vs. the provider's, mechanically distinct.** Upstream raises a dedicated
  `DeadlineExpired` rather than letting a supervisor timeout look like a model timeout. That
  distinction is a classification decision, and `error_classifier` is in scope.
- **Clamping at the boundary**, to kill an integer-overflow class before it reaches a syscall.

**Not decided here.** Adopting a shared primitive before there are call sites to share it
between is the classic premature abstraction, and D1's threading model means at least the
first item cannot be copied. But the question is now on the record with the evidence, rather
than being rediscovered when the third hang gets its own bespoke fix.

`agent/deadline.py` is tracked in `parity.tsv` as of this note so the ledger stops missing the
file upstream is actively building out.
