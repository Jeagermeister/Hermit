# 3. A tour of the architecture

This chapter walks the moving parts in the order a request meets them. The diagrams live in
[FLOW.md](../FLOW.md); the tool surface's binding definition is [ROUTING.md](../ROUTING.md);
the hard-to-reverse choices are [DECISIONS.md](../DECISIONS.md).

## There is no router

Requests are typed, not natural language. A caller selecting a tool *has* routed — the tool
schema is the routing table, and nothing in Hermit is a component that decides where work goes
([ROUTING.md](../ROUTING.md) §1). Work falls into three tiers by one rule: **one correct
answer computable from the inputs goes to Tier 0; choosing among defensible answers goes to
Tier 1, or escalates.**

| Tier | What it is | Cost |
|---|---|---|
| **0 — Execute** | Filesystem operations with verification. No inference. | syscall latency |
| **1 — Reason locally** | The local model under the supervisor loop (`triage`, `summarize` — designed, not yet built) | seconds to minutes |
| **2 — Escalate** | Decline, returning *context* rather than an answer | none |

Tier 2 is a first-class path, not an error branch: "I could not answer this, but here are the
4 relevant files out of 200" is a smaller problem handed back. And a corollary that matters
more than the rule: **a Tier 0 tool never silently falls back to something adjacent.** No
answer is a valid answer; a tool that quietly succeeds at something near what was asked makes
every later verification compare against the wrong intent.

## The sandbox, before any tool runs

Every path argument is resolved by `Sandbox::resolve` against the one root, walking components
in POSIX order and expanding symlinks as it meets them. The result is a `SandboxPath` — a type
only the resolver can construct, and the only path type any tool accepts. A tool that tried to
name a file outside the root would fail to compile ([D6](../DECISIONS.md)). Below that, file
I/O walks the tree one `openat(O_NOFOLLOW)` component at a time from the root descriptor, so a
symlink swapped in *after* resolution is refused at the hop where it appears rather than
followed — closed 2026-08-26, D6's "Closed" paragraph.

The guarantees are claims about a particular filesystem — case sensitivity, inode stability,
`ctime` behaviour. A probe that attempts each of them under the root rather than assuming
them is decided ([D11](../DECISIONS.md)) but **not yet built** as of 2026-08-27: today
nothing validates the root's filesystem, and rooting on an exotic substrate (a network
mount, `/mnt/c` under WSL) means trusting semantics nobody has checked there.

## The nine tools

Eight structural tools, each one complete job, verified per call wherever a call has
something to verify ([ROUTING.md](../ROUTING.md) §4):

| Tool | The one whole job | Verification |
|---|---|---|
| `read` | Exact bytes of one or more files | content hash returned alongside |
| `list` | Directory entries: type, size | identity tuple recorded per entry, supervisor-side |
| `find` | Paths matching a name glob | — |
| `grep` | Literal substring matches, line-granular, as sibling fields | — |
| `hash` | Content hashes for a path set | *is* the verification |
| `write` | Write, read back, compare | read-back; backup first |
| `edit` | Exact `old` → `new`, exactly once | read-back; backup; fails closed on a stale target |
| `move` | Hash source, rename, hash destination | hash at both ends; can never replace an existing file |

Plus `shell`, the ninth — kernel-confined, off by default, and different enough to get
[its own chapter](./15-shell-and-landlock.md).

Three semantics worth knowing before you watch a run:

- **`edit` requires its target to have been observed this session.** A fresh session has seen
  nothing and must read (or list) before it may edit; an edit against a file whose identity
  tuple has moved since it was last observed is refused. This converts "a write to a file the
  model never read" from a silent failure into a loud one.
- **`edit`'s `old` must occur exactly once.** Zero is "not found"; two or more is "ambiguous,
  give more context". Replace-first would silently guess; replace-all lets one confused call
  rewrite a file.
- **`move` structurally cannot replace a destination** (`renameat2(RENAME_NOREPLACE)`), so the
  recorded silent-destruction failure cannot be expressed. The refusal suggests the first free
  `name (N)` destination rather than performing the rename itself.

Results are never decorated — no line-number prefixes, no end-of-file markers, no truncation
ellipses. Models have been recorded copying a harness's own decorations back into files as
content, twice, in two different harnesses ([ROUTING.md](../ROUTING.md) §5).

