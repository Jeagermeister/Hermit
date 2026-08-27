# 2. The evidence

Every guarantee in Hermit traces to a failure that actually happened, on recorded hardware —
almost always with the run committed somewhere you can read, and labelled as an unrecorded
observation on the rare occasion it is not. None of them are preferences. This chapter
tells the stories at reading depth; the binding versions, with run counts and provenance, are
[REQUIREMENTS.md](../REQUIREMENTS.md) and the benchmark repository
([chapter 30](./30-benchmarks.md)).

The evidence base, in brief: local-model tournaments through OpenCode on `kitchen-desktop`, a
144-run diagnostic through Hermes Agent on the same machine, and 259 runs of the `fsops`
filesystem suite on an RTX 5080 laptop — later extended by a 360-run pinned-sampling re-run
and four pre-registered comparison experiments. Together they are why the following nine
requirements exist.

## The model wrote the right file in the wrong place — R1

Upstream's shell tool followed the process working directory and ignored the sandbox flag;
its file tools resolved paths differently again. Agents wrote exactly-correct files into the
wrong directories four levels up, and a 3B model scored 0/3 on the simplest task in the suite
having produced perfect content — a tool-design bug measured as model inability. Hence **one
sandbox root, set once, every path resolved against it explicitly** — never inherited, never
inferred, and escapes are refused rather than silently rebased.

## "The file still exists" passed while the content was destroyed — R3, R4

On a copy task, two models overwrote the original `config.ini` — one with the literal word
`(unchanged)`, one with invented config content — and never produced the copy. The assertion
"original still exists" passed in both cases. Hence **verify by content hash, never by
existence** (R3), and **back up before mutating** (R4): every overwrite is preceded by a
recoverable snapshot, and undo is a first-class operation. The first sweep recorded zero
collateral damage and concluded these models fail by inaction; the wider field in sweep 2
produced destruction on the second try. Guardrails are not optional.

## The harness's own decorations came back as file content — R5

A model wrote `5|2026-08-12 shipped` into a file — the harness's line-number prefix, echoed
back as content. Investigation showed the decoration was mostly the harness's own doing (226
of 240 recorded reads returned `N|`-prefixed content), which is why Hermit's tools return
**undecorated bytes with metadata in sibling fields, never interleaved** — and why every write
is **read back and compared** before the turn may succeed (R5).

## The model said DONE over an untouched tree — R6

`llama3.2:3b` replied `DONE` on an untouched tree in 18 of its 27 failed runs in sweep 1.
Separately, a model finished its work correctly and then hung past the timeout, never
declaring anything. Hence **completion is decided by inspecting the filesystem, not by parsing
the reply** — a supervisor waiting on the model's say-so hangs on the second case and is lied
to in the first.

## The same model, same task, three different outcomes — R7

39% of model×task cells in sweep 1 were unstable across three repeats; one model scored 3/3,
0/3 and 1/3 on the same task in three sessions. **This instability is the justification for
the whole architecture**: retry with one concrete remaining failure, fresh session each time,
state verified between attempts. If these models were consistent, an external supervisor would
be pointless.

## Thinking models are slow, and timeouts silently ate the data — R8

Thinking models measured 4–15× slower per turn (65–107 s vs 6–17 s medians) and were also the
only accurate ones; timeouts clustered on heavy tasks and dropped runs from the denominator.
Hence **wall-clock budgets per turn and per session, with a timeout recorded as a failure**,
never as missing data.

## Two installed models could never have worked — R9, and the template traps

One installed model had no `tools` capability at all; another's architectural context was
8192, below any usable floor — and the tool-use-tuned model was the one that could not be
loaded. Hence **preflight**: check the context window and tool capability against the live
daemon before the first request, and fail loudly at startup rather than sixty runs into a job.

Building the agent loop later surfaced two subtler gates the model card never mentions, both
properties of a model's *chat template*: on stock llama3.x templates the tool definitions
vanish from the prompt whenever the last message is a tool result — exactly the turn where the
model must decide whether to call another tool — and on `hermes3:8b` the caller's system
prompt is silently replaced wholesale the moment tools are offered. Both are measured, with
the mechanism read out of the templates, in [DECISIONS.md](../DECISIONS.md) § "Still open".
They are why [choosing a model](./12-choosing-a-model.md) has four gates rather than two.

## Malformed calls, and the fix that backfired — R2, D5, D12

A 3B model emitted a Python list where a string belonged, and a whole tool call as prose that
was never parsed. The first answer was constrained decoding on tool arguments (D5). Measured
later, on seven models: adding a `format` schema *alongside* tools took four of seven from a
correct call to no usable call at all — the cure reintroduced the disease. So tool calls ride
the models' native tool channel, `format` is reserved for structured replies with no tools
offered, and the combination is unrepresentable in the client's types
([D12](../DECISIONS.md)). The episode is worth knowing as a user because it is the project's
method in miniature: the mechanism was measured, the measurement overturned the decision, and
the record of being wrong stayed in the file.

## What the evidence does not say

The claim that upstream lacks Hermit's verification layer is inferred from behaviour across
hundreds of runs, not from auditing upstream's code — [REQUIREMENTS.md](../REQUIREMENTS.md)
states it precisely, and if a later module read turns up an equivalent, that is a finding, not
a contradiction. Per-model numbers do not travel between machines. And a benchmark that only
ever flattered its own product would be advertising — which is why the experiments that
measured Hermit also caught two real defects in it, published beside the wins
([chapter 30](./30-benchmarks.md)).
