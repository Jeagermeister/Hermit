<p>
  <img src="assets/logo/hermit-readme-320.png" alt="Hermit" width="200">
</p>

# Hermit

**Never trust a completion claim; check the tree.**

Hermit is the hands an AI agent uses on your filesystem, with less authority than the agent
would otherwise have. Every call runs inside one sandbox root; every mutating call is verified
by hash, and the whole tree is re-hashed after every turn; anything the file tools overwrite,
or the opt-in `delete` removes, is backed up first to a store the model cannot reach; and when
the model says "done", Hermit inspects the tree instead of believing it. A stated
post-condition the tree does not meet goes back to the model as one concrete failure, in a
fresh session, up to three times.

It is driven two ways ([D7](./DECISIONS.md)):

- **By you, with a small model on your own GPU.** `hermit agent` hands a 3–12B model running
  through Ollama a menu of eight structural tools, and nothing leaves the machine unless you
  pass `--allow-cloud`. This is the default, and every measurement below comes from small
  local models.
- **By a larger agent, over MCP.** An agentic IDE registers Hermit as a stdio MCP server and
  calls it for filesystem work. A typical stdio MCP server runs with all of the launching
  user's authority; this one reduces it. The frontend is built and shipped. How it fares under
  an outside caller is **not yet measured**: hermit-bench's E2 experiment and
  [DOCKET 1.30](./DOCKET.md) are where it gets measured.

The two halves of the value age differently. The retry half shrinks as models improve. The
containment and verification half does not: no model of any size should hold more filesystem
authority than the task needs, or be believed about what it changed. That is an argument, not
a measurement, and the [FAQ](./FAQ.md) makes it with what it concedes.

It exists because of a measurement, not a preference. Small local models can do this work, and
they misreport having done it: across the 259 runs in [`bench/fsops/`](./bench/fsops/) and the
sweeps that followed, models announced success over untouched trees, "copied" a file by
re-typing it one byte wrong, and erased a `tally.py` nobody asked them to touch. The tournaments'
own recommendation was a supervisor outside the model, checking state and re-invoking with the
one remaining failure. This is that supervisor, in C++.

**What it buys, stated at its real width.** Seven tasks, five repeats each, the same 9B model
pinned at a 65,536-token window on one machine, one referee applied to the filesystem and never
to the transcript — 105 runs:

| | Hermes Agent as shipped | Hermit, 1 attempt | Hermit, 3 attempts |
|---|---|---|---|
| tasks completed | 26/35 (74%) | 33/35 (94%) | 33/35 (94%) |

Four task-level wins, zero losses, three ties: p = 0.125, which seven paired tasks cannot
improve on, reported as underpowered rather than as proof. In the 70 supervised runs nothing was
destroyed, nothing escaped the sandbox, and no undeclared file changed. The protocols, the
graders and every run behind that table live in
**hermit-bench**, a separate repository, frozen before the first run, with the losses recorded
beside the wins. It is not released yet — see *The evidence sits in three places* below.

The name is a backronym, kept because each letter names a requirement or a design rule with a
measured failure behind it ([REQUIREMENTS.md](./REQUIREMENTS.md)): **H**ash-verified ·
**E**vidence-driven · **R**etries with concrete failure · **M**inimal structurally-enforced
authority · **I**ndependent verification · **T**iered dispatch.

## Start here

