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
fresh CachyOS/Arch machine: `HERMIT_LTO` on, sanitizers off, a checksum file signed with the operator's key. A
PKGBUILD for Arch as the first packaging target, since that is the only platform in scope.
A one-page `docs/10-building.md` addition: "install a release" above "build from source".
Versioning rule written once: the tag is the only version, and the binary prints it.

**Done when.** `hermit --version` on a fresh machine prints a tag, and the README's quickstart
starts with an install rather than a build.

**Struck if.** The outside-user goal is dropped.

**Size.** small for the first tag and checksum; medium with the PKGBUILD. **Needs.** none.

**Open.** Whether the release binary should be static. This entry first said "static where the
FAQ already claims static"; that claim was wrong (the binary is dynamically linked, and starts
in 0.8 ms median, measured 2026-09-25), so static linking is a choice to argue on its own
merits (distribution, not startup), not a promise to keep. Whether a release needs a decision
entry for the versioning rule. Probably one
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

- **FAQ.md's build claim is still stale on warnings.** The count half was fixed by PR #52 on
  2026-09-04 and `FAQ.md` now says 821. The "zero warnings" half is still false: this laptop's
  GCC 16.2.1 build produced seven warnings in `confine_test.cpp` that day. Either fix the
  warnings and keep the claim, or narrow it to the compiler it is true on. The count should
  stop being a literal in prose, or get a test that pins it.
- **`FileState` carries no `is_regular`.** `supervisor/verify.h` records this as a known gap.
  Close it or leave it; decide, since it is the one gap the verifier names about itself.
- **hermit-bench still carries the owner-column username** in four tracked result files and
  ten commits, docketed on its PR #6. Same rewrite recipe as Hermit's, same tag-then-reset
  cost on every clone.
- **`tool.h` says the MCP renderer is "not yet built".** It is (`app/mcp.cpp`); the two
  comments at `tool.h:9` and `tool.h:55` should say where it lives instead.

### 1.13 The commit map from the history rewrite

**Struck 2026-09-06 — landed.** The map is [docs/91-commit-map.md](./docs/91-commit-map.md):
all 168 pre-rewrite commits, verified by rewriting each old blob and comparing object hashes
(19,634 blobs compared, 1,120 differ, all explained, none unexplained), plus a separate message
comparison the tree check cannot do. The five ids the measurements cite are called out at the
top of it, and errata notes landed in this repo's `bench/delta/E1-RESULTS.md` and in
hermit-bench's four results documents and README.

Building it falsified this entry's own premise twice: the old objects are **not** gone from
every remote — see 1.14 — and 9 of the 168 ids never changed. Both are corrected in the map.

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

### 1.14 The 2026-09-04 rewrite did not reach the GitHub mirror

**Struck 2026-09-06 — fixed, the same day it was found, and not the way this entry proposed.**
The shape below assumed a GitHub Support request. It was unnecessary: the mirror is a
push-mirror of Gitea, holds nothing of its own, and had no forks, network members, issues,
pull requests or releases — so deleting and recreating it destroyed the object store outright
and cost one star. The Gitea mirror repopulated 53 branches immediately. Verified after:
the pre-rewrite ids return 404 over both the API and `raw.githubusercontent.com`, `main`
matches Gitea, and the rebuilt mirror carries no tags. Deletion should have been the first
option in this entry rather than the fallback.

**Why.** Found on 2026-09-06 while building the commit map. A force-push unreferences objects;
it does not delete them, and GitHub keeps serving unreachable commits by id indefinitely. The
pre-rewrite commits are still returned by GitHub's API, and a pre-rewrite blob still containing
the username was fetched over `raw.githubusercontent.com` at a force-pushed id. So the scrub
succeeded on Gitea and on every fresh clone, and failed on the one remote that is public.
`TODO.md`'s account of the rewrite reads as finished; it is not.

**Shape.** Ask GitHub Support to garbage-collect unreachable objects on the mirror, which is
the only supported way to drop them, and re-run the same fetch to confirm. If that is refused
or too slow, the alternative is deleting and recreating the mirror repository, which costs the
stars and forks it does not have. Then correct `TODO.md`, and `docs/91-commit-map.md`'s section
on where the old objects live.

