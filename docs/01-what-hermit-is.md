# 1. What Hermit is

Hermit is a supervisor for local language models doing filesystem work. You give it one
instruction and a directory; it hands the instruction to a small model running on your own GPU,
gives that model a fixed menu of file tools, watches what actually happens on disk, and decides
for itself — from the filesystem, never from the model's reply — whether the work got done. If
it did not, Hermit re-runs the model in a fresh session with the one concrete failure that
remains.

The name is an acronym, and each letter is a mechanism you can point to in the code:
**H**ash-verified · **E**vidence-driven · **R**etries with concrete failure · **M**inimal
structurally-enforced authority · **I**ndependent verification · **T**iered dispatch.

## A supervisor, not a chatbot

There is no conversation window and no long-running session. Hermit's architecture follows an
empirical finding from local-model tournaments run before any of this code existed
([the evidence](./02-the-evidence.md)): small models drift over long autonomous sessions, and
what recovers them is an *external* process that checks repository state and re-invokes with one
concrete remaining failure. So Hermit's unit of work is a **bounded session** — capped in turns,
wall-clock, and calls per turn — and its product is the machinery around the model, not the
model itself:

- Every path the model can name is resolved against one sandbox root, or refused. There is no
  way for a tool to address a file outside it — not as policy, as a property of the type system
  ([D6](../DECISIONS.md)).
- Every write is read back byte-for-byte before it counts. Every overwrite is preserved to a
  backup store the model cannot reach. Every move is hash-verified at both ends
  ([architecture](./03-architecture.md)).
- After every turn, the whole tree is diffed by hash against a snapshot. What the model *says*
  it did is never consulted — the recorded runs include models announcing success over an
  untouched tree, and one describing a file it had been handed as containing something else
  entirely ([D13](../DECISIONS.md)).
- A stated post-condition (`--expect exists:report.md`) is judged against the tree after every
  turn, and an unmet one drives a retry: a fresh session, the original task, plus that one
  named failure ([expectations](./14-expectations.md)).

The arithmetic the retry rests on: a task these models complete ~67% of the time per attempt,
retried up to three times with state verification between attempts, approaches ~96%. That is
computed from measured instability, and testing it as a measurement is what the
[delta experiments](./30-benchmarks.md) exist for.

## Two front doors

This is a decision, not an accident ([D7](../DECISIONS.md)): the same core is driven two ways.

**A person at a terminal.** You run `hermit agent --root ~/notes --model <tag> "..."` and a
model on your own machine does the work. Nothing leaves the machine — the backend is local
inference only, and a non-loopback URL is refused at configuration time.

**A larger model, calling Hermit as a tool.** An agentic IDE registers Hermit as an MCP server
over stdio; the cloud model does the thinking and calls Hermit for the hands. The pitch there
is the ecosystem's own warning inverted: a typical stdio MCP server runs with all the launching
user's authority, and this is the one that *reduces* authority instead of inheriting it. As of
2026-08-27 the MCP frontend is designed and its safety gate is cleared, but the server itself
is not yet built — [chapter 20](./20-mcp-and-kiro.md) keeps the honest status.

## What Hermit is not

**Not a port.** It is inspired by [NousResearch Hermes Agent](https://github.com/nousresearch/hermes-agent)
and deliberately not a reimplementation of it. Upstream is ~870k lines of non-test Python;
about 38k of it is even in scope as a behavioural reference, and the part of Hermit that
matters most — verification, backup, retry — has no upstream equivalent that the recorded runs
revealed ([SCOPE.md](../SCOPE.md), [REQUIREMENTS.md](../REQUIREMENTS.md)).

**Not a cloud client.** No cloud inference provider, ever, unless [D7](../DECISIONS.md) is
explicitly overturned. That keeps credentials, TLS, and egress policy out of the codebase
entirely, and bounds the blast radius: a confused local model with filesystem access is a
contained problem.

**Not cross-platform.** Linux is the product ([SCOPE.md](../SCOPE.md) § Platforms). The
guarantees were designed against POSIX semantics and Landlock — with a decided-but-unbuilt
probe ([D11](../DECISIONS.md)) to validate them per-filesystem rather than assume them; if
Windows ever happens it is a separate package, never an `#ifdef` in this tree.

**Not magic.** Structure is not meaning. Four structural predicates cover 329 of the 413
failures in the evidence base; the residue needs a semantic judge, which is a model's opinion
and is labelled as one everywhere it appears ([D15](../DECISIONS.md)). A file named correctly
and filled with nonsense satisfies `exists:` perfectly — a thing that has actually happened
in a live run — and the limit is printed on screen rather than hidden.

## Why native code

Not for inference speed — the process is blocked on the model and always will be. The wins are
structural: bounded sessions mean many process launches, and a static binary pays ~10 ms where
a Python interpreter pays 1–3 s, every time; per-turn verification is real tree-walking and
hashing work; and one binary distributes where a Python environment does not. The honest
version of this argument, concessions included, is in the [FAQ](../FAQ.md).
