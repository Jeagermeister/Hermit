# The Go rewrite — plan of record (proposed)

**Status: draft, proposed 2026-10-19, not scheduled by existing.** This file came out of a
brainstorming session with the operator and follows [DOCKET.md](./DOCKET.md)'s discipline:
every claim carries a *why*, every milestone a *done when*, and the whole plan a *struck if*.
It is written in this repository because this is where the design record lives; on adoption it
moves to the new repository with the record (see *Repository split*) and a pointer replaces it
here.

Three operator decisions frame it, made 2026-10-19:

1. **A full Go rewrite, clean break** — not a Go front end over the C++ core, not a hybrid.
2. **A new repository; this one archived** as the frozen reference.
3. **v0's TUI leads with the launcher and the watch view** — a form that builds and follows
   bounded jobs — not with a chat REPL.

---

## Why

An item with no *why* is a preference. This one has four, three of them already recorded in this
repository's own documents, and the fourth named honestly.

**1. D1 structurally blocks the product direction, and the blockage is already on the record.**
One blocking thread per process ([D1](./DECISIONS.md)) is why DOCKET 1.31 had to design
`hermit watch` as a *separate process reading a trace file* — the supervisor cannot redraw
during a 60-second model call. It is why streaming is set aside (DOCKET Part 2). And ROADMAP
Phase 2 records the sharpest instance: the loop "cannot interrupt a request already in flight,
so the real worst case is the budget plus one `chat_timeout`" — the open bounded-execution
question in DECISIONS.md. In Go, `context.Context` cancellation plus a goroutine per session
closes that gap outright: R8's wall-clock bound becomes a real cancellation, live watching
becomes in-process, and the multi-root design (DOCKET 1.10) stops needing N processes. This is
not a rewrite to get a nicer language; it is a rewrite because the recorded open problem and the
recorded front-end designs both route through the concurrency model.

**2. Distribution is the language.** DOCKET 1.4: "Someone who reads the README and wants to try
it has to build a C++23 project… Every large project this is meant to compete with is one
command to install." Go ships static, zero-dependency, cross-compiled binaries with a checksum
and a tag as a matter of course. 1.4's shape — reproducible release recipe, PKGBUILD, install
before build in the quickstart — gets *easier* in Go rather than merely possible.

**3. The one hot loop gets faster for free.** The verifier's SHA-256 is a portable software
implementation at ~60 MB/s, "roughly 70x slower than a hardware-accelerated `sha256sum`"
(ROADMAP Phase 3, verifier gaps). Per-turn verification is the only CPU work this product does
that a model turn does not dwarf, and DOCKET 1.3 exists precisely because its cost at scale is
unmeasured and feared. Go's `crypto/sha256` uses SHA-NI out of the box. 1.3's measurement still
has to run — but it starts from a better constant.

**4. The operator wants to build it.** Recorded honestly, in the manner this repository records
arguments versus measurements: the pull toward Go and a TUI is partly motivation, and a personal
project that dies of grind loses all of its value. The three structural arguments above are the
load-bearing ones; this one is why now.

**The counter-arguments, stated at their real width.** The README's "Why native code" section
argues startup against *Python* (1–3 s of interpreter); Go starts in ~1–2 ms, and both are noise
beside a 6–17 s model turn — that argument is a wash, not a loss. What is genuinely at risk is
[D6](./DECISIONS.md)'s sandbox: the POSIX-order resolution walk, `openat`/`O_NOFOLLOW`, the
Landlock confinement of [D10](./DECISIONS.md). In Go none of it may use `os.Open` — every
path-touching call must go through hand-rolled `x/sys/unix` syscalls — and `go-landlock` is a
dependency whose behaviour must be measured against D10's probe, not trusted. That is why
milestone 1 is the crown jewels and the abort gate (below): the idea dies at the cheapest
component that can kill it, the same rule the discovery cluster wrote for itself.

---

## What carries over, and what must be re-earned

**Hermit is the design record, the requirements, and the evidence — not the C++.** Those port
for free; the code does not port at all, it gets rebuilt against them.

