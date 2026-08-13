# Decisions

The hard-to-reverse choices [ROADMAP.md](./ROADMAP.md) asked to have argued out before code
exists. Each records what would overturn it, so a later disagreement can be settled on evidence
rather than re-litigated from scratch.

Settled 2026-08-13, at the point the first code was written.

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

---

## Still open

### HTTP client — not yet decided

Deferred because nothing written so far touches the network. The realistic candidates are
**libcurl** (already on the system at 8.21.0, handles streaming well, adds a system dependency
against D3's grain) and a **header-only client** such as cpp-httplib (pins cleanly, one less
system dependency, weaker streaming story).

The deciding question is whether responses are streamed. Streaming matters for a human watching
a TUI; this runs headless and blocks on the whole reply anyway, which argues the requirement is
weaker here than it first appears. Settle it when the Ollama client is written.

### Test oracle

Upstream ships 2,877 test files. Whether any are worth adapting as a behavioural spec is still
open, and is mostly a question about how much behaviour is genuinely shared — which the
module-level scope work suggests is not much.
