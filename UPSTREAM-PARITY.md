# Upstream tracking

This project is **not a port** of [NousResearch Hermes Agent](https://github.com/nousresearch/hermes-agent).
It is an independent C/C++ agent supervisor that borrows selectively from upstream's behaviour.
There is no shared git history and nothing is ever merged in.

Most of upstream is deliberately `OUT_OF_SCOPE` — see [SCOPE.md](./SCOPE.md). The ledger is
**module-granular** for in-scope code and whole-subsystem for what is ignored, so the table
below shows subsystem totals for context, not the unit the ledger tracks. For the twelve
modules that *are* in scope, upstream remains a useful reference, and one question needs
answering periodically:

> **Has upstream changed something in an area I borrowed from, in a way I should know about?**

`parity.tsv` records the scope and the answer. `tools/parity` reports it.

## Granularity: release tags, not commits

Each in-scope subsystem records the upstream **release tag** its behaviour was implemented
against — not a commit SHA.

This is deliberate. Measured 2026-08-13: upstream landed **5,715 non-merge commits in 30 days**
and cut **4 tags** in the same window. Tracking commits would mean triaging ~5,700 diffs a
month, which nobody sustains; the ledger would rot within weeks and then lie to you. Tags are
the natural review unit, and they reduce the job to ~4 review events a month.

Commit-level detail stays one command away (`tools/parity <subsystem>`). The ledger stays
coarse; the drill-down stays sharp.

## Scope

| Subsystem | Upstream LOC | Status | Why |
|---|---:|---|---|
| `agent/` | 136,525 | in scope | The loop. The core idea worth borrowing. |
| `tools/` | 128,965 | in scope | File operations — what this project is *for*. |
| `providers/` | 452 | in scope | Only Ollama matters here; upstream's is tiny anyway. |
| `hermes_cli/` | 213,032 | **out** | Largest subsystem, almost all surface area. |
| `gateway/` | 103,744 | **out** | Discord/Telegram/messaging. Not wanted. |
| `plugins/` | 128,457 | **out** | Extension surface for a program that does not exist yet. |
| `tui_gateway/` | 26,422 | **out** | |
| `skills/` | 16,505 | **out** | |
| `cron/` | 11,695 | **out** | |
| `acp_adapter/` | 5,809 | **out** | IDE integration. |

Upstream is **~867,000 lines of non-test Python** across 1,135 files. The three subsystems in
the table are ~31% of that; the twelve modules actually in scope are **~38k lines, about 4%** —
see [SCOPE.md](./SCOPE.md), which does the module-by-module accounting. Less still in practice,
since the goal is borrowing behaviour, not reproducing implementation.

`OUT_OF_SCOPE` exists so drift in those areas is **ignored on purpose** rather than quietly
accumulating as unread debt.

## Usage

```bash
tools/parity                     # scope + what has drifted
tools/parity conversation_loop   # commits touching one module since it was recorded
```

After implementing behaviour up to some tag, record it:

```
conversation_loop	agent/conversation_loop.py	v2026.8.13	PORTED
```

`STALE` is computed, never written by hand — the script derives it by comparing your recorded
tag against the newest upstream tag.

## Keeping the reference current

The reference clone lives at `~/Source/hermes-upstream` (override with `$HERMES_REF_REPO`).
It is blobless and single-branch — 261 MB against 5.8 GB for a full mirror, because upstream
carries 1,505 branches of which only `main` matters here.

```bash
hermes-upstream-sync      # fast-forward it and print what changed
```

It is disposable: it re-derives from GitHub in about 11 seconds, which is why it lives on the
local machine rather than on Gitea. **This** repo is the irreplaceable half.