## What one overwriting write actually does

1. Stat the target and compare its identity tuple (`dev:ino:size:mtime:ctime`) against what
   this session last observed — fail closed if it moved, recording nothing.
2. Preserve the current bytes as one generation in the backup store, which lives **outside**
   the sandbox root, where no tool can list, read, edit or move it
   ([undo and backups](./17-undo-and-backups.md)).
3. Write to an exclusive temp file beside the target, then atomically rename over it.
4. Read the bytes back and compare (R5).
5. Return the new content hash and a fresh identity tuple — never a bare success flag.

A create takes no backup (nothing is destroyed; it publishes by `link()`-no-replace, so a file
that appeared after the check is preserved and the call refused). A move takes no backup
either — content is hash-proven to survive, only the name is at risk, and undo is moving it
back.

## Verification lives at two layers

Per-tool verification is real, and it is not the guarantee. Both destructive incidents in the
evidence base came *through* file tools, as did every sandbox escape — so which tool the model
uses is the wrong lever ([SCOPE.md](../SCOPE.md) § "Why terminal survives"). The layer that
holds unconditionally is **per-turn**: the tree is snapshotted before the run and after every
turn, and a hash-verified changeset is computed that owes nothing to the model's reply
([D13](../DECISIONS.md)). Hashes carry forward — an entry whose identity tuple is unchanged
keeps its hash, so a turn costs the walk plus the bytes that actually moved. (When `shell` is
registered, every file is rehashed unconditionally instead; the tuple shortcut is not
trustworthy against everything a shell can do.)

The changeset separates real content changes (`created`, `deleted`, `modified`) from a
touch — tuple moved, content identical, as a rewrite of the same bytes produces — and from a
permissions-only change, which gets its own category rather than hiding among modifications.
Folding any of these together would teach the reader to ignore the report.

## The supervisor turn

One `hermit agent` job runs like this:

1. **Baseline.** Snapshot the tree. Parse any stated expectations against it
   ([chapter 14](./14-expectations.md)); refuse impossible sets before the model is called.
2. **Bounded session.** Offer the tools, dispatch each call in order, feed results back.
   Three bounds: `--max-turns`, `--budget` wall-clock, and a per-turn cap on calls from one
   reply — calls past the cap are refused, never dropped, because a dropped call reads to the
   model as still outstanding.
3. **Verify per turn.** Snapshot, diff, judge the structural expectations against the
   baseline.
4. **Judge meaning once per attempt**, only after every structural expectation is met: a
   `satisfies:` criterion goes to a model in a fresh session that reads the file's bytes and
   the tree's path list — never the transcript — and its verdict is labelled as judgment, not
   measurement ([D15](../DECISIONS.md)).
5. **Retry on an unmet verdict.** Up to `--attempts` total (default 3), each a fresh session
   handed the original task plus the one concrete remaining failure. Every attempt is judged
   against the *one* baseline taken before the first, so a wrong first attempt cannot change
   what your expectations mean mid-job. Infrastructure failures and undecidable-only verdicts
   are never retried — retry exists for model inconsistency, nothing else.

## The layering that keeps it honest

| layer | contains | knows about |
|---|---|---|
| `core` | sandbox, tools, verification, backup | no model, no network |
| `supervisor` | the loop, sessions, verify, judge, retry | the transport's types, never HTTP |
| `ollama` | the one HTTP client, native `/api/chat` | the daemon |
| `app` | config, toolset composition, expectations, the CLI | all of the above, thinly |

The boundary is the build graph, not discipline: `hermit_core` links neither the HTTP nor the
JSON library, so a Tier 0 tool that tried to reach a model would fail to link — the same
property at the architecture scale that `SandboxPath` provides per call
([ROUTING.md](../ROUTING.md) §7). One descriptor list renders both the tool definitions Ollama
sees and the MCP definitions a programmatic caller will read, so there is no second schema to
drift.

Concurrency: one request in flight, single-threaded, by decision ([D1](../DECISIONS.md)).
Parallelism, when wanted, is more `hermit` processes on disjoint roots — comparing filesystem
state is the product, and it is dramatically harder while something else mutates it.
