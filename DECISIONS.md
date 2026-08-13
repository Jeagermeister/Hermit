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

**Not yet in the build.** No source parses JSON, so the dependency is deliberately not declared
in `CMakeLists.txt` — it would cost a clone per build directory for nothing. It arrives with the
Ollama client.

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

**Known and accepted.** A TOCTOU window remains between `resolve` and `open` — closing it needs
`openat(O_NOFOLLOW)` component-walking. The threat model is a confused 3B model, not a local
attacker. Revisit if untrusted input ever reaches this API. Hardlinks to outside files placed
inside the root are also accepted; no path-based check can catch that.

**What would overturn it.** Untrusted input, which promotes the TOCTOU race and the hardlink
gap from documented to blocking.

## D7 — Local inference only; two ways in, human and machine

**Backend: local Ollama, and nothing else.** No cloud inference provider, ever, unless this
decision is explicitly overturned.

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
| **supervisor** | drives local Ollama; bounded sessions, retry (R6, R7) | the only layer with an HTTP client |
| **frontends** | CLI today, MCP-over-stdio next | neither of the above, beyond a small API |

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

**HTTP client: cpp-httplib, pinned.** Provisional until the Ollama client is actually written.
Under this decision the client only ever talks to loopback Ollama: no TLS, no auth, no proxies,
and headless operation makes streaming optional. Header-only pins cleanly under D3 and adds no
system dependency. libcurl would only earn its keep against cloud backends, which this decision
forecloses.

Recorded as low-stakes on purpose: it is one file behind a small interface, swappable in an
afternoon — unlike D4, which fifty tools inherit. It should not absorb more thought than it has.

**What would overturn it.** A caller that must reach this over a network rather than by
launching it (which changes the whole security posture, not just the transport), or a local
model good enough that the supervisor stops earning its keep — in which case the core survives
and the supervisor layer is what gets reconsidered.

---

## Still open

### HTTP client — settled provisionally in D7

Resolved by [D7](#d7--local-inference-only-two-ways-in-human-and-machine): cpp-httplib, pinned.
Local-only inference means loopback, so the libcurl case — TLS, proxies, auth, robust streaming
— never arises. Confirm when the Ollama client is written; the cost of being wrong is one file.

### The hardlink gap

D7 makes this concrete rather than theoretical. A hardlink inside the root pointing at an
outside file is accepted, and no path-based check can detect it — `resolve()` is doing its job
correctly and still lets it through. Options are device/inode comparison against the root, or
accepting it explicitly and writing down why. Undecided.

### Test oracle

Upstream ships 2,889 test files. Whether any are worth adapting as a behavioural spec is still
open, and is mostly a question about how much behaviour is genuinely shared — which the
module-level scope work suggests is not much.