| you want to | read |
|---|---|
| run it yourself | [Building](#building) below, then the [Quickstart](./docs/11-quickstart.md) and [Choosing a model](./docs/12-choosing-a-model.md) |
| let your IDE's agent use it | [MCP and Kiro](./docs/20-mcp-and-kiro.md) |
| decide whether to trust it | [The evidence](./docs/02-the-evidence.md), [the measurements](./docs/30-benchmarks.md), then the [FAQ](./FAQ.md) |
| change it | [Building](./docs/10-building.md), [DECISIONS.md](./DECISIONS.md), and [DOCKET.md](./DOCKET.md) for what is open |

Everything a user needs is in [the book](./docs/README.md). The files at the repository root
are the design record, listed under [The design record](#the-design-record): they say why each
choice was made and what would overturn it, and a user never has to read them.

## What it is for

Driving **local** models to do real filesystem work: create, modify, move, summarise and update
files, either directly or when called as a tool by a larger model.

The design follows an empirical finding rather than a preference. Local-model tournaments run on
`kitchen-desktop` concluded:

> Use an external supervisor that checks repository state and reinvokes the model with one
> concrete remaining failure instead of relying on a single long autonomous session.
>
> Break larger work into fresh sessions. The isolated-session results strongly support this
> architecture.

So this is **a supervisor, not a chatbot**. Its job is keeping a 3–12B model on rails: bounded
sessions, verified state between turns, and guardrails on anything destructive.

## What this looks like in use

Two front doors, and that is a decision ([D7](./DECISIONS.md)): a person drives it directly, or
a larger model calls it as a tool.

**A person, one machine, local by default.** John has a folder of meeting notes and wants order
out of chaos:

```
hermit session --root ~/notes --prompt "Find every note that mentions
'Project Falcon' and create falcon-index.md listing each one with its date."
```

Hermit sends the prompt to a small model on John's own GPU (`qwen3.5:9b` through Ollama —
nothing leaves the machine), **with the tool menu attached**: `find`, `grep`, `read`, `write`,
each with a description. That menu is the whole trick — there is no magic by which a model
"knows" tools exist; the menu travels with every request, and models are trained to answer with
"call tool X" when a task needs hands. The model calls `grep`, reads the matches, writes the
index — and every call runs inside `~/notes` only, every write is read back byte-for-byte, and
anything overwritten is backed up first to an archive the model cannot touch. When the model
says "done," Hermit does not believe it: it inspects the folder. Index present and verified?
Done. Model claimed done on an untouched tree — a thing the 259 runs actually recorded? Re-run,
with the one concrete failure: *falcon-index.md does not exist.*

**An assistant that delegates.** John works in an agentic IDE (Kiro is the named first caller —
[ROUTING.md](./ROUTING.md) §8; any MCP client fits the same socket). Hermit is registered
there as an MCP server over stdio, so the IDE's cloud model sees the same tool menu at the
assistant level. John types *"clean up the reports folder — everything from 2025 goes into an
archive subfolder."* The big model does the thinking and calls Hermit for the hands: `find`,
then `move` per file, each hash-verified at both ends, none able to overwrite what already
exists. Ask for a file outside `~/reports` and the request is refused before any tool runs —
not a rule the model follows, the only door it has. The pitch is sharper than "another MCP
server", and it is the ecosystem's own warning inverted: a typical stdio MCP server runs with
*all* the launching user's authority; this is the one that **reduces** authority instead of
inheriting it.

**Status, honestly** (2026-09-04): the whole loop is built and tested. `hermit agent` takes one
instruction, offers the eight structural tools to a local model, dispatches what it asks for, and
stops on a turn or wall-clock bound. After every turn it snapshots the tree and prints a
hash-verified changeset that owes nothing to the reply. A stated post-condition (`--expect
exists:falcon-index.md`) is judged against the tree, and an unmet one re-invokes the model — up to
three attempts by default, each a fresh session handed the one concrete remaining failure. Two
tools are opt-in: `shell`, kernel-confined by Landlock and refused rather than run unconfined, and
`delete`, gated on an observation this session and backed up before the name goes. The MCP
frontend — the same tool surface over stdio — is built and shipped. The dated record of how each
piece landed lives in [ROADMAP.md](./ROADMAP.md); [ROUTING.md](./ROUTING.md) §12 is the honest
odometer.

One limit remains, deliberate and visible on screen. **Structure is not meaning:** four
predicates — exists, absent, bytes-preserved-across-a-move, identical — cover 329 of the 413
failures recorded in `bench/fsops`, and the residue is named rather than absorbed. `report.md`
containing the literal text `grep -oP '(?<=^).*' notes.txt` satisfies `exists:report.md` perfectly,
and that is a real recorded run.

## Why native code, honestly

Not for inference speed — the process is blocked on the model, and always will be. The wins are
structural:

| | |
|---|---|
| **Startup** | Bounded sessions mean *many* process launches. A Python agent pays 1–3 s of interpreter and imports every time; this binary starts in 0.8 ms median (50 launches of `hermit config`, Kitchen, 2026-09-25). Under this architecture the difference compounds per session. |
| **Verification** | Checking what the model actually did — walking trees, hashing, diffing — happens every turn. That is real work, and native code is good at it. |
| **Distribution** | One binary, versus a Python environment plus Node plus system dependencies. |

The evidence sits in three places:

- **[`bench/fsops/`](./bench/fsops/) in this repo** — the first 259 runs, on the dev laptop
  (RTX 5080 16 GB) through Hermes Agent. This is what REQUIREMENTS.md is built on.
- **hermit-bench** — the benchmark repository: the later sweeps, the pre-registered delta
  experiments (E1, E3, E4, E5), and every run behind the numbers this README quotes. **Not
  released yet**, and named without a link for that reason. The remaining model results, a
  hardware pass, and a full replication run on machines that are not ours are still outstanding;
  until those land, nobody could take the suite and reproduce these numbers, and a benchmark
  that cannot be rerun is something you would have to take on trust. It gets linked here when
  it can be rerun.
- The earlier OpenCode tournaments and the 144 Phase 0 diagnostic runs on `kitchen-desktop`
  (W7900) live in a private repository; what transferred from them is recorded in
  [ROADMAP.md](./ROADMAP.md) Phase 0.

## Layout

| Path | What |
|---|---|
| `src/hermit/core/` | Library code. `sandbox` (R1), `tool` (D4's interface), `fsio` (the one open primitive), `sha256` (R3's hash), `observed` (the staleness guard), `backup` (R4's archive) |
| `src/hermit/core/tools/` | The eight structural Tier 0 tools — read, list, find, grep, hash, write, edit, move — [ROUTING.md](./ROUTING.md) §4's surface, verified per call, plus two registered only on request: `delete` — one file the model has already read or listed, its bytes preserved before the name goes ([D19](./DECISIONS.md), `--delete`) — and `shell`, kernel-confined by [D10](./DECISIONS.md) (`--shell`) |
| `src/hermit/ollama/` | The only layer that speaks HTTP: client and R9 preflight |
| `src/hermit/supervisor/` | The turn: `loop` (dispatch and the R8 bounds), `session` (history and the context budget), `compact` (D17: rebuilds the window from the tree once it fills, rather than trimming), `wire` (the JSON bridge between `core`'s pure data and the model), `verify` (R6's per-turn hash diff of the tree — `force_rehash` whenever `shell` is registered, closing [D13](./DECISIONS.md)'s gap), `judge` (what the stated post-conditions came to), `semantic` (D15: judges `satisfies:` expectations — a fresh model session reading the tree, never the transcript), `reinvoke` (R7: re-invokes with the one concrete remaining failure, fresh session each attempt), `undo` (R4's other half: enumerate, restore and retention over the backup store) — [D7](./DECISIONS.md)'s middle layer |
| `src/hermit/app/` | `config`, `toolset` (composing the tools), `expect` (post-conditions, parsed against a root) and `mcp` (the JSON-RPC frontend), shared by the CLI and MCP surfaces |
| `src/main.cpp` | `hermit` — manual harness for the pieces that exist |
| `tests/` | GoogleTest suite; run with `ctest` |
| `DECISIONS.md` | The hard-to-reverse choices, and what would overturn each |
| `ROADMAP.md` | Scope, phases, and what has to be settled before code |
| `parity.tsv` | Which upstream subsystems are in scope, and where each stands |
| `tools/parity` | Reports drift against upstream for in-scope subsystems |
| `UPSTREAM-PARITY.md` | How the reference tracking works and why it is tag-granular |

## Building

Requires Linux, a C++23 compiler (verified on GCC 16.2.1 and clang 22.1.8), CMake 3.25+, and
network access on first configure: dependencies are fetched at pinned versions rather than
taken from the system ([D3](./DECISIONS.md)).

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

That produces `build/hermit`. There is no release or package yet ([DOCKET 1.4](./DOCKET.md)).
The rest lives in the book: the sanitizer and link-time-optimized builds in
[Building](./docs/10-building.md), every subcommand and flag in the
[CLI reference](./docs/13-cli-reference.md), settings and their precedence in
[Configuration](./docs/16-configuration.md), and the opt-in `shell` and `delete` tools in
[Shell and Landlock](./docs/15-shell-and-landlock.md) and
[Undo and backups](./docs/17-undo-and-backups.md).

Maintainers track upstream Hermes Agent with `tools/parity`; the workflow is in
[UPSTREAM-PARITY.md](./UPSTREAM-PARITY.md).

## The design record

| document | what it answers |
|---|---|
| [docs/](./docs/README.md) | **the book**: how to use Hermit — building, quickstart, choosing a model, the CLI, expectations, troubleshooting |
| [REQUIREMENTS.md](./REQUIREMENTS.md) | what this must do, each requirement traced to a measured failure |
| [SCOPE.md](./SCOPE.md) | what gets built, read, or ignored — and why |
| [ROADMAP.md](./ROADMAP.md) | sequencing, and what must be settled before code |
| [ROUTING.md](./ROUTING.md) | the tool surface: three tiers, the eight structural tools plus the opt-in `delete` and `shell`, who may call what |
| [FLOW.md](./FLOW.md) | the same thing drawn: request path, one mutating call, the supervisor turn |
| [FAQ.md](./FAQ.md) | the questions an evaluator asks first — shell, Python, "why not wait for better models" — each with what it concedes |
| [DECISIONS.md](./DECISIONS.md) | the hard-to-reverse choices, and what would overturn each |
| [parity.tsv](./parity.tsv) | machine-readable scope ledger; `tools/parity` reports drift |
| [bench/fsops/](./bench/fsops/) | the evidence: 259 runs of local models doing filesystem work |

**Scope in one line:** upstream is ~870k lines of non-test Python, and ~38k of it is in scope.
The part that matters most — verification, backup, retry — has no upstream equivalent that these
runs revealed. That last claim is inferred from behaviour, not from reading upstream's code; see
[REQUIREMENTS.md](./REQUIREMENTS.md).

Built in **C++** around **local inference** — Ollama today, vLLM from [D9](./DECISIONS.md).
Inspired by [NousResearch Hermes Agent](https://github.com/nousresearch/hermes-agent) (Python),
but **not a port of it**. Upstream is a reference for behaviour worth having, not a target to
match. Most of it — the messaging gateway, the plugin surface, the fifty cloud providers — is
deliberately out of scope.

## Where this lives

Gitea (`git@gitea:Jeagermeister/Hermit.git`) is authoritative, mirrored to GitHub — see the
`gitea-selfhost` repo. The reference clone deliberately does **not** live there: it is a pure
function of upstream, so backing it up would be storing something freely regenerable.

## License

**MIT** — see [LICENSE](./LICENSE).

Upstream Hermes Agent is also MIT (Copyright (c) 2025 Nous Research), so the licenses are
compatible. Since this is a reimplementation rather than a port, most code here is original
work — but MIT requires the copyright notice to travel with *substantial portions*, so anything
closely transliterated should carry an upstream attribution header.
