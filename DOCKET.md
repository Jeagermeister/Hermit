# Docket — candidates from the 2026-09-04 design session

**Status: draft, under review. Nothing here is scheduled by being listed.** This file is the
working list that came out of one design session, written down so it can be argued with.
Items graduate from here into [ROADMAP.md](./ROADMAP.md)'s phase list or hermit-bench's
`TODO.md`, or get struck with a one-line reason; either way the entry stays so the reasoning
is not lost.

**This file is a staging area, not a canonical index.** Nothing here is the binding version of
anything; the binding version lives where the evidence is. An item that has found a home should
be struck here, not maintained in two places. Like [TODO.md](./TODO.md), this file should be
deletable once every item has either graduated or been struck.

Every entry carries the same six fields, because the same six questions get asked of every
item in this repository:

- **Why** — the measured failure, the open question, or the stated goal that motivates it.
  An item with no *why* is a preference, and this repository does not build on preferences.
- **Shape** — what it would look like built, in one paragraph. Not a design; a target for one.
- **Done when** — the observable that closes it. Prefer a number.
- **Struck if** — what would kill the item, so it is not re-opened without new information.
- **Size** — small (one session), medium (two to three), large (needs its own plan).
- **Needs** — none, 9B tier, 30B tier, or root (for loop mounts). The fact that matters is the
  model tier, not the hostname.

Part 1 is the new material. Part 2 records what was considered and set aside. Part 3 is a
proposed order.

---

## 1 · New — proposed here for the first time

### 1.1 A structured run trace

**Why.** A run's only durable record today is the trace lines it printed and the token rows in
`.hermit-usage-<root>/usage.jsonl`. hermit-bench reconstructs what happened from stdout;
the D18 judge-usage gap exists because the judge's call is not in the one observer that
writes anything down; E2 needs metered tokens per completed task and would have to scrape
them. Three docketed items are each partly "write the thing down", and none of them has a
place to write it.

**Shape.** One JSONL file per run, opt-in by flag and path (`--trace FILE`, or a `trace`
config block), one record per event: run start (config as rendered, model digest, root
identity), each turn (prompt bytes, `eval_count`, `prompt_eval_count`, wall clock), each tool
call (name, args as parsed, verdict, the hashes it verified), each verification pass (entries
walked, bytes hashed, changeset), each judge call (model, tokens, verdict per expectation),
the exit (reason, attempt number). The judge's call goes through the same writer, which
closes the D18 gap as a side effect. `usage.jsonl` stays as it is — a different consumer, a
different retention rule — but is fed from the same events. Nothing in the trace is read back
by the loop: it is a record, not state, so D13's "observe the tree, never the transcript"
is untouched.

**Done when.** hermit-bench's `run_delta.py` can score a run from its trace alone without
parsing stdout; `hermit usage` reports judge tokens; the E2 driver reads metered tokens from
the trace. A `hermit explain TRACE` that renders one run as a readable timeline is a natural
second step, not part of this item.

**Struck if.** The judge-usage gap is closed by a narrower fix and no consumer needs the full
event stream — but E2 metering and the hermit-bench parser both still land on it, so unlikely.

**Size.** medium. **Needs.** none.

**What it does not do.** No log levels, no logging framework, no stderr chatter changes. The
stdout trace lines stay exactly as documented in `docs/11-quickstart.md`.

### 1.2 Template probes inside preflight

**Why.** ROADMAP § *Open questions* records two chat-template properties no model card
reports and `ollama show` does not summarise: whether tool definitions survive a tool result,
and whether the caller's system prompt survives offering tools. Both were found by token
counting first and explained by reading templates second ([DECISIONS.md](./DECISIONS.md)
"Still open"): `llama3.2-3b`'s tools cost drops from +133 to +31 when a tool result is last,
and `hermes3-8b`'s system prompt goes from 39 tokens to 234 while the model loses the
sentinel. R9's preflight still checks only the two card-readable gates (context length, the
`tools` capability). A model that fails either template probe is admitted today.

