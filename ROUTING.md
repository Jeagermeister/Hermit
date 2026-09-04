# Routing — the tool surface, and who is allowed to call what

Drafted 2026-08-15; implementation began 2026-08-16. **§12 records what is done — everything
not marked there is still design intent.** This settles the surface
[Phase 2](./ROADMAP.md) will build: which tools exist, what they return, where each one lives in
the build graph, and which frontend exposes which subset.

[ROADMAP.md](./ROADMAP.md) names six tools in a bullet. This is that bullet, decided.

---

## 1. There is no router

The obvious design is a component that receives work and decides where to send it — a small model
with "a degree of intelligence" that reads a request and picks a backend.

Interrogate each input that decision needs, and every one of them is **already known by the
caller at the moment it calls**: what kind of operation this is, what language the project is in,
which files are involved. Re-deriving them locally pays twice for something the caller already
had, and pays it in inference latency.

So requests are **typed, not natural language**, and the type is the routing decision. Under
[D7](./DECISIONS.md)'s MCP-over-stdio frontend, the tool schema *is* the routing table: a caller
selecting a tool has routed, at the cost of the tool selection it was doing anyway.

**The router is a schema, not a service.** Nothing in this document is a component that decides
where work goes.

---

## 2. Three tiers

| Tier | What it is | Cost | Covers |
|---|---|---|---|
| **0 — Execute** | No inference. Filesystem operations with verification. | syscall latency | read, list, find, grep, hash, write, edit, move |
| **1 — Reason locally** | The local model under the supervisor loop (R6/R7/R8). | seconds to minutes | triage, summarize |
| **2 — Escalate** | Decline, and return *context* rather than an answer. | none | anything else |

Tier 2 is a first-class path, not an error branch. A local model handed work it cannot do returns
confident nonsense that the caller then has to detect and undo — strictly more expensive than
never delegating. The good version of a decline is not a refusal: it is a **smaller problem**
("I could not answer this, but here are the 4 relevant files out of 200"), which is Tier 1 doing
real work even when it fails.

---

## 3. The deciding rule

> **One correct answer computable from the inputs → Tier 0.**
> **Choosing among defensible answers → Tier 1, or escalate.**

And the corollary, which matters more:

> **A Tier 0 tool never silently falls back to Tier 1.** No answer is a valid answer.

Failing closed is the point. A tool that quietly succeeds at something adjacent to what was asked
makes R5's read-back compare against the wrong intent, and the guarantee is hollow.

---

## 4. The tool surface

### Tier 0 — eight tools

| Tool | The one whole job | Verification |
|---|---|---|
| `read` | Return exact bytes of one or more files | Returns content hash alongside |
| `list` | Directory entries: type, size | `dev:ino:size:mtime:ctime` recorded per entry, supervisor-side (§5) -- the tuple feeds the staleness guard and is not rendered to the model, which cannot echo or act on it |
| `find` | Paths matching a name/glob pattern | — |
| `grep` | Content matches: path, line, text as sibling fields per match | — |
| `hash` | Content hashes for a path set | *is* the verification (R3) |
| `write` | Write, read back, compare | R5; R4 backup |
| `edit` | Exact `old` → `new`, read back, compare | R5; R4 backup; fails closed on a stale identity tuple |
| `move` | Hash source, move, verify destination | R3 both ends |

**`delete`** is Tier 0 by the same rules but registered only on request (`--delete`), appended
ninth so the eight's prompt bytes never move: remove one regular file observed this session, its
bytes to the store before the name goes, the identity tuple re-checked at the name, the name
confirmed gone. Admitted 2026-09-04 under [D19](./DECISIONS.md); §11 has the two conditions it
waited on.

`find` and `grep` are a deliberate split of the roadmap's single `search` — name matching and
content matching have different inputs and different failure modes. `hash` is new to the roadmap
and earns its place by making R3 and R6 cheap enough to run after every turn.

`grep`'s row originally read `path:line:text` and was revised 2026-08-16: the colon-joined form
is §5's decoration shape by another name — metadata interleaved with content, the exact pattern
`5|2026-08-12 shipped` proved a model will copy back into a file — and it is ambiguous besides,
since a POSIX path may contain colons and text always does. Sibling fields are also what the
implemented result shape already returns, so the fix was a table cell, not code. A *human*
frontend remains free to render matches grep-style; that is presentation, not the tool result.

**Each call is one complete job.** "Low-level" here means *dumb* — no judgment, no interpretation,
no guessing what was meant — not *granular*. `move` moves the file, hashes it, confirms it
arrived, and reports; it does not expose `open`/`link`/`unlink` for the caller to compose. Every
tool call is a round trip and a chance to go wrong, and
[SWEEP2](./bench/fsops/SWEEP2.md) shows these models failing on multi-step composition, not on
individual operations.

### `list` returns identity, not content

An earlier draft had `list` return a content hash per entry. That is a full read of every child
on every listing, which makes `list` the expensive call and takes back exactly what `hash` was
added to provide. §5's rule does not extend to it, and the reason is mechanical: `read` already
holds the bytes, so hashing them is free; `list` does not.

**The two are different questions and want different answers.**

| Question | Answer | Serves |
|---|---|---|
| Is this the same file, in the state I last saw? | `dev:ino:size:mtime:ctime` | the staleness guard, at O(1) per entry |
| Did the content change the way I intended? | content hash | R3 |

