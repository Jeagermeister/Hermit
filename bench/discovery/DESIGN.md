# Discovery — a replay-guided exploration loop for Hermit

**Status: draft, 2026-09-21. Docketed as 1.15–1.21. Nothing here is scheduled, and nothing
here is measured.**

**Status, 2026-09-22: the gate (1.15) is passed as written, with one gap.** The domain is
sum–difference Γ in [`sumdiff/`](./sumdiff/README.md), an exact objective computed outside
the candidate's confined root. Hand controls score 1.0 and 1.034421. One `qwen3.8:27b`
attempt found a set scoring 1.046752 but ran out of budget before putting it in `solve.py`,
so the attempt as delivered scored 1.0 ([record](./sumdiff/results/2026-09-22-gate/RECORD.md)).
n = 1, so no rate is claimed. Everything below this line is still unmeasured. This file is a design target, argued out before code exists, in the same
discipline as `bench/delta/DESIGN.md`. It exists so the idea can be killed cheaply at the
right component rather than expensively at the end.

---

## Where the idea comes from

An external paper: **Dream-RSI** — *Recursive Self-Improvement through Evolving Worlds*
(arXiv:2609.14858v1, dated 2026-09-14, Google / Google DeepMind / U. Maryland / U. Virginia).
Three-sentence version: run an unmodified coding agent under an explicit, programmable
exploration policy (which branches to extend, how many attempts in parallel, when to stop);
record the whole discovery tree — every attempt's workspace, score, and diagnostics; then
treat that recorded tree as a *replay simulator* in which thousands of alternative policies
can be evaluated offline at near-zero cost, and redeploy the best one online. A loop.

**Provenance, stated plainly so nobody cites it as evidence.** It is a preprint. Its
referenced code repository has not been verified to exist. Its headline numbers are single
trajectories with no reported variance — and this repository's own Phase 0 established, by
measurement, that run-to-run variance in exactly this class of model behaviour is large
(`qwen35` went 6/6, 6/6, 4/6 on identical inputs). Its strongest-looking result is carried by
one dataset. **It is a source of design ideas, not of evidence, and nothing in this file
inherits its claims.** The one thing of substance it offers for free is a well-argued
identification of where the cost in long-horizon discovery actually sits: meta-level feedback
is delayed and expensive, so exploration strategies are nearly impossible to improve online.

## Why this belongs in Hermit at all

The paper's mechanism is Hermit's epistemics pointed at a different object. R6: never trust
a completion claim; decide from state. D13: observe the tree, never the reply. D17: rebuild
the window from the tree, discard the model's narration. Dream-RSI is the same rule one level
up: never trust a policy's self-report; decide from *recorded realised outcomes*. If this
repository believes its own requirements, they compose upward — and the backup store, the
hash-verified snapshots, and the bounded fresh-session architecture are already most of what
a replay simulator needs for records.

There is also a gap running the other way, worth naming now: the paper *assumes* a fixed,
trustworthy evaluator and per-attempt workspace state, and does nothing to secure either.
Hermit's R3/R4/D10 layer is exactly that integrity boundary. A replay pool whose nodes are
hash-preserved workspace snapshots is tamper-evident in a way the paper's is not described
as being.

## The honest scope

This is **harness-level work**. Nothing is trained, no weights move, no model is modified,
and nothing about it conflicts with DOCKET's set-aside "Training a worker model" gate — that
item and this one live on opposite sides of the line. The "self-improvement" on offer is
narrower than the paper's title: a per-task exploration controller gets better between
rounds. It is closer to what R7 already is — a fixed supervisor policy — with the fixed part
made improvable. If the validation experiment (1.21) says the improvement doesn't transfer
online, the correct outcome is a one-line entry in "Considered and set aside", and this file
stays as the record.

## What already exists, mapped

| Dream-RSI component | Hermit today | Status |
|---|---|---|
| Discovery agent (unmodified coding LLM) | The Ollama client + bounded sessions + tool surface | exists |
| Per-attempt workspace preservation | R4 backup store, per-turn hash diff (D13) | exists, stronger than the paper's |
| Evaluator | `judge` (structural) + `semantic` (D15) | exists, but **binary/predicate, not scalar** |
| Fixed exploration policy | R7's compiled-in "retry ≤3 with one failure" | exists, degenerate, hard-coded |
| Fresh session per attempt | the architecture itself | exists |
| Parallel workers (W) | D1: one blocking thread; DOCKET 1.10's N-processes design | **missing** |
| Tree-structured discovery history | trace lines + `usage.jsonl`; DOCKET 1.1's structured trace docketed | **missing** |
| Scalar score per attempt | nothing | **missing — the gate** |
| Replay simulator | nothing | **missing** |
| Policy as rewritable data | nothing — policy is compiled C++ | **missing** |
| Meta-loop (policy-development agent + selection) | D7's second front door (MCP) is the natural vehicle | **missing** |
| Cost metering including the meta-agent | DOCKET 1.1's trace (also docketed) | **missing** |

## The components, in dependency order

Each of these is a docket item with its own why/shape/done/struck. The order is the argument:
every component is useful *without* its successor, so any stopping point leaves value behind
rather than scaffolding.

### 0 · A scored discovery domain — docket 1.15 — **the gate**