**Shape.** `hermit preflight` (and the same check at run start, cached) sends two short probe
conversations at temperature 0 and reads the token differential, not the model's behaviour:
(1) the same request with and without a tool result last, comparing `prompt_eval_count` — a
model whose tools cost collapses has lost its definitions; (2) a system prompt carrying a
sentinel instruction, with and without tools offered — a model whose prompt cost jumps while
the sentinel disappears has had its system prompt replaced. The differential is self-calibrated
within one model (the absolute numbers are setup-dependent, per DECISIONS.md), so there are no
magic thresholds. It is deterministic because tokenization does not sample. Verdicts are cached
under the usage directory keyed by the model's digest, so a model is probed once per pull, not
once per run. A failed probe is a refusal with the mechanism named, the same way an unconfined
shell is a refusal today — the D10 precedent holds because the check is deterministic.
`--allow-unprobed` exists for research runs and is marked ⚠ in `hermit config` like the other
overrides.

**Done when.** The two known cases (`llama3.x` stock templates fail probe 1; `hermes3-8b`
fails probe 2) are refused by the binary with the right reason, and hermit-bench's launch
gate is one call to `hermit preflight --json`.

**Struck if.** The token differential proves unstable across pulls (it should not — tokenization
does not sample), or model selection settles on a fixed roster that excludes the failing
templates.

**Size.** medium. **Needs.** 9B tier (the failing models are small).

**Open.** Probe 1 needs a tool the model must call twice; `hash` on two paths is the obvious
candidate and it is read-only. Whether a probe result should also be written into the trace
(1.1) is a yes if 1.1 lands first.

### 1.3 Verifier at scale — measured first, optimised second

**Why.** D13's own "what would overturn this" clause: the per-turn walk is one `stat` per
entry, "and nothing has yet run it against a large repository." `TreeVerifier` exposes
`last_entries_walked()` and `last_hashed_bytes()` precisely so this stays a measurement. The
only trees it has been measured on are test fixtures and the benchmark's small repos. The
README's scope line names an ~870k-line upstream; a user pointing Hermit at a tree that size
would be the first to find out.

**Shape.** A measurement, then a decision. Point the verifier at a tree on the order of
10⁵ entries (a Linux kernel checkout is the conventional one), record cold and warm walk
times and both counters, repeat with one file touched, and publish the numbers in D13 the
way D16 published LTO's. Only then, and only if the walk is material against a 6–17 s model
turn, consider: `statx` with a reduced mask, walk order that matches directory layout,
`getdents64` batching, or the design question this will probably surface — whether `.git/`
belongs inside the verified tree at all, given that the model can reach it through `write`
and every object it contains is immutable by construction.

**Done when.** D13 carries a table with the numbers and a sentence saying whether the cost is
material. If it is, a follow-up entry names the one optimisation the numbers justify.

**Struck if.** The walk proves immaterial at 10⁵ entries — then no optimisation item exists,
which is the point of measuring first.

**Size.** small for the measurement. **Needs.** none.

### 1.4 Release engineering

**Why.** The stated goal is outside users. The repository has no tag that is not a backup
tag, no release, no checksums, and no install path other than "build it". Someone who reads
the README and wants to try it has to build a C++23 project with vendored sanitizer
discipline, which is a filter for contributors, not users. Every large project this is meant
to compete with is one command to install.

**Shape.** A `v0.1.0` tag on a commit the benchmark numbers were collected against, or the
first commit after with a clean suite. A release build recipe that is reproducible on a
fresh CachyOS/Arch machine: static where the FAQ already claims static (its ~10 ms launch
figure), `HERMIT_LTO` on, sanitizers off, a checksum file signed with the operator's key. A
PKGBUILD for Arch as the first packaging target, since that is the only platform in scope.
A one-page `docs/10-building.md` addition: "install a release" above "build from source".
Versioning rule written once: the tag is the only version, and the binary prints it.

