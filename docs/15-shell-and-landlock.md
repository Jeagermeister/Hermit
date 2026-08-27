# 15. Shell and Landlock

`--shell` adds a ninth tool to the menu: the model may run an opaque shell command. It is off
by default, kernel-confined when on, and the only tool that gets a whole chapter of caveats —
deliberately, because it is the one tool that cannot be made safe by construction, only
contained by the kernel.

## Why shell exists at all

The instinct is to drop shell and expose only the structured file tools. The measurements say
otherwise, twice over ([SCOPE.md](../SCOPE.md) § "Why terminal survives"): tool preference
varies by model and is not under your control — one measured model performed every filesystem
operation through the shell and never touched a file tool — and shell is *not* where the
recorded damage came from. Both destructive incidents in the evidence base, and every sandbox
escape, came through file tools. Removing shell breaks the shell-shaped models and protects
against the wrong thing. The benchmark series later put numbers next to a no-shell surface at
repository scale — as an inference from the surface difference, the results say plainly, not
a transcript-level observation — and found the cost model-dependent: models that think in
shell pay it, models that drive file tools directly pay nothing. The roadmap reads that as
sharpening the argument rather than weakening it
([ROADMAP.md](../ROADMAP.md) § "The measured levers").

## What turning it on does

```bash
hermit agent --root ~/scratch --model qwen3.5:9b --shell --shell-timeout 30 \
  'Count the lines in every .md file in this folder and write the totals to counts.txt.'
```

- The command runs in a forked child under a **Landlock ruleset** installed immediately before
  exec ([D10](../DECISIONS.md)): exactly one writable directory — your sandbox root — plus
  fixed read-only grants (`/usr`, `/etc`, `/proc`, `/dev/urandom`) and `/dev/null`. Writes
  anywhere else are denied by the kernel, whatever the command string says.
- It is bounded by `--shell-timeout` (default 60 s) and killed **as a whole process group**,
  so a command that backgrounds a grandchild does not outlive its timeout.
- stdout and stderr are captured and truncation-flagged rather than silently cut off.
- Temp space is inside the root — a shell that writes `/tmp` will be denied, and that is the
  one-writable-root rule working, not a bug: two writable roots would let a confined process
  hardlink or rename between them.

**Hermit refuses to start rather than run unconfined.** Before registering the tool it probes
this machine's actual enforcement — attempting a denied write and requiring `EACCES`, not
trusting a version string or a profile-accepted result ([D10](../DECISIONS.md)) — the same
argument R9's preflight already makes about asking the live daemon rather than inferring from
a version. A kernel without Landlock, or with it compiled out, gets a refusal at startup,
never a warning-and-continue.

## What confinement costs you

Registering `shell` switches the per-turn tree diff to **rehash every file unconditionally**,
abandoning the identity-tuple shortcut that normally makes a turn cost only the bytes that
moved. The reason: a shell can hold a `MAP_SHARED` mapping and change a file's bytes in ways
the timestamp/size tuple may not reflect ([D13](../DECISIONS.md)'s amendment). On a large
tree that makes verified turns slower — by how much is deliberately unmeasured so far;
nothing has yet run the walk against a large repository. It is the honest price of an opaque
tool, paid in wall clock rather than in trust.

## What confinement does not close

Stated here because the mechanism's own reputation invites the opposite reading
([D10](../DECISIONS.md) carries the full list with measurements):

- **Egress is ungoverned.** A confined process still reaches the network — DNS and TLS
  working — and pathname unix sockets. Containment bounds what shell can *touch*, not what it
  can *send*. This is a large part of why shell stays off the programmatic (MCP) surface by
  default: prompt-injection-influenced input plus unrestricted egress is exfiltration
  ([ROUTING.md](../ROUTING.md) §8).
- **In-root redirection is permitted.** Landlock catches escapes *out of* the root; a symlink
  trick that redirects a write from one in-root file to another is invisible to it. That
  failure mode is closed by a different mechanism — the per-component `openat(O_NOFOLLOW)`
  walk in the file tools — which is exactly why the two exist together.
- **A hardlink planted inside the root before the sandbox starts** remains reachable and
  writable through its in-root name. Creating new ones is blocked; pre-existing ones are
  accepted explicitly, with the threat-model reasoning on record.
- **Not everything is hooked.** `chmod`, `chown`, `utimes` and `stat` are outside Landlock's
  governance — a confined process can change a file's mode and mtime without holding any
  grant. This is why Hermit's staleness guard keys on `ctime`, which cannot be set directly.

## The operator's summary

Turn `--shell` on when the task genuinely wants command composition — counting, scripting,
transforms that would take many tool round-trips — and the model you run is one that reaches
for shell. Leave it off for anything the eight structural tools cover: every structural call
is verified per call, and shell's work is only ever verified after the fact, by the per-turn
diff. Either way, the per-turn hash diff and your stated expectations are the layer that
holds — which tool the model used was never the lever.
