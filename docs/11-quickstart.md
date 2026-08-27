# 11. Quickstart

From a fresh build to a verified run. Everything here happens on one machine; nothing leaves
it.

## 1. Have Ollama running, with a model that clears the gates

```bash
ollama pull qwen3.5:9b
```

`qwen3.5:9b` is the model the supervisor was built and measured against, and the safe first
choice. `gemma4:e4b-it-qat` leads the raw benchmark field (90% at ~7× the speed) and is worth
trying second — [chapter 12](./12-choosing-a-model.md) has the full field, the four gates, and
the models to avoid.

You do **not** need a context-pinned model variant. Hermit sets the context window per request
on the native API, which overrides a Modelfile pin upward ([D8](../DECISIONS.md)); the
64k-pinned variants some of the benchmark tooling uses are an artifact of a different
endpoint.

## 2. Preflight the model

```bash
./build/hermit preflight --model qwen3.5:9b
```

This asks the live daemon — not the model card — whether the model's architectural context
clears the floor and whether it can call tools, and fails loudly now rather than sixty runs
into a job (R9). If preflight refuses your model, [chapter 18](./18-troubleshooting.md) says
what each refusal means.

## 3. A first run, read-only

Point it at some directory with text in it. There is deliberately no default root — every
candidate default is an inferred root, which is the original bug this project was built
against — so `--root` is always explicit:

```bash
./build/hermit agent --root ~/scratch --model qwen3.5:9b --max-turns 8 \
  'Read notes.txt and tell me how many lines it has.'
```

Watch the output: one trace line per turn and per tool call, then a summary that says which
bound stopped the run, and a hash-verified changeset of what actually moved in the tree —
computed from snapshots, owing nothing to the model's reply. For a read-only task the
changeset should be empty, and that emptiness is evidence.

## 4. A real run, with a stated post-condition

A free-text instruction carries no post-condition, so state one:

```bash
./build/hermit agent --root ~/notes --model qwen3.5:9b \
  --expect exists:falcon-index.md --expect preserved:notes.txt=notes.txt \
  'Create falcon-index.md listing every note that mentions Project Falcon, with its date.'
```

Now the run does not merely report — it judges. After every turn each expectation is decided
against the tree (`met`, `unmet`, or `undecidable`), and if something stated remains undone
when the model stops, Hermit re-invokes it: a fresh session, the original task, plus the one
concrete failure — *falcon-index.md does not exist* — up to three attempts by default. The
grammar and the judgment rules are [chapter 14](./14-expectations.md).

`preserved:notes.txt=notes.txt` is the "do not touch this file" constraint — worth stating for
anything the task should read but never modify.

## 5. Read the exit code

| exit | meaning |
|---|---|
| `0` | the run finished; nothing stated is unmet |
| `1` | the run did not finish (a bound, a refusal, infrastructure) |
| `3` | something you stated is measurably undone |

With no `--expect` the run stays report-only and cannot exit 3 — a clean stop plus an accurate
changeset is evidence, not a verdict, and a command that implied otherwise would be the more
useful lie. (Usage and configuration errors exit `2`; the full table is in the
[CLI reference](./13-cli-reference.md).)

## 6. Know where the net is

Every overwrite during the run preserved the old bytes first, to a store beside your root that
the model cannot reach:

```bash
./build/hermit undo --root ~/notes
```

That lists what can be restored and changes nothing; restoring is an explicit flag away, and a
restore is itself undoable ([chapter 17](./17-undo-and-backups.md)).

## Where to go next

- The full flag surface: [CLI reference](./13-cli-reference.md).
- Giving the model a real shell, kernel-confined: [chapter 15](./15-shell-and-landlock.md).
- Config files and environment variables: [chapter 16](./16-configuration.md).
- When something refuses, freezes, or lies: [chapter 18](./18-troubleshooting.md).