**Done when.** `hermit --version` on a fresh machine prints a tag, and the README's quickstart
starts with an install rather than a build.

**Struck if.** The outside-user goal is dropped.

**Size.** small for the first tag and checksum; medium with the PKGBUILD. **Needs.** none.

**Open.** Whether a release needs a decision entry for the versioning rule. Probably one
paragraph in D16's neighbourhood, not a new D. The tag must land on a commit the benchmark
numbers were collected against — see 1.13, which is why the commit map comes first.

### 1.5 Contributor surface

**Why.** No `CONTRIBUTING.md`, no `SECURITY.md`, no issue template, no `.gitea/` or
`.github/` directory at all. The first outside report is the most valuable thing the project
can receive, and today there is no instruction for what to include in it. For a project whose
claims are "nothing escaped, nothing was destroyed", the security-report path matters more
than usual.

**Shape.** `CONTRIBUTING.md` that says three things: how a run is reported (root layout,
model tag and digest, config as rendered, the trace from 1.1 if it exists), what a change
needs before review (the ASan/UBSan build, the suite, a decision entry if it is hard to
reverse), and that attribution trailers are not accepted. `SECURITY.md` with a private
report path and the statement of what is in the threat model (from D10's "Still open"
hardlink entry) so reporters know what counts. One bug template asking for the run report.

**Done when.** The three files exist and README's *Start here* table points at
`CONTRIBUTING.md`.

**Struck if.** The repository stays private — then there are no outside reports to receive.

**Size.** small. **Needs.** none.

### 1.6 Fuzzing the code that holds the guarantees