Every Dream-RSI result sits on a domain with a cheap, honest, scalar score — solver runtime,
kernel speed, a mathematical objective. Hermit's verdicts are met/unmet. Without a scalar,
there is no replay objective, no policy improvement, no experiment; so this component goes
first and is allowed to kill the whole file.

Candidate shape: optimisation-in-the-sandbox — the model edits a program inside `--root` to
improve a *measured* number (the harness program's runtime under a pinned input; an objective
value printed by a fixed scorer). The scorer runs under the existing confined `shell`, and
writes `eval/score.json`. Structural predicates still gate correctness (an attempt that
breaks the scorer's own contract scores zero), but the optimisation target is a number.

**Prefer a measured scalar over a judged one for v1.** D15's semantic judge is a model;
models are stochastic; a stochastic score weakens replay determinism, which is the cheap part
this whole design buys. A runtime or an objective value has no such problem.

### 1 · Attempt-tree recording — docket 1.16

Every attempt becomes a node: its parent, its workspace (preservable via the backup store),
its proposal, its scorer diagnostics, its score. DOCKET 1.1's structured run trace is the
event source this consumes, which is why 1.1 sits earlier in Part 3's order already. Records
are hash-chained in R3's spirit: the replay pool should be provably unmodified after the
fact, because a meta-agent that can rewrite history is a failure mode this repository has
requirements about.

### 2 · Parallel refinement runner — docket 1.17

W attempts in flight as W *processes* on disjoint branch workspaces under one job root,
joining on hash — this is DOCKET 1.10's already-written design finding its first workload,
and it preserves D1 outright. The initial policy is fixed parallel-refine, which is both the
paper's Round-1 policy and, deliberately, the controlled baseline for 1.21.

### 3 · Exploration policy as data — docket 1.18

The seam the loop hangs on. The per-decision-round controller — which frontiers to extend,
batch composition up to W, when to stop — becomes a consulted artifact instead of compiled
logic. v1 is data (a policy file), not arbitrary code, because a data schema is auditable and
refusable; the paper rewrites code, and code is where its prompt-level anti-leakage
guardrails are weakest. Graduate to code only if a data schema is measured to be too narrow.

### 4 · Replay simulator — docket 1.19

Offline, model-free, deterministic reveal over recorded trees: a candidate policy navigates
the frozen grid, is shown recorded children only, and is scored (best score seen, minus cost
per attempt, plus a parallelism term). Pure software, no model involved, testable with hand-
built trees. It lives in bench tooling first — it is a measurement instrument, and products
graduate from instruments here, not the reverse. **Note the paper's own bound, inherited
deliberately:** replay can only re-navigate recorded branches, in recorded order; a policy
that would explore genuinely new ground cannot be scored. The simulator answers "which
re-use of seen work pays best", not "what should we try that we never have".

### 5 · The dreaming loop — docket 1.20

A policy-development agent reads replay trajectories and proposes a revised policy; M
revisions per round are replay-scored; the best redeploys online, with the incumbent always
in the candidate pool so selection is never-worse *on the replay objective*. (The paper's
only guarantee, quoted with its exact scope — it says nothing about online quality, which is
what 1.21 is for.) The vehicle is D7's machine front door: a larger model driving this over
MCP, or a bench-side script. The anti-leakage rule is made *mechanical* rather than
prompt-level where possible: the replay API exposes prefix-observable state only, the same
trick D6 plays with paths — the unsafe thing is unrepresentable rather than forbidden.

### 6 · The validation experiment — docket 1.21 — the exit gate

Pre-registered, frozen protocol, in hermit-bench's manner, on 1.15's domain. Two questions
the paper did not answer and this repository is built to answer:

1. At matched discovery-agent call budgets, does a replay-improved policy beat fixed
   parallel-refine online, across enough repeats to see through the variance Phase 0
   measured?
2. Does replay score actually predict online outcome — the correlation the entire mechanism
   assumes and the paper never plotted?

Losses recorded beside wins, whichever way it lands. A null result closes 1.20 and settles
the RSI question for this codebase at the cost of components 0–4, all of which remain useful
instruments.

## Explicitly not decided here

These are the D-shaped questions this file deliberately does not settle. They are listed so
nobody mistakes a draft design for a decision:

- **Model tier for the discovery agent.** 9B-tier discovery quality is unmeasured; 30B tier
  is the likely floor for the validation experiment. That is a measurement, not a decision.
- **Cloud for the meta-agent.** D18 admits Ollama Cloud narrowly and `--allow-cloud` exists.
  Bias is local-first (30B tier as the policy-development agent); if cloud is used, its
  tokens are metered through the same trace as everything else — the paper never counted its
  meta-agent's cost, and this repository has E2 docketed precisely because it does count.
- **Where the runner and simulator live long-term.** Bench tooling first; promotion to the
  product is a parity.tsv conversation, not a today conversation.
- **The policy schema itself.** Deferred to 1.18's own design round; the only commitment here
  is *data before code*.

## What would kill this file

- 1.15 fails: no honest scalar domain at local tier. Cheapest death, and the reason the gate
  is first.
- 1.21 returns null: replay-selected policies don't beat fixed ones online, or replay score
  doesn't predict online outcome. Then the components stand as instruments and the RSI claim
  is recorded as *set aside with evidence*, which is the outcome this repository exists to be
  able to have.