| Asset | Disposition |
|---|---|
| [REQUIREMENTS.md](./REQUIREMENTS.md) R1–R9 | **Carries over verbatim.** Every requirement is traced to a measured failure; none of the failures was about C++. |
| [DECISIONS.md](./DECISIONS.md) D1–D19 | **Carries over except where amended below** (D1 relaxed; see *Proposed decisions*). D6, D7's layering, D13, D15, D17, D19 are architecture, not language. |
| [SCOPE.md](./SCOPE.md) platform decision | **Carries over unchanged.** Linux is the product; Windows remains a separate hypothetical package; macOS is not a target. Go makes cross-compilation *possible*, and possibility is not a requirement — building portability before value is still backwards. |
| `tests/` — 821 tests, 13k lines | **The acceptance spec.** Port test *cases*, not test code. The four review rounds' defect classes (ROADMAP Phase 1) become deliberate regression tests in the new language: the `-1`-as-`uint64_t` clamp wrap, `ifstream` opening a directory, the two-parsers-one-command-line divergence, `num_predict` negative-means-unlimited, the token-collapse cliff detector. |
| `bench/fsops/` (259 runs), E1–E5 protocols and results, [docs/91-commit-map.md](./docs/91-commit-map.md) | **Carry over as the frozen evidence base.** The numbers were collected against the C++ binary pinned at specific commits, and they stay pinned there. |
| `src/` — ~15k lines of C++ | **Reference only.** Read it while porting; do not transliterate it. The behaviours that were *argued* (D6's resolution order, the staleness guard, `edit`'s exactly-once rule, config's three honest answers for relative paths) must be re-derived and re-tested, because the failure modes are different in a different language, not absent. |

**The parity rule, written while it is cheap:** the Go binary **inherits no benchmark numbers**.
It may cite E1's 74%→94% only after an E1 parity re-run — same frozen protocol
([bench/delta/E1-PROTOCOL.md](./bench/delta/E1-PROTOCOL.md)), same model tags, same machines —
lands in hermit-bench with the Go commit pinned, losses recorded beside wins. Until then the Go
README says what the C++ README said about hermit-bench before it was runnable: named, not
linked, not claimed.

---

## Shape

### The seam that defers the product-shape question

The operator is undecided between Hermit-as-agent (a supervised chat manager) and
Hermit-as-manager-of-agents (QA over other agents). The rewrite does not need the answer,
because both are the same machine with a different **mutation source**:

```
snapshot the tree → [ mutation source does work ] → hash-diff the tree
  → judge expectations (structural, then D15's semantic, labelled as judgment)
  → unmet? re-invoke with one concrete remaining failure (R7, fresh session)
  → backup/undo store throughout (R4, D14)
```

| Source | What it is | Status in the plan |
|---|---|---|
| **A — built-in chat loop** | Hermit's own Ollama loop with the eight-tool menu (today's `hermit agent`) | Milestone 3; the engine's first and default source |
| **B1 — MCP child** | An external agent (Kiro, OpenCode, any MCP client) calls Hermit's tools over stdio; verification is per-call, as today | Milestone 4; already exists in C++, ported |
| **B2 — supervised outsider** *(new, unscheduled)* | An external agent runs loose inside the root; Hermit checkpoints around it — snapshot, verify, judge, undo at turn boundaries it infers from the tree, never from the agent's transcript | Designed-not-built. Recorded here so the seam is not accidentally foreclosed; it needs its own docket entry with a *why* before code |

The engine core must therefore treat "who mutates" as an interface from day one, and the trace
(below) as the record every source writes into. Nothing else about the architecture changes.

### Concurrency, amended

D1's *semantics* survive; its *mechanism* changes. One supervisor session is still a linear
sequence of turns, and the loop still never reads replies to decide completion (D13 untouched).
What changes: each session runs in a goroutine; every model call and every walk carries a
`context.Context` that R8's wall clock can cancel — closing ROADMAP's in-flight-interrupt gap;
the TUI renders from the event stream without a second process. The amendment is a proposed
decision entry (below), with this plan as its evidence.

### The trace is designed in, not bolted on

DOCKET 1.1's structured run trace (one JSONL per run: config as rendered, turns, tool calls with
verdicts and hashes, verification passes, judge calls, exit) becomes part of the engine's first
cut, because three consumers need it at birth: the TUI's watch view, hermit-bench's parser, and
B2 if it ever lands. D1.1's rule stands: nothing in the trace is read back by the loop — it is a
record, not state.

### v0's front end: launcher + watch, not chat

Milestone 5 ships DOCKET 1.31's steps 2 and 3 fused into one session-manager TUI (Bubble Tea):
pick a model from those passing preflight, pick a root, state expectations with 1.25's
shortcuts, launch a bounded job; then watch it live — turn N of M, elapsed against budget,
tokens, the last tool call passed/refused, the changeset as a tree, the verdict as a table —
and browse the undo store. A form that builds a job, not a conversation. The chat REPL stays
set aside (DOCKET Part 2) *for v0*; the plain CLI and `--plain` survive unchanged because
hermit-bench parses them. Whether the REPL arrives later is the product-shape question, and the
demand test (1.30) — finally runnable, because install becomes one command — is what answers it.

---

## Proposed decisions to record