**Done when.** Fetching a known pre-rewrite blob by id over `raw.githubusercontent.com` returns
404, checked for more than one id. *(Met: five ids checked, all 404.)*

**Struck if.** The mirror is taken down, or the exposure is accepted deliberately and the
documents say so instead of implying otherwise.

**Size.** small, but it is mostly waiting on someone else. **Needs.** none.

**Open, and now closed too.** Whether the full 168-row map should be public was a live
question while the ids were still fetchable, since the left-hand column indexed exactly what to
ask for. They are not fetchable any more, so the column is inert and the map stands as
published.

### 1.15 A scored discovery domain — the discovery cluster's gate

**Cluster of seven (1.15–1.21), designed together in
[bench/discovery/DESIGN.md](./bench/discovery/DESIGN.md), 2026-09-21.** The source idea is an
external preprint (Dream-RSI, arXiv:2609.14858v1) — a replay-simulator loop for improving a
discovery system's exploration policy offline. It is a design idea, not evidence: single
trajectories, no variance reported, unverified repo. The cluster's rule is that the idea dies
at the cheapest component that can kill it rather than the most expensive one.

**Why.** Every result in the source paper sits on a domain with a cheap scalar score. Hermit's
verdicts are met/unmet predicates; nothing in the loop — replay objective, policy selection,
validation — exists without a number to optimise. This is the gate the whole cluster hangs on.

