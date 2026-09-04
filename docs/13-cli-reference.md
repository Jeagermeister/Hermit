# 13. CLI reference

One binary, eight subcommands. This chapter matches the binary as built 2026-09-04;
`hermit --help` is always the tie-breaker, and `hermit config` shows every setting actually in
force and where it came from.

```
hermit resolve   --root DIR <path>...
hermit preflight --model NAME
hermit session   --model NAME
hermit agent     --root DIR --model NAME <instruction>
hermit undo      --root DIR
hermit usage     --root DIR
hermit mcp       --root DIR
hermit config
```

## The subcommands

### `hermit agent` — the product

One instruction, one root, one model. Offers the eight structural tools (plus `delete` with
`--delete`, `shell` with `--shell`), drives the model through a bounded session, prints one trace line per turn and
per call, a hash-verified changeset after every turn, a verdict for anything stated with
`--expect`, and a summary naming which bound stopped the run.

### `hermit undo` — the recovery door

Lists the backup store's generations by default; mutates only by explicit flag.
[Chapter 17](./17-undo-and-backups.md).

### `hermit usage` — estimated Cloud spend, per root

Reads `.hermit-usage-<root name>/usage.jsonl` (D18), groups by model, and prices each group
against a rate table kept in this binary (a hand-synced copy of
[docs/31](./31-ollama-cloud-economics.md)'s). Scoped to one root at a time, the same as `undo`
— a total across every root you've run Hermit against means running this once per root and
adding the numbers yourself. Estimate only: Ollama Cloud has no usage API
(`ollama/ollama#15132`, `#15663`), and the estimate assumes no cache discount, since Cloud's is
not reliably applied (`ollama/ollama#16714`). Cross-check `ollama.com/settings` before trusting
it for a budget decision. A model with no rate-card entry is reported, not silently dropped.

### `hermit mcp` — the programmatic front door

An MCP server over stdio: reads one JSON-RPC message per line on stdin, writes one back on
stdout, until the client closes its side. No listener, no port, no auth, no TLS ([D7](../DECISIONS.md)).
Offers the same eight structural tools `agent` does (plus `delete` with `--delete`, and `shell`
with `--shell`, gated identically — refused at startup unless this machine's confinement probe
reports Enforced), published from the
same descriptor list that renders `agent`'s Ollama tool definitions, so there is no second schema
to drift. Takes no positional arguments and no `--model`/`--url` (tool-serving needs no model or
Ollama client). `--backups DIR` works the same as below; retention is fixed at the CLI's own
72-hour default and this surface does not expose `--keep-hours`. [Chapter 20](./20-mcp-and-kiro.md).

### `hermit preflight` — the model gates

Runs R9's checks against the live daemon: architectural context floor, `tools` capability,
optional inference warmup. Fails closed — "could not determine" is a failure, not a pass.

### `hermit session` — context accounting, visible

Exists because the token estimate is the one thing no unit test can settle. Each turn prints
what the session expected against what Ollama actually evaluated. Run it with
`--max-num-ctx 2048` to watch history being *trimmed* deliberately — the alternative, left
to the server, is a silent discard of nearly everything
([troubleshooting](./18-troubleshooting.md) § the context cliff). `session` has no tree to
verify, so it never reaches the reconstruction path `agent` uses; the trim is the whole of
what this command demonstrates.

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
| `--allow-cloud` / `--no-allow-cloud` | permit a Cloud-tagged model (D18); off by default, and required before one is accepted |
| `--url URL` | Ollama base URL; loopback only (D7) |
| `--max-num-ctx N` | hard ceiling on any `num_ctx` sent (D8 safety clamp; default 65536) |
| `--min-context N` | R9 architectural context floor |
| `--connect-timeout N` / `--metadata-timeout N` / `--chat-timeout N` | seconds |
| `--warmup` / `--no-warmup` | R9 inference check; off by default |
| `--tools` / `--no-tools` | R9 tools-capability gate; on by default |
| `--shell` / `--no-shell` | register the shell tool, kernel-confined (D10); off by default, and refused at startup unless this machine's confinement probe reports Enforced |
| `--shell-timeout N` | seconds; per-call wall-clock bound for shell (default 60) |
| `--delete` / `--no-delete` | register the delete tool ([D19](../DECISIONS.md)): one file the model has already read or listed, its bytes preserved before the name goes; off by default, and independent of `--shell` — with shell on and delete off, `agent` and `mcp` print a note that `rm` under shell is neither gated nor backed up |
| `--` | end of flags; lets a path or instruction begin with a dash |

## `agent` only

| flag | meaning |
|---|---|
| `--expect KIND:PATH` | a post-condition, repeatable; grammar in [chapter 14](./14-expectations.md) |
| `--max-turns N` | turn bound for one run (default 12) |
| `--budget N` | wall-clock seconds for one run, checked between turns (R8; default 300, bounded 1–86400) |
| `--no-verify` | skip the per-turn hash diff of the tree (on by default) |
| `--compact-at PCT` | rebuild the window from the tree at this percentage of the prompt budget (default 80). `0` falls back to trimming, which drops history without telling the model |
| `--read-record` | list the paths already named in a call in a rebuilt window. Off by default — it did not stop a model re-reading them in the one paired run so far, and on a tight window it can stop rebuilds firing at all |
| `--attempts N` | total attempts at the stated post-conditions, each a fresh session re-invoked with the one concrete remaining failure (default 3; `1` disables re-invocation; without `--expect` there is nothing to retry and one attempt runs) |
| `--judge-model NAME` | who decides `satisfies:` expectations (default: the working model, in a fresh session that never sees the transcript) |
| `--unjudged N` | declare N stated requirements that deliberately cannot be decided from the tree (reply-marker requirements); the verdict reports them as unjudged instead of letting them vanish |

## `agent`, `undo` and `mcp`

| flag | meaning |
|---|---|
| `--backups DIR` | where undo data goes; must be outside `--root`. Defaults to `.hermit-backups-<root name>` beside the root |
| `--keep-hours N` | retention: generations older than N hours are pruned at agent start and by `undo --prune` (default 72). `agent`/`undo` only — `mcp` prunes at its own fixed 72h default and does not expose this flag |

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