Identity is also the *stronger* answer to the first question. `dev:ino` catches a file that was
unlinked and recreated, or a symlink retargeted to a different file — replacement with
byte-identical content, which a hash cannot see at all. `ctime` catches a metadata-only change
between observation and write, and matters specifically because
[D10](./DECISIONS.md#d10--kernel-confinement-for-shell-landlock-vendored-one-writable-root)
records that a *confined* process can still call `utimes`: `mtime` is forgeable, `ctime` is not
settable directly.

The consequence worth having is that `list` and the staleness guard share one currency, so an
observation from listing a directory is directly usable as the expected value on a later `edit`.
Under the hash design those were two incompatible units.

### `find` and `grep` semantics, and the read cap — settled 2026-08-16

**`grep` matches literal byte substrings, line-granular.** Not regex, deliberately: models emit
literal fragments, a literal miss is a visible zero-match rather than a silent regex surprise,
and nothing is interpolated. One row per matching line — path, 1-based line, the line's exact
bytes as sibling fields, per the table above. An empty pattern is refused rather than matched
vacuously (§3), and a pattern containing `\n` can never match — visible in the zero-row result,
not special-cased. If fsops evidence later shows models emitting regular expressions at this
tool, the upgrade path is ERE behind the same argument, recorded here first.

**`find` matches `fnmatch(3)` globs against entry *names*,** case-sensitive, dotfiles matchable
by `*`, walking depth-first in sorted order from an explicit start directory. Symlinks are never
followed — neither for recursion nor matching — so no result can name a file outside the root
and cycles cannot occur.

> ⚠️ **Marked for revisit** (2026-08-16, at the maintainer's request): the fnmatch dialect is a
> starting point chosen for POSIX definition and zero dependency, not a measured endpoint.
> Path-globs (`src/**/*.c`) and case-folding are the two plausible future asks; either lands
> here first, then in code.

**`read` and `grep` refuse oversized files, and the refusal is guidance.** The cap is
*configuration* (§9) with a 16 MiB stand-in default, passed through each tool's constructor and
wired to the config layer when a composition layer exists. The refusal names the file, its size
and the cap, and points at `hash` and `list` — the tools that answer at any size — never a
truncated read: a partial answer is the adjacent-success §3 forbids. `hash` is deliberately
uncapped and streams at constant memory, because verification is exactly the job that must not
degrade with size.

### `edit` fails closed on a stale target

`edit` is checked against the identity tuple **this session last observed** for its target —
session state, never an argument — and refuses when the tuple no longer matches. It has to be
session state: a caller-supplied tuple could not express *unseen*, so it would let a fresh
session hand in a tuple for a file it never read, which "nothing is persisted" below exists to
forbid. (Settled 2026-08-16, resolving a reading of the earlier wording under which the tuple
arrived as an argument; the table below was always written for the session-state reading, and
it is what keeps `String`/`Path`/`PathList` sufficient as the argument types for every settled
tool.)
This is a **second layer**, not a replacement for anything in §6: it is a per-tool control and
covers only the tools the model chose to use, which is exactly the limit §6 names. It earns its
place by converting one specific silent failure — a write to a file the model never read, or
read before something else changed it — into a loud one at the point it happens, which is the
argument §6 already makes for keeping per-tool verification despite that limit.

Observed state is per-session and in memory: a path is **unseen**, **absent**, or **present at
tuple T**. A successful `read`, `list` or mutation records presence; a miss records absence.
Nothing is persisted, which is the correct semantics under bounded sessions — a fresh session
has observed nothing and must observe (read, or list) before it may edit; the one-currency
design above is exactly what makes a listing count.

| Intent | Observed | Decision |
|---|---|---|
| `write` | unseen or absent | create-if-absent |
| `write` | present at `T` | replace only if the tuple still matches `T` |
| `edit` | unseen | refuse — the file must be observed first (a read, or a listing) |
| `edit` | absent | refuse — not found |
| `edit` | present at `T` | compare against `T`, refuse on mismatch |

**The load-bearing row is the first one, and only if create-if-absent is honest.** It must fail
when the target already exists — that refusal is what stops a model overwriting a file it never
read, which is the `05_copy` shape SCOPE.md records. Implemented as a plain existence check it
is a race; the reference implementation this came from publishes by hard-link-no-replace
instead, so a file created *after* the check is still preserved and still rejected.

### The mutating trio — semantics settled 2026-08-16

**`edit` requires `old` to occur exactly once.** Zero is "not found"; two or more is
"ambiguous, give more context" — deliberately uncounted, because two is already ambiguous and an
exact total buys nothing. Replace-first would silently guess which occurrence was meant — the
adjacent success §3 forbids — and replace-all lets one confused call rewrite a whole file. Both
refusals tell the model what to do next.

**`move` never replaces an existing destination, structurally.** `renameat2(RENAME_NOREPLACE)`,
so the silent-destruction shape (`05_copy`) cannot be expressed. The refusal carries the
file-manager affordance *as a suggestion*: it names the first free `name (N)`-style destination,
and the caller moves there explicitly if a renamed copy is what it wants. Considered and
rejected: performing that rename automatically — the destination a caller names must be the
destination it gets, or every later reference to the name it asked for is wrong. Replacing a
destination's *content* is still expressible, deliberately as two observed steps: read it, then
write it.

**`move` is ungated by observed state.** The table above gates `write` and `edit` — the calls
that destroy bytes. A move preserves content byte-for-byte and R3 proves it did (hash before,
hash after, compare); only the name is at risk, and undo is moving it back. Gating would force
full-body reads just to rename, which the read cap makes hostile for exactly the files most
worth not loading. Regular files only: hashing is the verification, and a directory has no
content hash.

**Backups live outside the sandbox root** in a supervisor-provided directory (configuration,
§9), one generation per *overwriting* mutation, nothing ever overwritten. A create and a `move`
take no generation, and that is correct rather than an omission: neither destroys bytes —
create cannot replace (the `link()` refusal is atomic) and a move's undo is moving it back, R3
having proved the content survived. A reused directory continues numbering after the
generations already in it; order generations numerically, not lexicographically. Outside the
root, for two reasons: the model must never be able to list, read, edit or move its own undo
data; and the sandbox's `list` and `find` stay free of archive noise. Backup paths are
host-absolute and never appear in a model-facing row — `SandboxPath::relative` exists so the
host layout does not leak, and the archive is part of the host layout.

**Mechanics, recorded so they are not re-derived:** create publishes by `link()`-no-replace (a
file created after the check is preserved and rejected — the honest create the table demands);
replace goes through an exclusive temp beside the target and an atomic `rename`; every
*overwriting* mutation reads back and compares before succeeding (R5), while `move` verifies by
hash at both ends instead (R3, its own row), and the post-mutation stat becomes the recorded
observation. A failed staleness check records nothing — a recorded fresh tuple would let a
retry pass the guard without re-reading content. `read` commits its presence observations only
when the whole call succeeds: a refused call delivered no bytes, and presence recorded from it
would let a later write pass the guard on content the model never received. Absence records
immediately, from the misses that prove it — an ENOENT read, a guarded mutation finding its
observed file vanished; other failures prove nothing about existence and record nothing.
Missing parent directories are created by `write` and `move` (the surface has no mkdir tool;
parents are means to the one whole job, not judgment). A replace preserves the file's
*permission bits* and deliberately drops setuid/setgid/sticky — carrying privilege onto
model-chosen content is nobody's intent; a create honors the umask over 0666. `edit` shares
`read`'s cap — the file must be loaded to be edited. Every successful mutation returns the new
content hash and the fresh identity tuple, in `list`'s currency, so the result is immediately
usable as the next expected value. Durability is deliberately unclaimed: no fsync — R5 verifies
content and R4 keeps the old bytes recoverable, and a crash-durability guarantee would be a
DECISIONS.md entry, not a flag. Publication — parent creation, `link`, `rename` — is path-based
until §12 step 5 widens the funnel to it; the interior-component window that leaves open, and
why D10 does not backstop it, is recorded there.

### `shell` — kept, and it is a special case

The instinct is to drop shell and expose structured file tools only.
[SCOPE.md](./SCOPE.md) records why that is wrong: `qwen3.5:4b` performed every filesystem
operation through `terminal` and never touched a file tool, while `llama3.2:3b` used `write_file`.
**Tool preference varies by model and is not under our control.** Removing shell breaks the
qwen-shaped models, and `qwen3.5:9b` is the model this is built against.

Shell is also the **one tool that cannot be R1-correct by construction**. Every other tool takes a
`SandboxPath`, which only `Sandbox::resolve` can build, so it physically cannot name a file
outside the root. A shell command is an opaque string: there is no path argument to resolve and
nothing for [D6](./DECISIONS.md) to contain. That is what makes its frontend exposure a separate
question — see §8.

### Tier 1 — two tools

| Tool | The one whole job |
|---|---|
| `triage` | Given N paths and a question → ranked subset, one line of reason each |
| `summarize` | Given N paths and a question → one summary per file |

Short on purpose. Both are latency-tolerant, token-expensive and reliability-forgiving — the
profile local inference wins on. Neither is on a critical path.

### Opt-in — `delete`

Admitted 2026-09-04 under [D19](./DECISIONS.md), registered ninth by `--delete`; §11 records
the two conditions it waited on and how each was met.

---

## 5. What tools return

**No decoration, ever.** No `N|` line-number prefixes, no end-of-file markers, no truncation
ellipses. Line numbers travel in a sibling field, never interleaved with bytes.

This is a requirement rather than a preference because it has already failed twice, in two
different harnesses:

- Under OpenCode, Gemma E4B copied the rendered `(End of file - total 1 lines)` annotation into
  `oldString` twice, both edits failed, and it stopped.
- Under Hermes, `read_file` decorated 226 of 240 recorded calls with `N|` prefixes plus a phantom
  trailing marker, and `qwen3.5:4b` wrote `5|2026-08-12 shipped` into a file — the harness's own
  decoration, echoed back as content. That finding is what falsified R5's original stated
  evidence; see [REQUIREMENTS.md](./REQUIREMENTS.md).

**Every read returns a content hash.** That is what makes `hash` cheap enough to poll with:
confirming twenty files landed correctly costs a hash set, not twenty file bodies.

---

## 6. Verification belongs at the filesystem layer, not the tool layer

The natural reading of §4 is that safety lives inside each tool — `write` reads back, `move`
hashes both ends. That is real, and it is **not the guarantee**.

[SCOPE.md](./SCOPE.md) is explicit, and it is measured rather than reasoned: both destructive
`05_copy` incidents — the ones that overwrote `config.ini` with invented content — **came through
file tools, not shell**, as did every escape into the repository root.

> The conclusion is that *which tool* is the wrong lever. Verification belongs at the filesystem
> layer, not the tool layer: snapshot before the turn (R4), hash-diff after (R3), and decide
> completion from that rather than from the model's claim (R6) — regardless of which tool the
> model reached for.

So the layering is:

| Layer | Covers | Requirement |
|---|---|---|
| **Per-tool** | Only the tools the model actually chose to use | R5 read-back on `write` / `edit` |
| **Per-turn** | Everything, including shell and tools we did not write | R3 hash-diff and R6's **observation** half — built 2026-08-17 in `supervisor/verify.cpp` ([D13](./DECISIONS.md)). R6's *completion decision* is not: the loop still stops on `tool_calls.empty()`. R4's recoverable snapshot is not here either — it lives in the tool layer, and a hash is not an undo |

Per-turn is the one that holds unconditionally, and it is a **supervisor** concern
([Phase 3](./ROADMAP.md)), not a tool concern. Per-tool verification is worth having anyway —
it converts a silent wrong write into a loud failure at the point it happens — but a design that
relied on it alone would be trusting the model to use our tools.

---

## 7. Where the code lives

[D7](./DECISIONS.md)'s layering table already assigns every piece:

```
src/hermit/core/tools/        Tier 0. Cannot reach a model.
src/hermit/supervisor/tools/  Tier 1. Links the transport.
src/hermit/app/mcp.cpp        The frontend. JSON-RPC over stdin/stdout.
```

**The tier boundary is enforced by the build graph, not by discipline.** `hermit_core` links
neither `hermit_ollama` nor anything that could reach it, so a Tier 0 tool that tried to call a
model would fail to link. This is the same property `SandboxPath` gives R1: structural, and still
true at tool #37.

### Two link edges were missing, and were added first

Verified missing against `CMakeLists.txt` on 2026-08-15; **both added 2026-08-16**:

1. **`hermit_supervisor` did not link `hermit_core`.** Stated precisely: the gap was a missing
   *usage-requirement declaration*, not an in-tree link failure — every target publishes the
   same include dir and the two final binaries linked all four archives, so a Tier 1 tool here
   would in fact have compiled and linked by accident. The edge turns that accident into a
   declared dependency, which is exactly what an out-of-tree consumer of `hermit::supervisor`
   alone would have been broken by. Required before `triage` or `summarize` exists.
2. **`hermit_app` did not link `hermit_supervisor`.** No *library* target saw all four — only
   the final binaries (the CLI and the test runner) did — so registry composition in `app`
   would likewise have leaned on the binaries' link lines rather than a declared edge. Required
   before `mcp.cpp` exists.

### Tool descriptors stay JSON-free in `core`

[D4](./DECISIONS.md) gives tools an `Args` struct with field descriptors "for schema generation
and parsing" — both JSON operations. But [D2](./DECISIONS.md) deliberately put `nlohmann` in the
build *with the client*, and `hermit_core` links no JSON library.

The resolution is better than widening `core`: **descriptors are pure data** (name, type,
required, documentation) with no JSON types, and rendering them to JSON Schema happens in a layer
that already has `nlohmann`. D4's one-declaration guarantee survives intact — the rendering simply
is not in `core`.

That guarantee then extends further than D4 claimed. One descriptor list emits **both** the
tool definitions offered to Ollama in its `tools` array **and** the MCP tool definition the
programmatic caller reads. There is no second schema to keep in sync, so R2 drift stays
unrepresentable across the MCP surface too.

(Both said *"as `format`"* until 2026-08-17. [D12](./DECISIONS.md) settled that `format` cannot
be sent beside `tools` at all, so the destination named here was wrong — not the guarantee.
`supervisor/wire.cpp` is the renderer, and `tool_definitions()` is the one function both
surfaces go through.)

---

## 8. Frontend exposure is a policy, not a fixed set

Both frontends drive the same core; they need not expose the same subset.

**`shell` is off the MCP surface by default.** Enabling it is an explicit configuration field that
prints as a marked line, the same treatment a raised `max_num_ctx` clamp or a waived tools gate
already gets.

**The original reason no longer holds, and is replaced rather than quietly kept.** This section
argued that every other tool is contained by type while shell cannot be, there being no path in
a command string to resolve — so exposing it was handing an arbitrary caller an unsandboxable
string executor.
[D10](./DECISIONS.md#d10--kernel-confinement-for-shell-landlock-vendored-one-writable-root)
falsifies the premise: shell *is* contained, by the kernel rather than by the type system. The
default does not change. The argument for it does, and resting on a dead premise would be worse
than having no argument.

**Two reasons survive, and they are enough.**

*Egress is ungoverned.* D10 records that only `handled_access_fs` is set: a confined process
still reaches the network and pathname unix sockets. D7's threat model for a programmatic
frontend is prompt-injection-influenced input, and injection plus unrestricted egress is
exfiltration. Containment bounds what shell can *touch*; it does nothing about what shell can
*send*. **If shell is ever put on this surface by default, the net bits are governed first** —
that ordering is the decision, not a preference.

*Depth, not redundancy.* Filesystem containment for shell is one mechanism with one failure
mode. Every other tool has two independent ones — `SandboxPath` by construction *and* the same
kernel ruleset. Keeping shell off the default surface preserves that asymmetry deliberately
rather than by accident.

**Where exposure is gated, gate on the probe, never on the platform.** Enabling `shell` on this
surface requires our own confinement probe — the one that attempts a denied write and requires
`EACCES`, per D10 — to report full enforcement. This keeps §9 intact on a machine that has no
Landlock backend at all: the difference becomes *data* the probe reports, not a second build or
a stripped tool surface. A platform check would also silently pass on a Linux kernel with
Landlock compiled out, which the functional probe catches.

**This is deliberately not a judgment about the caller.** Whether a caller already has a shell is
a property of *its harness*, not of the model behind it — Claude Code has one, a bare MCP client
does not, and an unknown future harness could be either. A rule that depended on knowing which
would be wrong the first time someone new connected.

---

### Kiro is the first named programmatic caller

Recorded 2026-08-16 because it is the concrete case D7's "programmatic caller" was abstract
about, and because the shape of the integration turned out to constrain the platform question
rather than the reverse.

**Kiro is an AWS agentic IDE forked from Code OSS, with a separate CLI.** It consumes MCP over
stdio natively — `command`, `args`, `env`, `disabled`, `autoApprove` under an `mcpServers` map,
read from `.kiro/settings/mcp.json` (workspace, taking precedence) and `~/.kiro/settings/mcp.json`
(user), hot-reloaded on save. **That is exactly the transport D7 already specifies**, so no new
frontend shape is needed: a Hermit binary is a `command` entry and nothing more.

**A "Power" is the wrong vehicle, and this is settled rather than open.** Powers are pre-built
integrations *maintained by Kiro* for Kiro Web, toggled in agent settings; some wrap MCP servers
internally for OAuth. There is no documented path to author, package or distribute a third-party
Power, and the ones that exist solve an authentication problem this project does not have — D7
forecloses credentials entirely. **Ship a plain stdio MCP server.** If a Power path ever opens,
it wraps the same binary and changes nothing here.

**`autoApprove` maps onto the tier split for free**, which is a genuine piece of luck worth
using: it is a per-tool allow-list, and §4's surface already separates tools that observe from
tools that mutate. `read`, `list`, `find`, `grep` and `hash` are natural entries; `write`, `edit`
and `move` are not, and `shell` is off this surface by default per above. **This is caller
policy, not ours** — we do not ship a config that pre-approves a mutating tool, per §11's rule
about not legislating the caller's budget.

**The value proposition is sharper than "another MCP server", and Kiro's own documentation makes
the argument.** It warns that stdio MCP servers "execute arbitrary commands inside your
environment with the same privileges and access as the agent itself, including access to your
source code, environment variables, secrets, and any credentials available in the session."
Ambient full authority is the ecosystem default. Hermit is the MCP server that *reduces*
it — R1 containment by construction, D10 confinement for the one tool that cannot be, R3/R4/R5
verification on every mutation. That is the pitch, and it is stronger for a programmatic caller
than for a human one.

**The platform consequence, which is the load-bearing part.** Kiro CLI 2.0 runs **natively on
Windows 11**, MCP servers included. So on a Windows developer's machine Kiro spawns MCP servers
as native Windows processes, and a Linux ELF binary is not spawnable. Two paths, and neither is
free:

| Path | Cost |
|---|---|
| `"command": "wsl.exe", "args": ["-e", "hermit", …]` | Requires WSL, which is not a safe assumption. Kiro passes Windows paths; the sandbox root needs a Linux path. **The translation layer sits directly on `Sandbox::resolve`** |
| A native Windows build | A separate package — see [SCOPE.md](./SCOPE.md) § Platforms |

**Neither is on the near path, and that is the decision.** Linux is the product. This section
exists so the integration is not re-derived, and so the Windows question is understood as *a
consequence of Kiro's platform*, not as generic portability anxiety.

---

## 9. Machine differences are configuration, never code

**There is no laptop build and no desktop build.** One binary, one tool surface, no `#ifdef`, no
stripped variant. Tier 0 needs no GPU at all — it is filesystem syscalls — so it runs at full
fidelity anywhere. The only thing that scales with hardware is Tier 1, which is the tier that was
always optional.

Choosing a model appropriate to the machine is an **operator responsibility**. The model and base
URL are already `hermit_app` settings, so this is a config file per machine.

**Where a capability must be restricted, gate on the model, not the machine.** A rule like "no
mutating tools on the laptop" reintroduces the per-machine variant this section forbids, and
breaks the moment a larger model is installed there. A capability declared alongside the model
travels with it correctly. R9's preflight already has a waivable tools gate; this is the same
mechanism, not a new one.

---

## 10. Parallelism: disjoint roots, join on hash

[D1](./DECISIONS.md) is blocking and single-threaded, and states its own overturn path: several
independent sessions in parallel, answered by **process-level parallelism — separate sandbox
roots, one session each — not threads inside one session.** The reason is that R3 through R6 are
state-comparison requirements, and comparing state is dramatically harder when something else
might be mutating it.

| Shape | Verdict |
|---|---|
| Background read-only Tier 1 work while the caller works elsewhere | **Safe.** Nothing shared is mutated. |
| Background mutating work on a **disjoint** subtree | **Safe.** D1's stated overturn path. |
| Two workers editing the same tree concurrently | **Out.** Correctness, not performance. |

The third case is not a tradeoff. A caller hashes a file, composes an edit, and applies it while
another worker modifies it in between: best case the exact `old` no longer matches and `edit`
fails closed; worst case R4's snapshot captures an intermediate state neither worker intended,
and undo restores something that never existed.

Concurrency therefore lives in the **caller's harness**, expressed as more `hermit` processes.
Nothing inside Hermit gains a thread. The join is `hash` — deterministic, and a Tier 0 call.

---

## 11. Open, and deliberately deferred

### `delete` — admitted 2026-09-04, opt-in ([D19](./DECISIONS.md))

Not in the roadmap's original tool list, and this project exists partly because a tournament
run erased `tally.py`. It was deferred behind two conditions, and both now hold:

1. **Undo exists and works.** R4 says "undo is a first-class operation, not a debugging aid."
   **Where** backups live was settled 2026-08-15 from an unexpected direction:
   [D10](./DECISIONS.md#d10--kernel-confinement-for-shell-landlock-vendored-one-writable-root)
   permits exactly one writable directory grant, so a backup store outside the sandbox would need
   a second one — and two writable roots let a confined shell rename or hardlink between them.
   The resolution is that **backups are never granted to the child at all**: R4 is a per-turn
   supervisor concern under §6, and the supervisor is the unrestricted parent. Backups may
   therefore live anywhere convenient, including outside the root, because the confined process
   cannot address them. **Designed and built 2026-08-18** ([D14](./DECISIONS.md)): `hermit undo`
   lists the store's generations and restores by explicit flag, restore preserves what it
   overwrites, and a restore whose target is missing recreates it — the exact shape a deleted
   file's recovery takes. **Held since then.**
2. **A retested `06_selective_delete` holds for the model being built against** — on the current
   harness, with more than 3 repeats and a `--deterministic` pass. SWEEP2 §4 demonstrated that
   per-cell numbers at n=3 are not capability: the control moved 3/3 → 0/3 on the simplest task
   in the suite, same machine, same night. **Held since 2026-08-26**: SWEEP3, five repeats,
   sampling pinned — `gemma-e4b` 5/5, `qwen-9b` 5/5. D19 states the width of that claim.

What was built, in one paragraph: one regular file per call, gated on an observation this
session whose identity tuple still matches; the bytes go to the store before the name goes, and
a failed backup deletes nothing; the name is re-checked against the observed identity before
`unlink` and confirmed gone after. `--delete` registers it, ninth, ahead of `shell`, off by
default. Dry-run was decided against in the same decision — D19 says why, and what would
reopen it.

### Fuzzy `edit` — not inside `edit`, if ever

Upstream Hermes' `patch` fuzzy-matches, which is why stage `05_recovery` is not like-for-like even
in principle. Fuzzy matching is judgment: "close enough to what you asked for" is a model's call.
If it is ever wanted it is a **separately named tool that admits it is guessing**, never a silent
fallback inside `edit`.

### Not our policy

The threshold at which a caller should delegate selection to `triage` rather than choosing from a
`grep` result depends on that caller's context budget. It is **caller policy and does not belong
in this codebase**.

---

## 12. Next steps

Ordered. Steps 1–5 are Phase 2 and 2.5 as already written; only the tool list is new.

1. ~~**Add the two link edges**~~ (§7) — `supervisor → core`, `app → supervisor`. Prerequisite
   for everything below, because they decide which target `tool.h` can live in. **Done
   2026-08-16.**
2. ~~**`tool.h`**~~ — the D4 base class and JSON-free descriptors. Everything inherits it.
   **Done 2026-08-16**, in `core` beside the sandbox: the spec/argument/result types are pure
   data with no JSON, per §7, and `parse_args` is the one place a `Path` argument becomes a
   `SandboxPath`.
3. ~~**The eight Tier 0 tools**~~ in `core`, with tests. **Done 2026-08-16**: the observe
   surface (`read`, `hash`, `list`, `find`, `grep`) and the mutating trio (`write`, `edit`,
   `move`), with `ObservedState` carrying §4's staleness table, the backup store outside the
   root, and the settled semantics recorded in §4. `shell` was not among them at the time —
   registered separately once D10 landed (step 5's *first* condition, kernel confinement;
   done 2026-08-22) plus its own config flag and a live `probe_confinement()` check. Step 5's
   *second* condition, the `openat` walk, gates `mcp.cpp` (step 6) specifically, per D7's own
   text — not this registry, which this list corrected once it stopped being ambiguous which
   frontend "the gate" meant.
4. ~~**The agent loop**~~ — not in this list when it was written, because §12 tracks the tool
   surface and the loop is Phase 2's own bullet. Recorded here anyway, since the steps below now
   build on it. **Done 2026-08-17**: `supervisor/loop.cpp` drives the turn, `supervisor/wire.cpp`
   is the JSON bridge §7 above specified, `app/toolset.cpp` composes the eight tools in the order
   §4 lists them, and tool calls reached the wire for the first time under
   [D12](./DECISIONS.md) — which settled that `tools` and `format` cannot both be sent, and made
   the combination unrepresentable rather than discouraged. `shell` was still absent at the time
   this step was written; see step 3 above for where and when it landed.

4b. ~~**The judgment half reaches a caller**~~ — numbered 4b rather than 5 because the steps
   below are cited by number elsewhere, and renumbering them would break those references.
   **Done 2026-08-18.** `supervisor/judge.cpp` decided post-conditions and nothing produced
   any, so it was unreachable code; `app/expect.cpp` is the source. Expectations are stated
   as `--expect kind:path` (repeatable) or an `expectations` array in the config file, judged
   after every turn against the baseline, and printed by `agent`, which exits 3 when
   something stated is measurably undone.

   Two rules earn a place here rather than in a commit message, because both are cases where
   the code looked right and would have reported something false:

   - **A path is keyed by its literal spelling, never by `resolve()`'s output.** `resolve`
     expands symlinks and §6's walker does not, so with `link.md -> notes.txt` the resolved
     key answers about `notes.txt` while the operator asked about `link.md` — with a hash
     behind it, so it reads as verified. `resolve` is kept as the containment *gate* and its
     answer discarded. `..` is refused outright for the same reason: POSIX order and lexical
     folding disagree after a symlink, so `dirlink/../x` gates as `a/x` and keys as `x`.
   - **A tree that could not be read leaves every expectation `Undecidable`.** A default
     `Verdict` has no findings, and `met()` is true of no findings — so a run that failed to
     look would have reported everything as passing. §6's fail-closed rule has to reach the
     verdict, not only the walk.

4c. ~~**The verdict feeds back — R7's re-invocation**~~ — same numbering rule as 4b.
   **Done 2026-08-18.** `supervisor/reinvoke.cpp` runs up to `--attempts` total attempts
   (default 3), each a **fresh session** given the original task plus the one concrete
   remaining failure from `Verdict::first_unmet()` — never the failed history. The nearest
   measurement (ROADMAP.md, 2026-08-17): handed a mid-session refusal, three of four models
   stopped calling tools and addressed the human instead — these models do not reliably
   self-correct in place, so the correction has to arrive fresh. Three policies, each a
   test:

   - **One baseline per job** (`LoopOptions::judge_baseline`). Attempt two is judged against
     the tree attempt one started from, because `preserved:a=b` reads its source bytes from
     the baseline — re-baselining between attempts would let a wrong first attempt change
     what the operator's expectation *means* mid-job. Per-turn changesets stay per-attempt:
     "what this attempt changed" is a claim about this attempt's calls, and inheriting a
     previous attempt's residue would make it false.
   - **Infrastructure is never retried.** Transport, a refused session, an unreadable tree
     and misconfiguration stop the job; an undecidable-only verdict does too, since "one
     side could not be read" is never sent to the model. A bound cutoff with an unmet
     finding *is* retried — the judge's evidence is no weaker for the model having been
     stopped mid-stride.
   - **The retry prompt carries the task, not just the failure.** "falcon-index.md does not
     exist" alone does not say what the file should contain; the task does. Composed from
     the original task every time, so the framing cannot nest by the third attempt.

5. ~~**Clear D7's gate, which is two conditions and not one.**~~ **Done 2026-08-26.**
   ⚠️ Both were required before the programmatic frontend ships; a programmatic caller is exactly
   the one D6's threat model did not cover.
   - **Kernel confinement** — vendor D10's Landlock routine, `fork` → restrict → `exec`, one
     writable directory. Probe by attempting a denied write and requiring `EACCES`, not by
     running a command that succeeds. This also disposes of the hardlink gap's *creation* half;
     a link planted before the sandbox starts is still reachable, and that is now recorded
     rather than open. **Done 2026-08-22.**
   - **`openat(O_NOFOLLOW)` component-walking** — the in-root correctness half, which
     confinement does not supply. A swap that redirects to a different file *inside* the root is
     permitted by the kernel and is D6's own worked example.

     **Settled 2026-08-16, out of PR #6's review: allow the semantics, funnel the spelling.**
     Tools never spell `open()` themselves. One core primitive beside `Sandbox` carries every
     open from the first tool onward — plain `open` with `O_NOFOLLOW` on the final component
     until this step lands (free now, and it already catches a final-component swap), the
     component walk plus a post-open identity check after. Direct opens today would sit inside
     D6's accepted race either way; the funnel exists so clearing this gate swaps one function
     body instead of rewriting eight tools' I/O — and so the widened parse-to-use window PR #6
     introduced (arguments resolve before the tool runs) is closed at the same single site.

     **Scope widened 2026-08-16, out of PR #9's review: the gate must also cover
     *publication*.** The mutating tools create parent directories, `link()` and `rename()` by
     resolved path, and those calls follow *interior* symlinks — a window wider in kind than
     the final-component open D6 accepted, and one D10 does not backstop, because Landlock
     confines only the shell child while these writes run in the unconfined parent. A
     pre-planted interior link is caught at resolve time; only a concurrent swap in the
     resolve-to-publish window — the same actor class as D6's race, outside the stated threat
     model — can exploit it. Clearing this gate therefore means converting publication to
     `mkdirat`/`linkat`/`renameat2` under the same walked root descriptor, not just swapping
     `open_in_root`'s body. Until then the window is accepted *and named*, here.

     **Done 2026-08-26.** `fsio.h`'s `open_in_root` and new `open_parent_in_root` both walk
     from `SandboxPath::sandbox_root()`, a copy of the root each resolved path now carries.
     That's simpler than threading `Sandbox&` through every one of the eight tools plus
     `supervisor/semantic.cpp` and `supervisor/undo.cpp` that already call these primitives to
     read.
     Each walk opens one component at a time via `openat(O_DIRECTORY | O_NOFOLLOW)` from a
     freshly-opened root directory fd, not from `SandboxPath::path()`'s pre-expanded string,
     so a symlink swapped into any component after `resolve()` is refused at that hop and
     never followed (`ELOOP` on the final component, `ENOTDIR` on an interior one — verified
     against this kernel, not assumed). Publication now goes through `open_parent_in_root`
     plus `mkdirat`/`linkat`/`renameat2` on the walked directory fd, exactly as this step
     named: `write.cpp`, `edit.cpp`, `move.cpp`. The "post-open identity check" this step's
     original text calls for turns out to be the walk itself: every component is re-derived
     fresh at open time, so there's no separate stat-then-open step for a concurrent swap to
     race against, and no new mechanism was needed. The one thing the walk still can't see —
     unlink-then-recreate under an unchanged name — stays `ObservedState`'s job, unchanged.
     New coverage: `fsio_test.cpp` at the primitive level, `mutate_tools_test.cpp` end to end
     through the actual tools, each planting the swap in the real parse-to-use window
     (`parse_args` resolves, the filesystem changes, then `invoke` runs). Full account in
     [DECISIONS.md](./DECISIONS.md), D6's "Closed 2026-08-26" paragraph.

     **One call site missed the first pass, caught by review the same day:**
     `supervisor/undo.cpp`'s `restore()` already used these primitives to read the backup and
     the file being replaced, but still published — parent directory, temp file, final rename —
     off `target->path()`'s pre-expanded string, the exact pattern just converted everywhere
     else. Now converted too, via `open_parent_in_root` and a new `open_temp_in_dir` (streaming,
     since a restored backup has no size cap to buffer first). See DECISIONS.md D6's follow-up
     paragraph for why this one has no dedicated race-window test: `restore()` has no
     `parse_args`/`invoke` seam to plant the swap in.
6. ~~**`mcp.cpp`** in `app`.~~ **Done 2026-08-28.** `hermit mcp --root DIR` — a subcommand
   beside `agent`/`undo`, not a second executable — reads JSON-RPC on stdin and writes it to
   stdout (D7). Its tool schema and dispatch are exactly the two things this step promised
   would need no second implementation: `tools/list` renders through a new
   `supervisor::mcp_tool_definitions()`, sharing `wire.cpp`'s existing schema builder with the
   Ollama-facing `tool_definition()` rather than restating it, and `tools/call` runs through
   the same `supervisor::dispatch_call()` the CLI's own loop already used. `shell` stays off
   this surface by default, gated the same way `agent_command` gates it: an explicit config
   flag plus a live `probe_confinement() == Enforced` check, hard refusal otherwise. See
   [chapter 20](./docs/20-mcp-and-kiro.md) for the deployment shape and hermit-bench's E2
   protocol (`delta/E2-PROTOCOL.md`), whose one named prerequisite this was.
7. **Tier 1** (`triage`, `summarize`) in `supervisor`, once model selection is settled. Note
   [D12](./DECISIONS.md): these are the callers `format` is *for* — a structured reply with no
   tools offered, which is the one configuration it was re-measured working in.

Independent of the above, and cheap:

8. ~~**Re-run `bench/fsops/`.**~~ **Done 2026-08-26** — 360 runs, six models, five repeats,
   sampling pinned, trees outside any repo. Written up as sweep 3 in the benchmark repository
   (`hermit-bench/fsops/SWEEP3.md`), which is where the suite and its results now live; the
   copy under `bench/fsops/` here is legacy and kept only because the in-repo docs cite it.

   What it settled, and what it broke:

   - **`gemma-e4b` wins the suite**, 54/60 (90%) at a 9.4s median — better accuracy than
     `qwen3.5:9b` at roughly a seventh of the wall clock. It had gone untested through two
     sweeps because its base was deleted for disk space; re-pulling took 4.4 seconds. Sweep 2's
     "no fast, accurate, small option exists in this field" is false as stated, and step 7's
     model-selection question now has a different leading answer than the one the old tables
     imply. Re-check it under E1's protocol before it moves any default.
   - **The escape fix is confirmed**, and the control's 3/3 → 0/3 swing that motivated the whole
     re-run resolves to a clean 5/5. The escape also depressed sweep 2 beyond the three tasks
     its banner named, so those totals are lower bounds by an unknown margin.
   - **Reasoning has never been a controlled variable**, in any sweep. `--reasoning` defaulted
     to `None` and was passed only when truthy, so every sweep silently used the config's
     `medium` while recording `none`; and fixing that is not sufficient, because the level does
     not reach the wire under `--provider custom` at all — transcripts at `none` and `medium`
     are byte-identical. Ollama honours the parameter when it is genuinely sent, so this is a
     provider-layer gap. Worth noting the supervisor is not exposed to it: [D8](./DECISIONS.md)
     puts Hermit on native `/api/chat`, not the endpoint where the level goes missing.
   - **`--deterministic` works, but its repeats do not measure sampling noise.** The per-run
     working directory embeds the repeat index and reaches the model, so each repeat is a
     different prompt and a flip measures path sensitivity. Repeats are not independent samples:
     the unit for any significance claim is 12 tasks, not 12×N runs. This is why the
     "more than 3 repeats" line below could not do what it was asked to do.

   The original text follows, since the bullets below are what the run was measured against.
   Both sweep documents carry the banner *"Re-run before treating
   any per-task number as settled."* Per [NEXT-RUN.md](./bench/fsops/NEXT-RUN.md):
   - The working-tree escape is **already fixed** — runs now live in `~/.cache/hermit-fsops/runs`,
     outside any git repo. The published scores simply predate that fix.
   - `01_create_file`, `02_make_dirs` and `08_write_and_run_script` are the three with confirmed
     escaped artifacts and are the minimum re-run.
   - Use more than 3 repeats and a `--deterministic` pass, to separate sampling noise from
     capability.
   - **Do not enable `--transcripts` for a full sweep.** The recording proxy destabilises long
     runs (`qwen-4b`: 0 timeouts without it, 4 in 7 with) and that bug is unfixed. It is a
     single-task diagnostic only.
   - `gemma-e4b` still has no data and is now unblocked — the tag is present locally.

   This is what turns the model-selection question in step 7 into a measurement. The re-run
   also doubles as the baseline arm of `bench/delta`'s reliability experiment — same tasks,
   same model, collected once, used twice; design in
   [bench/delta/DESIGN.md](./bench/delta/DESIGN.md).

9. **The substrate probe** ([D11](./DECISIONS.md#d11--the-substrate-is-probed-not-assumed)) —
   independent of every step above and needs no link edge, because it touches no model and no
   network. It validates §4's identity tuple against the ground it is actually standing on,
   which is a Linux concern today (`/mnt/c`, network mounts, `tmpfs`, overlayfs) and not a
   Windows one.

---

## A note on numbers

Any token or cost figures discussed alongside this document are **estimates from byte counts, not
measurements** — roughly 4 bytes per token for source code, ±20%. They were used to decide whether
the architecture is worth building, which they support comfortably at a third of their nominal
value. They are not a performance claim and nothing here depends on them.
