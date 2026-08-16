# Hermes-Cpp

## Start here

| document | what it answers |
|---|---|
| [REQUIREMENTS.md](./REQUIREMENTS.md) | what this must do, each requirement traced to a measured failure |
| [SCOPE.md](./SCOPE.md) | what gets built, read, or ignored — and why |
| [ROADMAP.md](./ROADMAP.md) | sequencing, and what must be settled before code |
| [ROUTING.md](./ROUTING.md) | the tool surface: three tiers, the eight tools, who may call what |
| [DECISIONS.md](./DECISIONS.md) | the hard-to-reverse choices, and what would overturn each |
| [parity.tsv](./parity.tsv) | machine-readable scope ledger; `tools/parity` reports drift |
| [bench/fsops/](./bench/fsops/) | the evidence: 259 runs of local models doing filesystem work |

**Scope in one line:** upstream is ~870k lines of non-test Python; ~38k of it is in scope, and
the part that matters most — verification, backup, retry — has no upstream equivalent that these
runs revealed. That last claim is inferred from behaviour, not from reading upstream's code; see
[REQUIREMENTS.md](./REQUIREMENTS.md).


A fast, local-first agent runner in **C/C++**, built around **local inference** (Ollama today, vLLM from [D9](./DECISIONS.md)).

Inspired by [NousResearch Hermes Agent](https://github.com/nousresearch/hermes-agent) (Python),
but **not a port of it**. Upstream is a reference for behaviour worth having, not a target to
match. Most of it — the messaging gateway, the plugin surface, the fifty cloud providers — is
deliberately out of scope.

## What it is for

Driving **local** models to do real filesystem work: create, modify, move, summarise and update
files, either directly or when called as a tool by a larger model.

The design follows an empirical finding rather than a preference. Local-model tournaments run on
`kitchen-desktop` (the `local-agent-benchmarks` repo) concluded:

> Use an external supervisor that checks repository state and reinvokes the model with one
> concrete remaining failure instead of relying on a single long autonomous session.
>
> Break larger work into fresh sessions. The isolated-session results strongly support this
> architecture.

So this is **a supervisor, not a chatbot**. Its job is keeping a 9–12B model on rails: bounded
sessions, verified state between turns, and guardrails on anything destructive.

## What this looks like in use

Two front doors, and that is a decision ([D7](./DECISIONS.md)): a person drives it directly, or
a larger model calls it as a tool.

**A person, one machine, no cloud.** John has a folder of meeting notes and wants order out of
chaos:

```
hermes-cpp session --root ~/notes --prompt "Find every note that mentions
'Project Falcon' and create falcon-index.md listing each one with its date."
```

Hermes sends the prompt to a small model on John's own GPU (`qwen3.5:9b` through Ollama —
nothing leaves the machine), **with the tool menu attached**: `find`, `grep`, `read`, `write`,
each with a description. That menu is the whole trick — there is no magic by which a model
"knows" tools exist; the menu travels with every request, and models are trained to answer with
"call tool X" when a task needs hands. The model calls `grep`, reads the matches, writes the
index — and every call runs inside `~/notes` only, every write is read back byte-for-byte, and
anything overwritten is backed up first to an archive the model cannot touch. When the model
says "done," Hermes does not believe it: it inspects the folder. Index present and verified?
Done. Model claimed done on an untouched tree — a thing the 259 runs actually recorded? Re-run,
with the one concrete failure: *falcon-index.md does not exist.*

**An assistant that delegates.** John works in an agentic IDE (Kiro is the named first caller —
[ROUTING.md](./ROUTING.md) §8; any MCP client fits the same socket). Hermes-Cpp is registered
there as an MCP server over stdio, so the IDE's cloud model sees the same tool menu at the
assistant level. John types *"clean up the reports folder — everything from 2025 goes into an
archive subfolder."* The big model does the thinking and calls Hermes for the hands: `find`,
then `move` per file, each hash-verified at both ends, none able to overwrite what already
exists. Ask for a file outside `~/reports` and the request is refused before any tool runs —
not a rule the model follows, the only door it has. The pitch is sharper than "another MCP
server", and it is the ecosystem's own warning inverted: a typical stdio MCP server runs with
*all* the launching user's authority; this is the one that **reduces** authority instead of
inheriting it.

**Status, honestly** (2026-08-16): the second half of these stories is built — the sandbox, all
eight Tier 0 tools with per-call verification, the staleness guard, and the backup store are
merged and tested. The first half — the loop that drives the local model, and the MCP server —
is [ROUTING.md](./ROUTING.md) §12 steps 4–5, the next work. Until then the stories describe the
destination, and §12 is the honest odometer.

## Why native code, honestly

Not for inference speed — the process is blocked on the model, and always will be. The wins are
structural:

| | |
|---|---|
| **Startup** | Bounded sessions mean *many* process launches. Python pays 1–3 s of interpreter and imports every time; a static binary pays ~10 ms. Under this architecture the advantage compounds per session. |
| **Verification** | Checking what the model actually did — walking trees, hashing, diffing — happens every turn. That is real work, and native code is good at it. |
| **Distribution** | One binary, versus a Python environment plus Node plus system dependencies. |