On adoption these become DECISIONS.md entries in the new repository, argued at their real length
there; named here so nothing is decided silently:

- **D20 (proposed) — The implementation language is Go.** Evidence: the four *whys* above.
  Overturns: nothing; amends the README's "Why native code" from "C++" to "a compiled language
  with a static binary and real concurrency", which is what the section's own arguments actually
  needed.
- **D21 (proposed) — D1 amended: bounded concurrency with cancellable contexts.** One goroutine
  per session, `context` cancellation on every blocking call, R8 as real interruption. Overturned
  by: any measurement showing the race surface costs more than the interrupt buys — the suite
  runs under `-race` from milestone 1, which is where that measurement would come from.
- **D22 (proposed) — The repository split, and the name.** Decided 2026-10-20: the Go project
  takes the name **Hermit** — new repo authoritative on Gitea as `Jeagermeister/Hermit`, Go
  module `github.com/Jeagermeister/Hermit` (the house convention `Apocrypha.TUI` set), binary
  stays `hermit`. This repository is renamed **`Hermit-cpp`** and archived read-only (never
  deleted — DOCKET 1.14's lesson is that remotes keep serving what you think is gone, and the
  archived repo must keep resolving the SHAs that E1–E5 and the commit map pin). The design
  record and the in-repo bench analysis documents are *copied*, not moved; the renamed repo
  stays frozen as cited. The backronym survives untouched — every letter names a requirement,
  and the requirements carry over.

---

## Milestones

Each is done when its observable is met, in the order given; milestone 1 is the abort gate.

