# Routing — the tool surface, and who is allowed to call what

Drafted 2026-08-15. **Nothing here is implemented yet.** This settles the surface
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
| `list` | Directory entries: type, size, hash | Hash per entry |
| `find` | Paths matching a name/glob pattern | — |
| `grep` | Content matches as `path:line:text` | — |
| `hash` | Content hashes for a path set | *is* the verification (R3) |
| `write` | Write, read back, compare | R5; R4 backup |
| `edit` | Exact `old` → `new`, read back, compare | R5; R4 backup |
| `move` | Hash source, move, verify destination | R3 both ends |

`find` and `grep` are a deliberate split of the roadmap's single `search` — name matching and
content matching have different inputs and different failure modes. `hash` is new to the roadmap
and earns its place by making R3 and R6 cheap enough to run after every turn.

**Each call is one complete job.** "Low-level" here means *dumb* — no judgment, no interpretation,
no guessing what was meant — not *granular*. `move` moves the file, hashes it, confirms it
arrived, and reports; it does not expose `open`/`link`/`unlink` for the caller to compose. Every
tool call is a round trip and a chance to go wrong, and
[SWEEP2](./bench/fsops/SWEEP2.md) shows these models failing on multi-step composition, not on
individual operations.

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

### Deferred — `delete`

Recorded in §11 with the conditions that would unblock it.

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
| **Per-turn** | Everything, including shell and tools we did not write | R4 snapshot, R3 hash-diff, R6 poll state |

Per-turn is the one that holds unconditionally, and it is a **supervisor** concern
([Phase 3](./ROADMAP.md)), not a tool concern. Per-tool verification is worth having anyway —
it converts a silent wrong write into a loud failure at the point it happens — but a design that
relied on it alone would be trusting the model to use our tools.

---

## 7. Where the code lives

[D7](./DECISIONS.md)'s layering table already assigns every piece:

```
src/hermes/core/tools/        Tier 0. Cannot reach a model.
src/hermes/supervisor/tools/  Tier 1. Links the transport.
src/hermes/app/mcp.cpp        The frontend. JSON-RPC over stdin/stdout.
```

**The tier boundary is enforced by the build graph, not by discipline.** `hermes_core` links
neither `hermes_ollama` nor anything that could reach it, so a Tier 0 tool that tried to call a
model would fail to link. This is the same property `SandboxPath` gives R1: structural, and still
true at tool #37.

### Two link edges are missing and must be added first

Verified against `CMakeLists.txt` on 2026-08-15:

1. **`hermes_supervisor` does not link `hermes_core`.** A Tier 1 tool placed there today could not
   construct a `SandboxPath` — it would be R1-unsafe by construction, the exact inverse of the
   property this codebase is built on. The edge is required before `triage` or `summarize` exists.
2. **`hermes_app` does not link `hermes_supervisor`.** Only the executable links all four targets,
   so registry composition cannot happen in `app` as the frontend layer needs. The edge is
   required before `mcp.cpp` exists.

### Tool descriptors stay JSON-free in `core`

[D4](./DECISIONS.md) gives tools an `Args` struct with field descriptors "for schema generation
and parsing" — both JSON operations. But [D2](./DECISIONS.md) deliberately put `nlohmann` in the
build *with the client*, and `hermes_core` links no JSON library.

The resolution is better than widening `core`: **descriptors are pure data** (name, type,
required, documentation) with no JSON types, and rendering them to JSON Schema happens in a layer
that already has `nlohmann`. D4's one-declaration guarantee survives intact — the rendering simply
is not in `core`.