**Shape.** One task: a program in a sandbox root that a model is asked to improve against a
*measured* number (script runtime on a pinned input, or a fixed scorer's objective value),
scored by an existing confined path writing `eval/score.json`. Structural predicates still
gate correctness; the optimisation target is scalar. Measured, not model-judged, for the
first domain — D15's judge is a model and its stochasticity weakens replay determinism, which
is the cheap property the design buys.

**Done when.** The task and scorer exist on one machine, and two known-different attempts
score measurably differently.

**Struck if.** No honest scalar domain exists at local model tier — then the cluster closes
here, which is why this is first.

**Size.** small-medium. **Needs.** 30B tier (to produce the two differing attempts).

**Status, 2026-09-22: passed as written, one gap named.** Domain: sum–difference Γ,
[bench/discovery/sumdiff/](./bench/discovery/sumdiff/README.md). The objective is exact and
computed outside the candidate's root, because a score file inside it can be rewritten by the
candidate (tested). Hand controls: 1.0 against 1.034421. One `qwen3.8:27b` attempt on Kitchen
searched its way to a set the official scorer confirms at 1.046752. It spent its 1,500 s
budget before writing that set into `solve.py`, so the delivered attempt scored 1.0. n = 1.
The gap, discoveries that never become deliverables, passes to 1.17. The record is
[RECORD.md](./bench/discovery/sumdiff/results/2026-09-22-gate/RECORD.md).

### 1.16 Attempt-tree recording for discovery runs

**Why.** A replay simulator reads recorded trees; nothing records them. A run's durable
record today is trace lines and `usage.jsonl`; docket 1.1's structured trace is the event
source this item consumes.

**Shape.** Each attempt becomes a node with a parent reference, a preservable workspace (the
backup store already does this), the proposal text, the scorer's diagnostics, and the score
from 1.15. Records hash-chained, so the replay pool is provably unmodified after the fact —
a meta-agent that can rewrite its own history is a failure mode this repository already has
requirements about.

**Done when.** A full discovery round is reconstructable from node records alone, without
parsing stdout.

**Struck if.** 1.1 lands in a shape that already carries tree structure — then this folds
into it.

**Size.** medium. **Needs.** none (depends on 1.1 and 1.15, not on a model).

### 1.17 Parallel refinement runner

**Why.** The source design runs W attempts in parallel; D1 is one blocking thread per
process. DOCKET 1.10 already wrote the answer — N processes on disjoint roots, join on hash —
and it has been waiting for a workload. This is that workload.

**Shape.** W branch workspaces under one job root, each disjoint and each driven by its own
process, with R8 wall-clock bounds per attempt and the join verified by hash. The policy
driving it is fixed parallel-refine — the paper's own Round-1 policy and, deliberately,
1.21's controlled baseline.

**Done when.** W workers × R refinements × one round completes inside its budget and yields a
fully recorded tree via 1.16.

**Struck if.** 1.10 is struck — but this *is* a workload needing N roots, so more likely this
item is what un-strikes 1.10 into a phase.

**Size.** large. **Needs.** 30B tier.

### 1.18 Exploration policy as data

**Why.** Today the exploration policy is compiled C++ ("retry ≤3 with one failure"). The whole
loop hangs on that becoming a consulted artifact. This is the seam; everything after it is
application of the seam.

**Shape.** A policy file read per decision round: which recorded frontiers to extend, batch
composition up to W, stop conditions. **Data before code** — the paper rewrites code, and
code is exactly where its leakage guardrails are weakest. A data schema is auditable and
refusable at the router, the way the tool menus already are. Code only if a data schema is
measured too narrow.

**Done when.** One discovery round runs identically under the fixed policy expressed as data,
and a hand-edited policy produces measurably different allocation.

**Struck if.** 1.15 is struck.

**Size.** medium. **Needs.** none.

### 1.19 Replay simulator over recorded trees

**Why.** The cheap half of the idea, and the half with zero model risk. It is a measurement
instrument; instruments are where this repository starts things.

**Shape.** Offline, model-free, deterministic reveal: a candidate policy navigates a frozen
recorded tree, is shown recorded children only, never generates, and is scored — best score
seen, minus cost per attempt, plus a parallelism term. Lives in bench tooling; promotion to
the product is a later parity.tsv conversation. Inherits the paper's bound by design: replay
can only re-navigate recorded branches, so it answers *which re-use of seen work pays best*,
never *what to try that was never tried*.

**Done when.** Two hand-written policies score differently over the same recorded tree and
the ordering survives inspection against the tree's actual contents.

**Struck if.** 1.16's records prove insufficient to reconstruct a navigable tree — a record
design failure, fixed there.

**Size.** medium. **Needs.** none.

### 1.20 The dreaming loop

**Why.** The RSI claim proper: a policy-development agent reads replay trajectories, revises
the policy (1.18's artifact), M revisions per round are scored by 1.19, and the best
redeploys online with the incumbent always in the pool. The paper's only guarantee — never
worse *on the replay objective* — is quoted with its scope; it says nothing about online
quality, which is 1.21's entire job.

**Shape.** A larger local model as the development agent, driven through D7's machine front
door or a bench-side script; its own tokens metered through the same 1.1 trace as everything
else, because the source paper never counted that cost and this repository has E2 docketed
precisely because it counts. Anti-leakage is *mechanical* where possible: the replay API
exposes prefix-observable state only — the unrepresentable-not-forbidden trick D6 plays with
paths — rather than the paper's prompt-level "never look at unrevealed cells".

**Done when.** One full loop runs unattended: online round → recorded tree → replay-scored
revisions → policy redeployed → next online round.

**Struck if.** 1.21 returns null; also struck if the development agent cannot produce legal
policies at 30B tier after honest effort — that is a capability measurement worth recording,
not a failure to hide.

**Size.** large. **Needs.** 30B tier.

### 1.21 The validation experiment — the exit gate

**Why.** The two questions the source paper did not answer: at matched discovery-agent
budgets, does a replay-improved policy beat fixed parallel-refine online, across enough
repeats to see through the variance Phase 0 measured — and does replay score predict online
outcome at all? The mechanism assumes the second; nobody has plotted it. This repository's
whole culture exists to run exactly this experiment.

**Shape.** Pre-registered, frozen protocol in hermit-bench's manner on 1.15's domain: paired
runs, fixed vs replay-improved, repeats sized against measured variance, plus the
replay-score/online-outcome correlation. Exit criteria written down before the first run.
Losses recorded beside wins, whichever way it lands.

**Done when.** The protocol is frozen and the result is recorded with a verdict either way.

**Struck if.** 1.15 is struck — no domain, no experiment.

**Size.** small for the protocol; medium including runs. **Needs.** 30B tier.

### 1.22 Lead with the safe hands — the product track's frame

**Cluster of nine (1.22–1.30), opened 2026-09-25 by operator direction: make Hermit a product
someone other than its author installs and keeps using.** The design documents argue *why*
at length. This track is about *getting there*: install, first run, the front door for
agents, speed, and a test of whether anyone wants it.

**Why.** The README pitches Hermit as a supervisor for small local models. Its guarantees are
reduced authority (one root, kernel-enforced for shell), hash-verified changes, undo, and
completion decided from the tree. None of them depends on the model being small. A frontier
agent calling Hermit over MCP gets the same guarantees, and FAQ.md's answer to "why not wait
for better models" already says why they stay useful: better models make fewer mistakes, not
more trustworthy reports. *This is argument: nobody outside this repository has used Hermit
that way yet, which 1.30 exists to find out.*

**Shape.** The README's first screen says what Hermit does for *any* agent: reduced
authority, a verified changeset, undo, a verdict from the tree. Small local models stay the
default and the evidence; the MCP front door moves up. Touches README claims, so it lands as a
proposed diff for operator sign-off, not by edit.

**Done when.** A README revision with this framing is merged, and it cites the existing
evidence at its real width without extending it.

**Struck if.** 1.30 finds no agent user wants reduced authority over their own IDE's tools.
Then the small-local-model framing stands, stated as the whole product.

**Size.** small (the diff); the argument is the work. **Needs.** none.

### 1.23 `hermit doctor` — one command that says what is wrong

**Why.** A first run can fail on Ollama being down, a model failing preflight, Landlock
unenforced, or a missing root, and each failure is reported by a different subcommand today.
Chapter 18 of the book is 139 lines of symptoms because nothing diagnoses them in one place.

**Shape.** Read-only. Checks, in order: daemon reachable and its version; D10's own
confinement probe; every installed model through R9 preflight, with the ones that pass
listed first; the resolved configuration and where each value came from. Prints one
suggested next command. Everything it checks already exists as a library call. This is
composition, not new mechanism.

**Done when.** On a machine with one fault planted at a time (daemon stopped, Landlock
disabled, only failing models installed), `doctor` names the fault and the fix each time.

**Struck if.** 1.4's install path makes first-run failure rare enough to not matter, which
would need an outside user to show it.

**Size.** small. **Needs.** 9B tier (one model passing preflight).

### 1.24 MCP setup that can be pasted

**Why.** For the frame in 1.22, the MCP registration *is* the install. Chapter 20 explains
Kiro's config by hand; other clients (Claude Code, OpenCode, Cursor) each want a slightly
different JSON shape, and a wrong absolute path fails silently inside the client.

**Shape.** A subcommand, name to be settled (`hermit mcp --print-config <client>` is the
candidate), that prints a ready block for the named client with this binary's absolute path
and the chosen `--root`. Refuses a relative root for the same reason R1 exists. One book
section per client, each tested once against the real client.

**Done when.** Each listed client, configured only from the printed block, lists Hermit's
tools and completes one verified write.

**Struck if.** A client-side standard for MCP registration makes per-client blocks pointless.

**Size.** small. **Needs.** none (the calling client brings the model).

### 1.25 Expectation shortcuts

**Why.** `--expect preserved:notes.txt=notes.txt` is exact and unfamiliar. The common cases
are "this file must exist" and "do not touch these", and a newcomer should not have to learn
the grammar to state either.

**Shape.** Flags that compile to existing expectations and nothing else: `--creates PATH`
→ `exists:PATH`; `--keep PATH` → `preserved:PATH=PATH`. The verdict prints the compiled form,
so what is judged is never hidden behind the sugar. No new predicate kind; D15's judged
criteria are out of scope here.

**Done when.** The shortcuts parse to the identical `Expectations` set as their long forms,
proven in the suite, and the quickstart uses them.

**Struck if.** Review finds the aliases hide more than they save.

**Size.** small. **Needs.** none.

### 1.26 Thinking control — measured first

**Why.** Hermit sends no `think` field today (checked 2026-09-25: no occurrence in the client).
In the 1.15 gate run, five of 22 turns produced 79% of 55,806 generated tokens, most of it
thinking (RECORD.md, corrected). REQUIREMENTS R8 measured thinking models at 65–107 s per
turn against 6–17 s for non-thinking ones, and also found the thinking ones were the accurate
ones. So less thinking may cost accuracy, and that trade is the whole item.

**Shape.** First a measurement, frozen before it runs: the E1 task set, one thinking model,
default against reduced thinking (whatever `think` values the daemon version supports),
paired, with accuracy and wall clock both reported. Only if the trade favours it: a config
key, off by default until the numbers say otherwise.

**Done when.** The measurement is recorded with a verdict either way.

**Struck if.** Reduced thinking loses accuracy outside the noise floor at every setting tried.

**Size.** medium. **Needs.** 30B tier.

### 1.27 Keep the model warm, and reuse the prefix — measured first

**Why.** Every attempt is a fresh session by design (R7), and the system prompt and tool
menu are identical each time. Hermit sets no model `keep_alive` (checked 2026-09-25; the only
keep-alive in the client is the HTTP connection's). Whether reloads and repeated prompt
evaluation cost anything that matters is unmeasured.

**Shape.** Measure first: time-to-first-token and prompt eval time on attempt 1 against
attempts 2–3 of a retried task, under Ollama defaults. If the gap is real, set `keep_alive`
per request and check Ollama's prefix reuse; vLLM's prefix caching (D9) is the stronger
answer if it ever runs here.

**Done when.** The measurement is recorded, and either a change lands with before/after
numbers or the item is struck with them.

**Struck if.** Reload and prefix cost is under a few percent of attempt wall clock.

**Size.** small. **Needs.** 9B tier.

### 1.28 The thirty-second demo

**Why.** The whole pitch is one moment: a model says "done" over an untouched folder, and
Hermit says no and names the missing file. The runs recorded that moment many times. None
of them is watchable.

**Shape.** A terminal recording (asciinema, text not video, so it carries no generated-media
provenance) of a real run, not a staged one, with the model tag and date on screen. If the
first honest recording shows the model succeeding, record that too and keep looking. The demo
must not become the one run where the failure was coaxed.

**Done when.** The recording is linked from the README's first screen.

**Struck if.** Never; at worst it waits for 1.4.

**Size.** small. **Needs.** 9B tier.

### 1.29 A front door for newcomers

**Why.** The repository root holds 15 Markdown files, 7,520 lines between them (counted
2026-09-25). The book in `docs/` is the user path and is good, but a first visitor meets the
design record first and has no way to tell which files are for them.

**Shape.** Two parts. First, fix what is wrong in the book (a correctness audit against the
binary, 2026-09-25). Second, one "Start here in five minutes" path at the top of the README
that routes users to the book, evaluators to the design record, and contributors to 1.5's
files, and nothing else. The design documents are not shortened: they are the record, and
their length is their evidence. The README part touches claims and waits for 1.22's sign-off.

**Progress, 2026-09-25.** The correctness half is done: eight verified errors fixed across
chapters 1, 10, 12, 13, 17, 18, 20 and 31, one broken link, and one claim replaced by a
measurement. The binary is dynamically linked, not static, and starts in 0.8 ms median, not
~10 ms. README.md and FAQ.md carried the same wording; the sign-off diff that reframes the
README (1.22) corrects both, and adds the "Start here" routing that is this item's second half. Friction the audit found, all judgment and none of it done:

- no sample verdict (ch. 14) and no sample `undo` listing (ch. 17), so the reader cannot
  picture the output before running it;
- no copy-paste `mcp.json` and no smoke test in ch. 20 (1.24 answers this);
- ch. 12's measured field is all from the 16 GB laptop, with nothing for 24–72 GB cards;
- ch. 15 has fifteen lines of rationale before the flag, and no pre-check for Landlock;
- ch. 30 promises "run it yourself" eight lines before saying the commands cannot run today;
- the glossary does not define Kitchen, fsops, E1–E5 or "sweep 3".

**Done when.** The book matches the binary, and the README's first screen routes each of the
three readers in one click.

**Struck if.** Never; it can only be done badly.

**Size.** small-medium. **Needs.** none.

### 1.30 Three outside users — the demand test

**Why.** Everything in 1.22–1.29 assumes someone wants this. Nobody outside this repository
has installed Hermit, so that assumption is untested, and the "safe hands" frame in 1.22 is
argument. This repository does not build on untested assumptions.

**Shape.** After 1.4, 1.23 and 1.24 land: three people who already use agentic IDEs install
Hermit from a release, register it, and use it on real work for a week. Record what each
tried, where each got stuck, and whether they kept it. No survey; watch what they do.

**Done when.** Three reports are recorded, whatever they say.

**Struck if.** Never struck. If all three drop it, that is the result, and 1.22's frame is
retracted in public.

**Size.** medium (mostly waiting). **Needs.** none.

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
- **Adopting the Dream-RSI loop without its validation experiment.** Considered alongside the
  1.15–1.21 cluster (2026-09-21). The preprint asserts replay-to-online transfer and never
  measures it; an unmeasured self-improvement claim is precisely what this repository exists
  to distrust. The loop is docketed only with 1.21 attached as the exit gate.

---

## 3 · A proposed order for laptop sessions

The order below optimises for the outside-user goal first — the release and the contributor
surface come before the measurement items — then for cheapest evidence first within the
measurement work. The done-at-bound exit (ROADMAP § *The measured levers*) is the top
user-facing item but is already docketed there, so it is not re-listed. This is a suggestion
to be argued with, not a plan.

| # | Item | Why here | Size |
|---|---|---|---|
| 1 | ~~Commit map (1.13)~~ | **Done 2026-09-06.** | small |
| 2 | Book corrections (1.29, first half) | Nothing about a user's first hour should be wrong | small |
| 3 | Release tag + checksum (1.4) | Serves the outside-user goal directly; no dependency; an hour's work | small |
| 4 | `hermit doctor` (1.23) | First-run failures diagnosed in one place | small |
| 5 | Pasteable MCP setup (1.24) | For the 1.22 frame, registration is the install | small |
| 6 | Positioning + README front door (1.22, 1.29) | Proposed diff; needs operator sign-off | small |
| 7 | Contributor surface (1.5) | Serves the outside-user goal; small | small |
| 8 | Cloud provenance paragraph (1.11) | Small, dated, no dependency | small |
| 9 | Structured run trace (1.1) | Judge-usage logging, E2 metering and the hermit-bench parser all land on it | medium |
| 10 | Template probes in preflight (1.2) | Closes an open ROADMAP question in code; hermit-bench's gate becomes one call | medium |
| 11 | Verifier scale measurement (1.3) | Cheap, and it decides whether an optimisation item exists at all | small |
| 12 | CI on the self-hosted runner (1.9) | Everything above it becomes safer to merge | medium |
| 13 | Substrate matrix, then D11's probe (1.7) | The probe cannot be written before the matrix exists | small |
| 14 | Fuzzing (1.6) | Strengthens the claims the README leads with; benefits from CI existing | medium |

Unscheduled by design: Tier 1 tools (1.8) and multi-root (1.10) wait for a caller and a
workload respectively. Kitchen-gated items (retry quality gating, E6 calibration) live in
ROADMAP and hermit-bench, not here.

The **discovery cluster (1.15–1.21)** is also deliberately not in this order: it is a
dependency chain argued in [bench/discovery/DESIGN.md](./bench/discovery/DESIGN.md), to be
scheduled or struck as a unit. Its internal order is fixed — 1.15 gates everything, 1.21
gates the claim — and it consumes 1.1 when that lands.

**The product track (1.22–1.30), added 2026-09-25**, is interleaved above where it is cheap
and user-facing. The rest follows it: expectation shortcuts (1.25) after 1.4; the two speed
measurements (1.26, 1.27) once the trace (1.1) can carry their numbers; the demo (1.28) after
1.4 so it can show an installed binary; the demand test (1.30) last, once there is something
to hand an outside user. Operator direction the same day puts this track ahead of the
discovery cluster: the cluster stays designed and gated, and it does not bring a user.