**Why.** Every vendored dependency ships fuzzers (`nlohmann_json`, `httplib`); Hermit has
none of its own. The README leads with containment. The sandbox resolver (`Sandbox::resolve`,
R1, D6's final-component rule), the `edit` argument parser, the config parser and the wire
decoder for tool calls are all parsers of hostile input in the strict sense — a model, or a
file the model wrote, chooses their bytes. The suite proves the cases someone thought of.

**Shape.** libFuzzer targets under `tests/fuzz/`, built only when `HERMIT_FUZZ` is set, run
under ASan/UBSan: (1) `resolve()` against a fixture root seeded with symlinks in every
direction, asserting the returned path is always inside the root or refused; (2) the `edit`
parser and applier, asserting the hash-verified read-back invariant holds or the call refuses;
(3) `apply_json` for config; (4) the tool-call decoder in `supervisor/wire.cpp`. A corpus
directory checked in, a five-minute run in CI when 1.9 exists.

**Done when.** Each target runs an hour without a finding, and the corpus is committed. Any
finding becomes a test first, in the red/green order the suite already follows.

**Struck if.** The parsers are rewritten such that the fuzz targets no longer match the code
— unlikely, since these are the stable boundaries.

**Size.** medium. **Needs.** none.

### 1.7 A substrate matrix

**Why.** D11 says the substrate is probed, not assumed, and nothing probes. Before a probe can
be written, someone has to know what actually differs. The tuple `dev:ino:size:mtime:ctime`
is relied on for staleness, for `edit`'s fail-closed check, and for the verifier's carry-
forward hash — and its behaviour on btrfs, xfs, tmpfs and an overlay is documented nowhere in
this repository.

**Shape.** Loop-mounted images of ext4, xfs and btrfs, plus tmpfs and this laptop's native
btrfs, each hosting a root; the full suite run against each; a short table recording which
guarantees held and which tests, if any, changed outcome. The result is the input D11's probe
needs: the set of checks that can distinguish the substrates that matter. Nothing in the
product changes from this item alone.

**Done when.** D11 carries the table and names the probe it justifies.

**Struck if.** D11's probe is written without the matrix — but the probe needs the matrix's
table to know what to check, so this is the input, not a rival.

**Size.** small. **Needs.** root (for loop mounts).

### 1.8 Tier 1 tools — `triage` and `summarize`

**Why.** ROUTING §4 designs two Tier 1 tools and neither exists. Both are latency-tolerant,
token-expensive and reliability-forgiving, which is the profile local inference wins on.

**Shape.** As designed: N paths and a question in, a ranked subset with one line of reason
each (`triage`) or one summary per file (`summarize`). Both read-only, both run by a second
model call, both labelled as judgment the way D15 labels the judge.

**Trigger.** A named caller wants one. Until then this stays here as a pointer, deliberately
unscheduled — the same rule that kept `delete` unbuilt until the benchmark asked for it.

**Done when.** A named caller has run `triage` or `summarize` on a real task and the ranked
subset or summary is recorded in a result file.

**Struck if.** No caller ever wants one — which is the current state, and why it is unscheduled.

**Size.** medium. **Needs.** 9B tier.

### 1.9 Continuous integration on the self-hosted runner

**Why.** Neither repository runs anything on push. hermit-bench already dockets CI for its
self-tests; Hermit's suite is 821 tests that only run when someone remembers. The laptop now
has Docker installed, which unblocks the `act_runner` chore in the infrastructure repository
that this depends on.

**Shape.** A Gitea Actions workflow that builds under ASan/UBSan with both compilers, runs
`ctest`, runs `tools/parity`, and greps the tree for attribution trailers as a
belt-and-braces check on the hook. hermit-bench gets `fsops/selftest.py` and
`run_delta.py --selfcheck`. No model in CI; a GPU runner on Kitchen is a later, separate item.

**Done when.** A PR shows a green check on Gitea before merge, on both repositories.

**Struck if.** The self-hosted runner never materialises.

**Size.** medium, mostly infrastructure. **Needs.** none (plus the runner host).

### 1.10 Multi-root parallelism

**Why.** ROUTING §10 has the design: disjoint roots, join on hash. Not motivated by any
measured failure yet; recorded so it is not re-designed.

**Shape.** Not now. When a workload arrives that needs N roots at once, the design is
already written and D1 (one blocking thread per process) is preserved by running N
processes.

**Trigger.** A workload arrives that needs N roots at once.

**Done when.** N processes on disjoint roots complete a real workload and the join-on-hash
produces a verified result.

**Struck if.** No workload needs N roots — the current state, and why it is unscheduled.

**Size.** large. **Needs.** 30B tier.

### 1.11 Ollama Cloud and pixel-level provenance

**Why.** D18 admits Cloud narrowly. Local inference is unwatermarked by construction; Cloud
output is unverified, and there is no public detector to check with. The operator's notes
record a regulatory date of 2026-12-02 to confirm before relying on it.

**Shape.** A documentation item, not code: one paragraph in `docs/31` stating what is and is
not known about provenance marking on Cloud output, in the same "no readable metadata" versus
"unmarked" discipline the logo notes already use. Revisit if a detector or a vendor statement
appears.

**Done when.** The paragraph exists and is dated. **Struck if.** A detector or vendor
statement appears — then the paragraph writes itself.

**Size.** small. **Needs.** none.

### 1.12 Housekeeping, found while surveying

- **FAQ.md's build claim is stale twice.** It says "819 tests at zero warnings"; main has 821
  (PR #52, still open, fixes the count), and on this laptop's GCC 16.2.1 build on 2026-09-04
  `confine_test.cpp` produced seven warnings. Either fix the warnings and keep the claim, or
  narrow the claim to the compiler it is true on. The count should stop being a literal in
  prose or get a test that pins it.
- **`FileState` carries no `is_regular`.** `supervisor/verify.h` records this as a known gap.
  Close it or leave it; decide, since it is the one gap the verifier names about itself.
- **hermit-bench still carries the owner-column username** in four tracked result files and
  ten commits, docketed on its PR #6. Same rewrite recipe as Hermit's, same tag-then-reset
  cost on every clone.
- **`tool.h` says the MCP renderer is "not yet built".** It is (`app/mcp.cpp`); the two
  comments at `tool.h:9` and `tool.h:55` should say where it lives instead.

### 1.13 The commit map from the history rewrite

**Why.** The 2026-09-04 history rewrite renumbered every commit. The benchmark results pin
the supervisor at hashes that no longer resolve: E3/E4/E5 pin `070da1e`, E1 pins `a5722dc`,
and neither is an ancestor of the rewritten `main`. hermit-bench's results files now cite
commits nobody can fetch, which breaks the project's central claim — that the numbers are
reproducible. The old→new mapping lives only in the operator's scratchpad and dies with this
session.

**Shape.** Two things. (1) Commit the mapping to a durable file in this repository, listing
old SHA → new SHA for every commit the benchmark results cite (`070da1e` → `043ba6f`,
`a5722dc` → `68cfc63`, and any others the results name). (2) An errata note in hermit-bench's
results files recording the mapped SHAs so a reader can resolve the pins. The mapping is the
only thing that makes the results reproducible at all.

**Done when.** Every SHA cited in hermit-bench's results resolves to a fetchable commit, and
the mapping is committed in this repository.

**Struck if.** The mapping is committed and the errata lands — then it is done.

**Size.** small. **Needs.** none.

---

## 2 · Considered and set aside

Recorded so the next session does not re-open them without new information.

- **Streaming responses.** D1 is one blocking thread; `"stream": false` is deliberate. The
  case for streaming is watching a model think or aborting a runaway; `num_predict` and R8's
  wall clock already bound the runaway, and watching is not a supervisor's job.
- **An interactive or REPL mode.** The bounded session is the product. A REPL puts the human
  back on the critical path the supervisor exists to take them off.
- **Windows.** Out of scope by the platform decision; Linux is the product. Landlock alone
  makes this a port, not a build flag.
- **Training a worker model.** `bench/distill/DESIGN.md` is gated on E1 being collected first
  so the supervision delta stays recoverable, and the corpus recorder records the wrong
  system. Nothing has changed on either count.
- **A smaller judge under a larger worker.** Real, but that is E2 territory and E2 is now
  runnable; measure before designing.

---

## 3 · A proposed order for laptop sessions

The order below optimises for the outside-user goal first — the release and the contributor
surface come before the measurement items — then for cheapest evidence first within the
measurement work. The done-at-bound exit (ROADMAP § *The measured levers*) is the top
user-facing item but is already docketed there, so it is not re-listed. This is a suggestion
to be argued with, not a plan.

| # | Item | Why here | Size |
|---|---|---|---|
| 1 | Commit map (1.13) | Correctness; the benchmark pins are unfetchable until it lands, and the map dies with this session | small |
| 2 | Release tag + checksum (1.4) | Serves the outside-user goal directly; no dependency; an hour's work | small |
| 3 | Contributor surface (1.5) | Serves the outside-user goal; small | small |
| 4 | Cloud provenance paragraph (1.11) | Small, dated, no dependency | small |
| 5 | Structured run trace (1.1) | Judge-usage logging, E2 metering and the hermit-bench parser all land on it | medium |
| 6 | Template probes in preflight (1.2) | Closes an open ROADMAP question in code; hermit-bench's gate becomes one call | medium |
| 7 | Verifier scale measurement (1.3) | Cheap, and it decides whether an optimisation item exists at all | small |
| 8 | CI on the self-hosted runner (1.9) | Everything above it becomes safer to merge | medium |
| 9 | Substrate matrix, then D11's probe (1.7) | The probe cannot be written before the matrix exists | small |
| 10 | Fuzzing (1.6) | Strengthens the claims the README leads with; benefits from CI existing | medium |

Unscheduled by design: Tier 1 tools (1.8) and multi-root (1.10) wait for a caller and a
workload respectively. Kitchen-gated items (retry quality gating, E6 calibration) live in
ROADMAP and hermit-bench, not here.