| # | What | Done when | Size | Needs |
|---|---|---|---|---|
| 0 | **Freeze.** Tag this tree `v0.1.0-cpp` on the commit the benchmark numbers pin (DOCKET 1.4's tag rule), archive the repo read-only, stand up the new repo with the copied design record, Go module, CI from day one (absorbing DOCKET 1.9: both linters' worth of vet + `-race` suite on every PR) | The tag resolves; the new repo's first commit builds and runs an empty suite green in CI | small | none |
| 1 | **The crown jewels: sandbox, fsio, sha256.** `Sandbox::resolve`'s POSIX-order walk re-derived over `x/sys/unix` (`Openat`, `O_NOFOLLOW`, `RENAME_NOREPLACE`), the capability type re-expressed as an opaque struct constructible only by the resolver, Landlock confinement via `go-landlock` measured against D10's probe, SHA-NI hashing. The 42 sandbox tests re-expressed as spec; `go test -fuzz` on `resolve()` from day one (absorbing DOCKET 1.6's first target) | The ported suite green under `-race`; the D10 confinement probe refuses an unconfined shell exactly as the C++ binary does; fuzz target runs one hour without a finding; SHA throughput measured against the C++ 60 MB/s baseline with the ratio recorded | large | none |
| 2 | **Verifier, judge, expectations.** Tree snapshot/diff with carry-forward hashes and `force_rehash` (D13's amendment), the structural judge, the expectation grammar with its four argued rules (literal-path keying, `..` refused, impossible sets refused, unreadable tree ⇒ all `Undecidable`), D15's semantic judge session | Verdicts reproduce against recorded E1 fixtures; ROADMAP 1.3's scale measurement finally runs, on SHA-NI, and lands as a table | medium | none |
| 3 | **The loop.** Ollama native client (D8: `num_ctx` set per request, the clamp, loopback-only), preflight R9 (fail-closed, "undetermined" honesty included), session/context budget with the cliff detector and D17's rebuild-from-tree compaction, the turn loop with context cancellation, reinvoke R7 | One supervised run end-to-end on a 9B model; a wedged daemon is *interrupted* at the R8 bound, demonstrated, not asserted | large | 9B tier |
| 4 | **MCP front end** (official Go SDK, stdio only — D7's no-listening-port rule untouched) plus the CLI's plain surface | Today's Kiro/OpenCode registration works against the Go binary unchanged; `--plain` output parses with hermit-bench's existing parser | medium | none |
| 5 | **TUI: launcher + watch** (Bubble Tea/Lipgloss/Bubbles), reading the 1.1 trace in-process | The operator uses it daily for one week; `hermit doctor` (DOCKET 1.23) and pasteable MCP config (1.24) ship as compositions of existing calls | large | 9B tier to demo |
| 6 | **E1 parity re-run** in hermit-bench, protocol frozen before the first run | The Go binary may cite the numbers, at their real width, with its own commit pinned — whatever the result says, including if it is worse | medium | 9B tier, both machines |

---

## Library posture

D3's discipline carries over: pinned versions, fetched by the build, no system dependencies.
The candidate set, each replaceable if review objects:

| Need | Candidate | Note |
|---|---|---|
| syscalls | `golang.org/x/sys/unix` | Non-negotiable; the resolve path touches nothing else |
| Landlock | `github.com/landlock-lsm/go-landlock` | Maintained beside the kernel feature; **measured against D10's probe in milestone 1, not trusted** |
| hashing | stdlib `crypto/sha256` | SHA-NI is the point |
| MCP | `github.com/modelcontextprotocol/go-sdk` | Official; stdio transport only |
| Ollama | stdlib `net/http` | The client is five endpoints and a clamp; a dependency would be larger than the code |
| TUI | Bubble Tea + Lipgloss + Bubbles | MIT, the ecosystem's centre of gravity |
| testing | stdlib `testing` + `-race` + `-fuzz` | The red/green discipline is the framework |

Open: CLI flag parsing (stdlib `flag` versus a library) — the C++ config's four-source overlay,
per-field provenance and two-parser-drift lesson (ROADMAP Phase 1) constrain whatever is chosen;
decide in milestone 0, not mid-build.

---

## What this plan absorbs, and what it leaves alone

**Absorbed** (strike or fold in the new repo's docket): 1.1 (designed in), 1.4 (Go releases),
1.6 (native fuzzing, first target), 1.9 (CI from day one), 1.23/1.24/1.25 (milestone 5's
compositions), 1.31 (v0 *is* steps 2+3; step 1 ships as `--plain`-preserving formatting).
1.3 stops being a fear and becomes a milestone-2 table. 1.13's problem is prevented by never
rewriting history again — a rule worth writing into CONTRIBUTING when 1.5's turn comes.

**Unchanged and waiting:** 1.2 (template probes — port with preflight), 1.7 (substrate matrix —
still needs root and loop mounts, language-independent), 1.8/1.10 (wait for a caller/workload,
though D21 makes 1.10 cheaper when it comes), 1.11, the discovery cluster 1.15–1.21 (its gate
passed; the cluster is language-independent and replays recorded trees), 1.26/1.27 (speed
measurements — re-run against the Go binary when the trace carries their numbers), 1.28 (the
thirty-second demo — record it against whatever binary installs in one command), **1.30 (the
demand test — more load-bearing than ever; the rewrite makes it runnable and cannot substitute
for it)**.

**What this plan does not do.** No chat REPL in v0. No Windows port. No streaming in the
supervisor's verification path — D13 never reads the reply, and cancellation is not streaming.
No inheritance of any measured number. No deletion of the old repository, its objects, or its
mirror. The threat model does not change: D6's race is still accepted against a confused model,
and D10's hardlink reasoning still stands — the rewrite re-earns the guarantees, it does not
renegotiate them.

---

## Struck if

- **Milestone 1 fails**: Go cannot hold D6/D10's guarantees — `go-landlock` proves inadequate
  against the probe, or the hand-rolled resolve path cannot reproduce the 42 tests' semantics.
  Then the cheapest kill point has killed it, the C++ tree is still tagged and archived, and the
  record says why in one paragraph rather than in a half-ported codebase.
- **The operator stops wanting it** for two consecutive scheduled sessions — motivation is a
  stated *why* of this plan, so its absence is a strike condition, recorded rather than
  discovered silently.
- **1.30 returns null** (all three outside users drop it): the product frame retracts in public
  as 1.30 already promises, and the Go project continues only as the personal tool the SCOPE
  platform section always said it was — which is a legitimate outcome, not a failure to hide.

**Size.** Large — six milestones, the first two of which are the safety-critical core.
**Needs.** none to build through milestone 4; 9B tier for 3, 5, 6; Kitchen and the laptop for 6.

## Open questions (settle in milestone 0, not mid-build)

1. ~~**Name.**~~ **Decided 2026-10-20 — recorded in D22 above:** the Go project takes
   `Hermit`, this repo is renamed `Hermit-cpp`. The rename churn (doc links that name the repo,
   the mirror) is accepted deliberately, once, now, rather than paid twice later.
2. **Mirror handling.** The GitHub mirror follows the rename, *by delete-and-recreate* only
   after 1.14's lesson is re-checked — the archived Gitea repo is authoritative and must keep
   serving the pinned SHAs regardless. Operator-side, at freeze time.
3. **Go version floor.** go1.27.1 is installed on the laptop; `go.mod` records 1.27 as the
   floor at milestone 0, bumped deliberately — the D3 posture applied to the toolchain itself.
4. **Attribution.** Upstream Hermes Agent remains the inspiration, MIT-compatible, not ported;
   the C++ repo's attribution rules carry over verbatim, and the keep-AI-local and scrub
   disciplines of this repo's history apply to the new one from its first commit.