That guarantee then extends further than D4 claimed. One descriptor list emits **both** the
schema sent to Ollama as `format` (D5's constrained decoding) **and** the MCP tool definition the
programmatic caller reads. There is no second schema to keep in sync, so R2 drift stays
unrepresentable across the MCP surface too.

---

## 8. Frontend exposure is a policy, not a fixed set

Both frontends drive the same core; they need not expose the same subset.

**`shell` is off the MCP surface by default.** Enabling it is an explicit configuration field that
prints as a marked line, the same treatment a raised `max_num_ctx` clamp or a waived tools gate
already gets.

The reason is §4's, not a guess about who is calling. D7 notes that a programmatic frontend
changes the threat model — paths derived from a document someone pasted into a chat are
prompt-injection-influenced input reaching a filesystem API. Every other tool contains that by
type. Shell cannot, because there is no path in a command string to resolve. Handing an
arbitrary caller an unsandboxable string executor is the blast-radius expansion D7 exists to
prevent.

**This is deliberately not a judgment about the caller.** Whether a caller already has a shell is
a property of *its harness*, not of the model behind it — Claude Code has one, a bare MCP client
does not, and an unknown future harness could be either. A rule that depended on knowing which
would be wrong the first time someone new connected.

---

## 9. Machine differences are configuration, never code

**There is no laptop build and no desktop build.** One binary, one tool surface, no `#ifdef`, no
stripped variant. Tier 0 needs no GPU at all — it is filesystem syscalls — so it runs at full
fidelity anywhere. The only thing that scales with hardware is Tier 1, which is the tier that was
always optional.

Choosing a model appropriate to the machine is an **operator responsibility**. The model and base
URL are already `hermes_app` settings, so this is a config file per machine.

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

Concurrency therefore lives in the **caller's harness**, expressed as more `hermes-cpp` processes.
Nothing inside Hermes gains a thread. The join is `hash` — deterministic, and a Tier 0 call.

---

## 11. Open, and deliberately deferred

### `delete` — deferred, two conditions

Not in the roadmap's tool list, and this project exists partly because a tournament run erased
`tally.py`. It is added only when **both** hold:

1. **Undo exists and works.** R4 says "undo is a first-class operation, not a debugging aid," and
   there is currently no design for where backups live, how long they are kept, or how undo is
   invoked. Every other mutating tool overwrites content a snapshot holds; `delete` is the only
   one whose failure is irreversible if that story is missing. **This is the load-bearing
   condition** — model confidence without working undo only means being wrong less often.
2. **A retested `06_selective_delete` holds for the model being built against** — on the current
   harness, with more than 3 repeats and a `--deterministic` pass. SWEEP2 §4
   demonstrates that per-cell numbers at n=3 are not capability: the control moved 3/3 → 0/3 on
   the simplest task in the suite, same machine, same night.

Until then, `write` covers truncation and shell covers the rest for the local model.

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

Ordered. Steps 1–4 are Phase 2 and 2.5 as already written; only the tool list is new.

1. **Add the two link edges** (§7) — `supervisor → core`, `app → supervisor`. Prerequisite for
   everything below, because they decide which target `tool.h` can live in.
2. **`tool.h`** — the D4 base class and JSON-free descriptors. Everything inherits it.
3. **The eight Tier 0 tools** in `core`, with tests.
4. **Close the TOCTOU race** — `openat(O_NOFOLLOW)`, one component at a time — and answer the
   hardlink gap (device/inode comparison against the root, or accept it explicitly and record
   why). ⚠️ **This is a gate**, per D7: a programmatic frontend is exactly the caller D6's
   threat model did not cover.
5. **`mcp.cpp`** in `app`. Callable from here on.
6. **Tier 1** (`triage`, `summarize`) in `supervisor`, once model selection is settled.

Independent of the above, and cheap:

7. **Re-run `bench/fsops/`.** Both sweep documents carry the banner *"Re-run before treating
   any per-task number as settled."* Per [NEXT-RUN.md](./bench/fsops/NEXT-RUN.md):
   - The working-tree escape is **already fixed** — runs now live in `~/.cache/hermes-fsops/runs`,
     outside any git repo. The published scores simply predate that fix.
   - `01_create_file`, `02_make_dirs` and `08_write_and_run_script` are the three with confirmed
     escaped artifacts and are the minimum re-run.
   - Use more than 3 repeats and a `--deterministic` pass, to separate sampling noise from
     capability.
   - **Do not enable `--transcripts` for a full sweep.** The recording proxy destabilises long
     runs (`qwen-4b`: 0 timeouts without it, 4 in 7 with) and that bug is unfixed. It is a
     single-task diagnostic only.
   - `gemma-e4b` still has no data and is now unblocked — the tag is present locally.

   This is what turns the model-selection question in step 6 into a measurement.

---

## A note on numbers

Any token or cost figures discussed alongside this document are **estimates from byte counts, not
measurements** — roughly 4 bytes per token for source code, ±20%. They were used to decide whether
the architecture is worth building, which they support comfortably at a third of their nominal
value. They are not a performance claim and nothing here depends on them.