The evidence sits in two places, across two machines and two agent harnesses:

- **[`bench/fsops/`](./bench/fsops/) in this repo** — 259 runs on the MSI laptop
  (RTX 5080 16 GB) through Hermes Agent. This is what REQUIREMENTS.md is built on.
- **[`local-agent-benchmarks`](https://gitea-ec2.tail328f9a.ts.net/Jeagermeister/local-agent-benchmarks)**
  — five harnesses on `kitchen-desktop` (W7900): the earlier tournaments through OpenCode, and
  the 144 Phase 0 diagnostic runs through Hermes.

## Layout

| Path | What |
|---|---|
| `src/hermes/core/` | Library code. `sandbox` (R1), `tool` (D4's interface), `fsio` (the one open primitive), `sha256` (R3's hash), `observed` (the staleness guard), `backup` (R4's archive) |
| `src/hermes/core/tools/` | The eight Tier 0 tools — read, list, find, grep, hash, write, edit, move — [ROUTING.md](./ROUTING.md) §4's surface, verified per call |
| `src/hermes/ollama/` | The only layer that speaks HTTP: client and R9 preflight |
| `src/hermes/supervisor/` | Bounded sessions and the context budget ([D7](./DECISIONS.md)'s middle layer) |
| `src/hermes/app/` | Configuration, shared by the CLI and the coming MCP frontend |
| `src/main.cpp` | `hermes-cpp` — manual harness for the pieces that exist |
| `tests/` | GoogleTest suite; run with `ctest` |
| `DECISIONS.md` | The hard-to-reverse choices, and what would overturn each |
| `ROADMAP.md` | Scope, phases, and what has to be settled before code |
| `parity.tsv` | Which upstream subsystems are in scope, and where each stands |
| `tools/parity` | Reports drift against upstream for in-scope subsystems |
| `UPSTREAM-PARITY.md` | How the reference tracking works and why it is tag-granular |

## Building

Requires a C++23 compiler, CMake 3.25+, and network access on first configure (dependencies are
fetched at pinned versions rather than taken from the system — see [D3](./DECISIONS.md)). Verified
on GCC 16.2.1 and clang 22.1.8.

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

With sanitizers:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DHERMES_SANITIZE=ON
cmake --build build-asan && ./build-asan/tests/hermes_tests
```

The binary is a manual harness for the pieces that exist, not the product's CLI:

```bash
hermes-cpp resolve   --root DIR <path>...   # R1 path resolution
hermes-cpp preflight --model NAME           # R9 model gates, against a live daemon
hermes-cpp session   --model NAME           # context accounting, against a live model
hermes-cpp config                           # every setting in force, and where it came from
```

`resolve` shows how paths land against a sandbox root from a *different* working directory —
the R1 failure made visible:

```bash
cd /tmp && "$OLDPWD/build/hermes-cpp" resolve --root ~/some/root note.txt ../../etc/passwd
```

`session` exists because the token estimate is the one thing no unit test can settle — there is
no tokenizer in the process, so only the daemon can say whether the guess is conservative
enough. Each turn prints what the session expected against what Ollama actually evaluated.
Run it with a small window to watch the session compact history *deliberately*, which is the
whole point: left to itself the server discards the middle of an over-long prompt, keeps the
system message, and says nothing.

```bash
hermes-cpp session --model gemma31-agent --max-num-ctx 2048
```

Settings come from four places, in increasing precedence: **defaults < `--config` file <
environment < flags**. There is deliberately no default sandbox root and no implicit search for
a config file — either one would be an inferred root, which is R1's original bug. `hermes-cpp
config` prints the resolved set with the origin of each value:

```bash
hermes-cpp config --config ./hermes.json --model qwen35-agent
```

## Working with upstream

The Python reference lives at `~/Source/hermes-upstream` (blobless, `main` only, 261 MB). It is
not part of this repo and is disposable — it re-clones from GitHub in ~11 s.

```bash
hermes-upstream-sync      # refresh the reference, print what changed
tools/parity              # what has drifted in the modules we care about
tools/parity conversation_loop   # the specific commits behind one module
```

Set `$HERMES_REF_REPO` if the reference lives elsewhere.

## Where this lives

Gitea (`git@gitea:Jeagermeister/Hermes-Cpp.git`) is authoritative, mirrored to GitHub — see the
`gitea-selfhost` repo. The reference clone deliberately does **not** live there: it is a pure
function of upstream, so backing it up would be storing something freely regenerable.

## License

**MIT** — see [LICENSE](./LICENSE).

Upstream Hermes Agent is also MIT (Copyright (c) 2025 Nous Research), so the licenses are
compatible. Since this is a reimplementation rather than a port, most code here is original
work — but MIT requires the copyright notice to travel with *substantial portions*, so anything
closely transliterated should carry an upstream attribution header.
