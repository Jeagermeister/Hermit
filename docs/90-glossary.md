# 90. Glossary

The project's working vocabulary, one line each. R-numbers are requirements
([REQUIREMENTS.md](../REQUIREMENTS.md)); D-numbers are decisions
([DECISIONS.md](../DECISIONS.md)) — the links are the binding versions.

## The requirements

| | one line |
|---|---|
| **R1** | One sandbox root, every path resolved against it explicitly; escapes refused, never rebased |
| **R2** | Tool arguments are structurally valid — by the model's native tool channel plus fail-closed parsing (see D12) |
| **R3** | Verify by content hash, never by existence |
| **R4** | Back up before mutating; undo is a first-class operation |
| **R5** | Read back after every write and compare before the turn succeeds |
| **R6** | Never trust a completion claim; poll the filesystem |
| **R7** | Retry with one concrete remaining failure, fresh session each time |
| **R8** | Bound turns by wall clock, not just turn count; a timeout is a failure |
| **R9** | Preflight the model against the live daemon before running anything |

## The decisions

| | one line |
|---|---|
| **D1** | Blocking and single-threaded; parallelism is more processes on disjoint roots |
| **D2** | nlohmann/json, pinned |
| **D3** | Dependencies fetched at pinned versions at configure time |
| **D4** | Tools: virtual dispatch plus one declarative descriptor list that renders every schema |
| **D5** | Constrained decoding on tool arguments — overturned in part by measurement; see D12 |
| **D6** | The sandbox is a capability type; resolution is POSIX-order; opens walk `openat(O_NOFOLLOW)` per component |
| **D7** | Local inference only; two front doors (CLI and MCP-over-stdio); loopback only |
| **D8** | Native `/api/chat`, context set per request, and the `max_num_ctx` safety clamp |
| **D9** | Two local backends in principle (Ollama and vLLM); vLLM deferred until Kitchen needs it |
| **D10** | Kernel confinement for shell: Landlock, vendored, exactly one writable root |
| **D11** | The substrate is probed, not assumed — guarantees are claims about a particular filesystem |
| **D12** | Tool calls are native; `format` is never sent alongside `tools` (measured: it breaks four of seven models) |
| **D13** | Per-turn verification observes the filesystem, never the reply; hashes carry forward by identity tuple |
| **D14** | Undo is list-first and never destructive; retention is 72 h and automatic |
| **D15** | Meaning is judged by a model, after structure, and labelled as judgment everywhere |
| **D16** | LTO is measured, real, and opt-in (`HERMIT_LTO`) |
| **D17** | Compaction rebuilds the context window from the tree once it fills; it never summarizes |
| **D18** | Ollama Cloud admitted narrowly, behind `--allow-cloud`; the local daemon is the only new egress point |
| **D19** | `delete` is admitted: opt-in, gated on observation, backed up first; dry-run is decided against |

## Terms

- **Sandbox root** — the one directory a run may touch, always explicit, never defaulted or
  inferred.
- **`SandboxPath`** — the only path type tools accept; constructible only by the resolver, so
  containment is a compile-time property.
- **Identity tuple** — `dev:ino:size:mtime:ctime`, the currency shared by `list`, the
  staleness guard, and the per-turn diff. `dev:ino` catches unlink-and-recreate;
  `ctime` is the field that cannot be set directly.
- **Observed state / staleness guard** — per-session memory of what has been seen. A fresh
  session has observed nothing and must read (or list) a file before it may edit it; a
  mutation against a moved tuple is refused.
- **Changeset** — the per-turn hash-verified diff of the whole tree: created, deleted,
  modified, touched (tuple moved, content identical), and permissions-changed. Owes nothing
  to the model's reply.
- **Baseline** — the snapshot taken before the first attempt; every attempt and every
  `preserved:` expectation is judged against it.
- **Expectation** — a stated post-condition (`exists:`, `dir:`, `absent:`, `preserved:`,
  `identical:`, `satisfies:`); the judge's input and the retry's vocabulary.
- **Verdict** — the per-expectation outcome set: met, unmet (feeds retry), undecidable
  (reported, never retried).
- **Judge** — structural: a pure function of two snapshots. Semantic: a model in a fresh
  session reading the file's bytes and the tree's paths, labelled "(the model's judgment,
  not a measurement)".
- **Generation** — one preserved pre-mutation file body in the backup store; numbered, never
  overwritten, never renumbered.
- **Tier 0 / 1 / 2** — execute without inference / reason locally / decline and return
  context. The deciding rule: one correct answer computable from the inputs → Tier 0.
- **Preflight** — R9's live-daemon checks: context floor, tools capability, optional warmup.
  Fails closed.
- **The clamp** — `max_num_ctx`, the hard ceiling on any context size sent; exists because an
  oversized request has frozen a machine rather than failing.
- **Confinement probe** — attempts a write outside the grant and requires `EACCES`; the only
  evidence accepted that Landlock is actually enforcing.
- **The context cliff** — Ollama's overflow behaviour: one token past the window silently
  costs nearly the whole prompt. Prevention is the only strategy; that is the session
  machinery's job.
- **Compaction** — rebuilding the context window from the filesystem once the prompt reaches
  80% of its budget: task kept verbatim, changed paths re-read, unmet requirements restated,
  the model's own narration discarded. Deliberately *not* summarisation — a summary would be
  the model's account of events, which is what verification exists not to trust. Needs a
  verifier; without one the trim is the fallback.
- **The trim** — the older, cheaper policy underneath compaction: drop whole call-and-result
  groups off the front until the prompt fits. Still the backstop, and still what runs when
  there is no tree to rebuild from. Counted separately (`dropped`) from compaction
  (`rebuilt`), because history lost in silence is not the same as history replaced.
- **Bounded session** — one instruction, capped turns, capped wall clock, capped calls per
  turn; the architecture's unit of work.
- **Front doors** — the human CLI, and MCP over stdio for a programmatic caller.
