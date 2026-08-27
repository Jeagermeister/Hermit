# 13. CLI reference

One binary, six subcommands. This chapter matches the binary as built 2026-08-27;
`hermit --help` is always the tie-breaker, and `hermit config` shows every setting actually in
force and where it came from.

```
hermit resolve   --root DIR <path>...
hermit preflight --model NAME
hermit session   --model NAME
hermit agent     --root DIR --model NAME <instruction>
hermit undo      --root DIR
hermit config
```

## The subcommands

### `hermit agent` — the product

One instruction, one root, one model. Offers the eight structural tools (nine with
`--shell`), drives the model through a bounded session, prints one trace line per turn and
per call, a hash-verified changeset after every turn, a verdict for anything stated with
`--expect`, and a summary naming which bound stopped the run.

### `hermit undo` — the recovery door

Lists the backup store's generations by default; mutates only by explicit flag.
[Chapter 17](./17-undo-and-backups.md).

### `hermit preflight` — the model gates

Runs R9's checks against the live daemon: architectural context floor, `tools` capability,
optional inference warmup. Fails closed — "could not determine" is a failure, not a pass.

### `hermit session` — context accounting, visible

Exists because the token estimate is the one thing no unit test can settle. Each turn prints
what the session expected against what Ollama actually evaluated. Run it with
`--max-num-ctx 2048` to watch history being compacted *deliberately* — the alternative, left
to the server, is a silent discard of nearly everything
([troubleshooting](./18-troubleshooting.md) § the context cliff).

### `hermit resolve` — path resolution, visible

Shows how paths land against a sandbox root from a different working directory — R1's
original failure made inspectable:

```bash
cd /tmp && "$OLDPWD/build/hermit" resolve --root ~/some/root note.txt ../../etc/passwd
```

### `hermit config` — the resolved settings

Prints every setting in force with the origin of each value (default, file, environment, or
flag). Anything unusual — a raised clamp, a waived gate, shell enabled — prints as a marked
line. [Chapter 16](./16-configuration.md).

## Global settings (all subcommands)

Precedence, increasing: **defaults < `--config` file < environment < flags.**

| flag | meaning |
|---|---|
| `--config PATH` | a JSON config file. Never searched for implicitly |
| `--root DIR` | sandbox root (R1). No default, ever |
| `--model NAME` | the Ollama tag to drive |
| `--url URL` | Ollama base URL; loopback only (D7) |
| `--max-num-ctx N` | hard ceiling on any `num_ctx` sent (D8 safety clamp; default 65536) |
| `--min-context N` | R9 architectural context floor |
| `--connect-timeout N` / `--metadata-timeout N` / `--chat-timeout N` | seconds |
| `--warmup` / `--no-warmup` | R9 inference check; off by default |
| `--tools` / `--no-tools` | R9 tools-capability gate; on by default |
| `--shell` / `--no-shell` | register the ninth tool, kernel-confined (D10); off by default, and refused at startup unless this machine's confinement probe reports Enforced |
| `--shell-timeout N` | seconds; per-call wall-clock bound for shell (default 60) |
| `--` | end of flags; lets a path or instruction begin with a dash |

## `agent` only

| flag | meaning |
|---|---|
| `--expect KIND:PATH` | a post-condition, repeatable; grammar in [chapter 14](./14-expectations.md) |
| `--max-turns N` | turn bound for one run (default 12) |
| `--budget N` | wall-clock seconds for one run, checked between turns (R8; default 300, bounded 1–86400) |
| `--no-verify` | skip the per-turn hash diff of the tree (on by default) |
| `--attempts N` | total attempts at the stated post-conditions, each a fresh session re-invoked with the one concrete remaining failure (default 3; `1` disables re-invocation; without `--expect` there is nothing to retry and one attempt runs) |
| `--judge-model NAME` | who decides `satisfies:` expectations (default: the working model, in a fresh session that never sees the transcript) |
| `--unjudged N` | declare N stated requirements that deliberately cannot be decided from the tree (reply-marker requirements); the verdict reports them as unjudged instead of letting them vanish |

## `agent` and `undo`

| flag | meaning |
|---|---|
| `--backups DIR` | where undo data goes; must be outside `--root`. Defaults to `.hermit-backups-<root name>` beside the root |
| `--keep-hours N` | retention: generations older than N hours are pruned at agent start and by `undo --prune` (default 72) |

## `undo` only

| flag | meaning |
|---|---|
| *(none)* | list the generations; nothing changes |
| `--restore N` | write generation N's preserved bytes back; the file's current bytes are preserved first, so a restore is itself undoable |
| `--last` | restore the newest generation |
| `--prune` | apply retention now, without running a job |

## Environment variables

`HERMIT_CONFIG` (absolute path), `HERMIT_SANDBOX_ROOT` (absolute path), `HERMIT_MODEL`,
`HERMIT_OLLAMA_URL`, `HERMIT_MAX_NUM_CTX`.

The two path variables are rejected if relative: a relative path in the environment has no
honest anchor — it was not typed at this launch and it does not live in a file — so resolving
it against anything would be a guess ([chapter 16](./16-configuration.md)).

## Exit codes

| exit | meaning |
|---|---|
| `0` | run finished; nothing stated is unmet |
| `1` | run did not finish — a bound, a refusal, infrastructure |
| `2` | usage or configuration error — bad flags, an unloadable config, a refused `--backups`, an unknown judge model |
| `3` | something stated with `--expect` is measurably undone |
