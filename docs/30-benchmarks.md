# 30. The measurements

Every number this project quotes is meant to be reproducible, auditable, or refutable by
someone who is not us. The suites, pre-registered protocols, and complete results live in the
**hermit-bench** repository — a sibling repository, extracted from this one so the
measurements can stand on their own; the copy under this repo's `bench/` is the legacy
in-tree version, kept because older documents cite it. This chapter is the user's map — what
each experiment asks, what it found, and how to run it yourself.

## The rules the numbers were collected under

1. Protocols and graders are **frozen before the first run**; changes after that are a new
   experiment.
2. One referee for every arm, applied to **the filesystem — never the transcript**. A model
   once "copied" a file by re-typing it one byte wrong; only the hash caught it.
3. **Timeouts are failures**, never missing data.
4. **Losses are published with the wins**; discarded collections are quarantined rather than
   deleted, and the logs behind every log-traced claim are committed.
5. **Paired deltas on identical tasks, never leaderboards.** Per-model, per-machine numbers
   do not travel, and no claim is made about models other than the ones named.

The series has caught real defects in Hermit itself and published them — a benchmark that
only ever flatters its own product is advertising.

## fsops — which models can be trusted with the primitives

Sixteen filesystem tasks (twelve single-file primitives, four opt-in repository-scale ones),
each with per-task assertions **plus collateral-damage detection**: every file the task did
not authorise is hashed before and after, and touching one fails the run even if the goal was
met. A canary file sits one level above the working tree to catch escapes, and a self-test
performs every task correctly and then sabotages a subset, requiring the referee to catch
each — so an impossible task cannot masquerade as a hard one.

Three sweeps exist. Read **sweep 3** (2026-08-26, 360 runs, sampling pinned, trees outside
any repo) as the current authority. It corrected sweep 2's central conclusion — "no fast,
accurate, small option exists" was an artifact of the best model never having been tested,
and `gemma4:e4b-it-qat` won the suite on its first run ([chapter 12](./12-choosing-a-model.md)) —
and it separately confirmed that a working-directory escape had depressed sweep 2's totals
beyond the tasks the evidence could name, so those earlier numbers read as lower bounds.
Its own limitations section is a model of the genre; the one to internalise is that repeats
are not independent samples — the effective n is 12 tasks per model, not 60 runs.

## delta — does the supervisor actually help?

Five pre-registered experiments, each frozen before its first run.

- **E1 — the headline.** The same tasks, the same `qwen3.5:9b`, through upstream Hermes Agent
  as shipped versus Hermit: **26/35 (74%) → 33/35 (94%)**, four task-level wins, zero losses,
  three ties — reported as *underpowered, not proof* (p = 0.125 is the best seven paired
  tasks can do). The retry mechanism fired on one task and converted 4 of 4. Every remaining
  supervised failure was semantic, none structural: in 70 supervised runs nothing was
  destroyed, nothing escaped the sandbox, and no undeclared file changed. The experiment also
  caught two real Hermit defects and a 14% malformed-tool-call rate on the transport — all
  published.
- **E3 — the same question at 27B.** Every cell at the ceiling — 105/105 runs in all, 35/35
  per cell: the pre-registered "supervision costs nothing measurable" outcome, at a ~3.5×
  wall-clock advantage. At that tier the value is containment and audit, not correction.
- **E4 — repository scale, 30B coder.** The tier discriminates (55% baseline); supervision is
  a wash at five repeats; and the semantic judge's first dogfooding measured its error mode
  honestly in both directions — blind where nothing was stated, confabulating where the judge
  model was the wrong one.
- **E5 — the same tier, three general models.** One near-ceilings it; every baseline failure
  lands on the same silently-wrong lockfile line of the judge-blind task; and the judge
  question closed — **zero false unmets in 120 runs** with general judges, so E4's
  confabulation was the judge model, not the design. Four models now show the same law:
  *where the judge sees, nothing slips silently; where it is blind, everything does.*
- **E2 — economics, frozen and waiting.** Tokens per completed task, native tools versus the
  Hermit MCP surface as the only hands. The MCP frontend it was waiting on shipped 2026-08-28
  ([chapter 20](./20-mcp-and-kiro.md)); collection has not run yet.

## Reproducing

Both suites are plain Python 3 over a local Ollama daemon, in the hermit-bench repository:

```bash
cd fsops
./selftest.py            # no model needed; proves the suite is scoreable
./run_fsops.py --plan    # what a sweep would run, and for how long

cd ../delta
./run_delta.py --selfcheck   # the shared referee (expectation half needs HERMIT_BIN=…)
./run_delta.py --plan
./run_delta.py --report      # recompute the published tables from committed results
```

Collecting new data needs the model tags in `fsops/README.md`, upstream Hermes Agent for the
baseline arm, and a `hermit` binary for the supervised arms. Numbers you collect are yours:
comparable within your machine, not with ours. The repository's own `GUIDE.md` walks the
reading order and the pitfalls (the recording proxy destabilises long runs; reasoning level
has never been a controlled variable; per-cell numbers at n=3 are not capability).

## How to read any number from this project

- **A paired delta on one machine** is the strongest form here; a raw suite score ranks
  models against each other on that machine only.
- **"Underpowered" is said out loud** where it applies, rather than rounded up to proof.
- **Corrections stay in the documents** — sweep 2's central conclusion is preserved under the
  banner that corrects it, because deleting the record of being wrong would make the next
  reader trust the current claim less, not more.
