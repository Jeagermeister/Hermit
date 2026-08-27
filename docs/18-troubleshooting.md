# 18. Troubleshooting

Symptom-first. Where a fix says "by design", the link says why — most of what looks like
Hermit being difficult is Hermit refusing to guess.

## Preflight refuses the model

- **"context undetermined" or below the floor.** The gate asks for the model's
  *architectural* context — what it was built for — from the live daemon. A model whose
  architecture is genuinely under the floor (`llama3-groq-tool-use:8b` at 8192) cannot be
  raised by any setting. The floor itself is `--min-context` if your use genuinely needs
  less; the waiver prints as a marked line.
- **"no tools capability."** The model cannot emit structured tool calls; it would score zero
  for reasons unrelated to filesystem ability. `--no-tools` waives the gate, which is only
  useful for `session`-style probing — `agent` with a toolless model is pointless.
- **Preflight passes, the model still behaves oddly.** Two known traps pass every live gate
  because they live in the chat template: llama3.x tags lose their tool definitions after a
  tool result, and `hermes3:8b` silently drops your system prompt whenever tools are offered.
  [Chapter 12](./12-choosing-a-model.md) § "the four gates".

## The model forgot everything mid-run

The context cliff. When a prompt exceeds the window, Ollama does not trim proportionally — it
silently keeps the system prompt and a fragment, and 10% over costs the same as 118% over
(measured: a 4,496-token prompt into a 4,096 window kept **44 tokens**). The model still
*sounds* configured, having forgotten everything it did. Hermit's session accounting exists
to prevent this — it budgets pessimistically, calibrates against what the daemon actually
evaluated, and drops whole call-and-result groups deliberately rather than letting the server
discard the middle. Run `hermit session --model <tag> --max-num-ctx 2048` to watch the
machinery work. If you see collapse anyway, that is a bug worth reporting with the trace.

## The reply came back empty, no error

Check the summary for `done_reason == length`. A thinking model that exhausts its generation
budget mid-think writes nothing: it reasoned, ran out, and stopped. Raise the budget or use a
non-thinking model for small tasks ([chapter 12](./12-choosing-a-model.md)).

## I raised `--max-num-ctx` and the machine hard-froze

Ollama performs no admission control on context size: an oversized request is not rejected —
on the recorded occasion it deadlocked the GPU driver with no OOM kill and no reset
([D8](../DECISIONS.md)). The clamp's default (65536) is a value known to fit a 16 GB card.
Raise it only within what your card actually holds; nothing in the API reports free VRAM, so
Hermit cannot compute this for you — it can only refuse to exceed what you stated.

## `--shell` refuses to start

The confinement probe could not confirm the kernel is actually enforcing a Landlock ruleset —
it attempts a denied write and requires `EACCES`. A kernel without Landlock, with it compiled
out, or an LSM list that excludes it will all refuse. This is a refusal, not a warning, by
design: the alternative is silently running an opaque shell unconfined
([chapter 15](./15-shell-and-landlock.md)).

## Exit 3, attempt after attempt, same failure

The retry loop restates *your* expectation, so first suspect the expectation. Only
structurally illegal specs are refused up front (absolute paths, `..`, contradictory sets);
a path that simply does not exist parses fine — deliberately, since expectations usually
name files the run is supposed to create — which means a **misspelled or wrong-case path is
a permanent unmet**, restated to the model attempt after attempt. So is an expectation
*stricter than the task's bar*: a byte-identity `preserved:` on a file the task legitimately
rewrites sends every retry chasing the impossible. Check the spelling against the tree, and
assert the task's bar, not a stronger one ([chapter 14](./14-expectations.md)).

## The verdict says `undecidable`

One side could not be read — an unreadable directory, a file over the read cap, a judge that
could not be reached or answered unparseably. Undecidable is reported to you and never sent
to the model, never retried: a model can do nothing with "something could not be read". Fix
the readability problem and re-run.

## A `satisfies:` verdict looks wrong

It is a model's opinion and labelled as one. The measured failure mode is a *code-specialised*
judge confabulating unmet verdicts over correct trees; general models measured zero false
unmets in 120 runs. Bring `--judge-model` with a general model
([chapter 12](./12-choosing-a-model.md) § "the judge model").

## The run seems wedged

A blocking single-threaded loop cannot interrupt a request already in flight, so the honest
worst case is your `--budget` plus one `--chat-timeout` — a wedged daemon holds the run until
the chat timeout fires. Bound both. A timeout is recorded as a failure, never as missing
data (R8).

## Config and flag surprises

- **`--config /some/dir` fails** — the config must be a regular file, never searched for
  implicitly, and a directory (or FIFO, or device) is refused up front.
- **"unknown key" on a config file** — deliberate. A typo'd safety setting silently dropped
  would leave a default in force while you believed otherwise.
- **`HERMIT_SANDBOX_ROOT=./x` rejected** — environment paths must be absolute; a relative
  path there has no honest anchor ([chapter 16](./16-configuration.md)).
- **A path or instruction starting with `-`** — use `--` to end flag parsing. Models pick
  filenames; some of them start with dashes.
- **`--url http://<lan-ip>:11434` refused** — loopback only, at configuration time, by
  decision ([D7](../DECISIONS.md)). Hermit runs on the same box as its backend.

## Something was destroyed anyway

First: `hermit undo --root <dir>` lists every preserved generation
([chapter 17](./17-undo-and-backups.md)). Second: the per-turn changeset in the run output is
the authoritative account of what changed, including anything `shell` did. If a tree changed
in a way neither shows, that is precisely the class of event this project treats as a
first-order bug — report it with the run trace.
