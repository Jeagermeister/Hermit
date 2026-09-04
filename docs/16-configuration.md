# 16. Configuration

Settings come from four places, overlaid field by field in increasing precedence:

**defaults < `--config` file < environment < flags**

`hermit config` prints the resolved set with the origin of each value, and it is a product
surface, not a debugging aid: anything unusual in force — a raised clamp, a waived tools gate,
shell or delete enabled, a non-loopback URL — prints as a marked line, so nothing unusual can be in
force silently.

## Two things Hermit will never do

**No default sandbox root, and no implicit config-file search.** Both would be an *inferred*
root — the working directory, `$HOME`, a git root found by walking upward — and an inferred
root is R1's original bug wearing a convenience costume. A config file is read when it is
named and never otherwise; `--root` is always explicit.

## Relative paths have three different honest answers

Where a relative path is anchored depends on where it was written, and the rules were chosen
so each source means one thing regardless of where the binary launched
([ROADMAP.md](../ROADMAP.md), Phase 1 · Config):

| source | a relative path is | why |
|---|---|---|
| config file | anchored to **the file's own directory** | one file must mean one thing wherever it is read from |
| flag | anchored to **the working directory** | it was typed at launch, where that directory is what you are looking at |
| environment | **rejected** | it has neither anchor, so resolving it would be a guess |

## The config file

JSON, named explicitly via `--config` or `HERMIT_CONFIG` (absolute path). **Unknown keys are
errors** — a typo'd setting that was silently dropped would leave a safety default in force
while you believed you had changed it.

```json
{
  "model": "qwen3.5:9b",
  "allow_cloud": false,
  "sandbox_root": "./notes",
  "ollama": {
    "base_url": "http://127.0.0.1:11434",
    "max_num_ctx": 65536,
    "connect_timeout_s": 5,
    "metadata_timeout_s": 30,
    "chat_timeout_s": 600
  },
  "preflight": {
    "minimum_context": 65536,
    "require_tools": true,
    "warmup": false
  },
  "shell": {
    "enabled": false,
    "timeout_s": 60
  },
  "delete": {
    "enabled": false
  },
  "expectations": [
    "exists:falcon-index.md",
    { "kind": "preserved", "path": "notes.txt", "other": "notes.txt" },
    { "kind": "satisfies", "path": "report.md", "criterion": "a one-line summary" }
  ]
}
```

Notes on the shape:

- The `expectations` array accepts both the compact `kind:path` strings and object form; the
  object form is how you name a path containing `=`
  ([chapter 14](./14-expectations.md)).
- `sandbox_root` here is relative to the file's own directory, per the table above.
- `allow_cloud` permits a Cloud-tagged `model` to reach Ollama Cloud through the local
  daemon's own proxying (D18). Off by default here too — setting it in a file is legitimate
  for a fixed, deliberate setup, but the flag exists for the common case of deciding this
  per invocation rather than leaving it on.
- Repeated `--expect` flags land in this same array through one parser — the command line and
  the file cannot disagree about the grammar.

The example above is illustrative, not exhaustive; `hermit config` is the authoritative list
of every setting and its current value.

## The environment variables

`HERMIT_CONFIG`, `HERMIT_SANDBOX_ROOT` (both must be absolute), `HERMIT_MODEL`,
`HERMIT_OLLAMA_URL`, `HERMIT_MAX_NUM_CTX`.

One behaviour worth knowing: an exported `HERMIT_OLLAMA_URL` is validated only by the
subcommands that actually open a socket — `hermit resolve` and `hermit config` still work with
a bad one in the environment, and `config` marks the offending value instead of refusing to
print, since printing the wrong value is its whole job.

## The safety settings, and why they are marked

Two numbers are doing safety work and deserve respect before raising:

- **`ollama.max_num_ctx` (default 65536)** — a hard ceiling on any context size sent. Ollama
  performs no admission control on `num_ctx`: an oversized request is not rejected or reduced,
  and on one recorded occasion it hard-froze the machine — no OOM kill, no GPU reset, the
  kernel simply stopped ([D8](../DECISIONS.md)). The clamp guarantees "at most this much",
  never "fits your card" — 65536 is a value known to work on a 16 GB card, not derived from
  yours.
- **`preflight.minimum_context` (default 65536)** — the R9 floor. Waivable, like the
  `require_tools` gate, and a waiver prints as a marked line.

The URL is **loopback only**, refused at configuration time rather than at connect
([D7](../DECISIONS.md)) — that rule is what keeps sandbox file contents on the machine, so
there is exactly one place that decides it.
