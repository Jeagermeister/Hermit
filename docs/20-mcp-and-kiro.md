# 20. MCP and Kiro

**Status (2026-08-28): built.** `hermit mcp --root DIR` — a subcommand beside `agent`/`undo`,
not a second binary — reads JSON-RPC on stdin and writes it to stdout ([ROUTING.md](../ROUTING.md)
§12 step 6, done). The shape below is no longer a design; it is what shipped.

## The shape

A local MCP **server over stdio**: a subprocess speaking JSON-RPC on stdin/stdout. No
listener, no port, no auth, no TLS — nothing reachable from off the machine
([D7](../DECISIONS.md)). The caller launches the binary and gets the same tool surface the
CLI drives, published from the same descriptor list that renders Ollama's tool definitions,
so there is no second schema to drift.

This is also where the native-binary bet pays hardest: a caller invoking a tool pays process
startup on every call, and bounded sessions mean many calls — ~10 ms for a static binary
against 1–3 s for an interpreter.

## The pitch, which is the ecosystem's warning inverted

Kiro's own documentation states the default plainly: stdio MCP servers "execute arbitrary
commands inside your environment with the same privileges and access as the agent itself,
including access to your source code, environment variables, secrets, and any credentials
available in the session." Ambient full authority is the norm.

Hermit is the MCP server that **reduces** authority instead of inheriting it: containment by
construction for every path-taking tool, kernel confinement for the one tool that cannot be
contained by type, verification on every mutation, and a backup the caller's model cannot
reach. The argument is stronger for a programmatic caller than for a human one — a human
notices when a tool wanders; a model does not.

## Kiro, concretely

Kiro is the first named programmatic caller ([ROUTING.md](../ROUTING.md) §8), and it consumes
exactly the transport specified: a `command` entry under `mcpServers` in
`.kiro/settings/mcp.json` (workspace) or `~/.kiro/settings/mcp.json` (user), hot-reloaded on
save. A Hermit binary is a `command` entry and nothing more. A Kiro "Power" is the wrong
vehicle, and that is settled — there is no third-party authoring path, and the problem Powers
solve (OAuth) is one this project forecloses by design.

Kiro's `autoApprove` list maps onto the tool surface for free: `read`, `list`, `find`,
`grep`, `hash` are natural entries — they observe; `write`, `edit`, `move` are not — they
mutate. That is caller policy, and Hermit will not ship a config that pre-approves a mutating
tool.

## What differs from the CLI surface

**`shell` is off the MCP surface by default**, and enabling it there is an explicit marked
configuration act. The surviving reason is egress: Landlock bounds what a confined shell can
*touch*, not what it can *send*, and the threat model for a programmatic frontend is
prompt-injection-influenced input — injection plus unrestricted egress is exfiltration. If
shell ever goes on this surface by default, the network rules get governed first; that
ordering is a recorded decision, not a preference ([ROUTING.md](../ROUTING.md) §8).

## The platform consequence

Kiro CLI runs natively on Windows, where a Linux ELF binary is not spawnable. The two paths —
`wsl.exe` wrapping (with a path-translation layer sitting directly on the sandbox resolver)
or a native Windows build (a separate package, never an `#ifdef`) — are both recorded and
neither is near ([SCOPE.md](../SCOPE.md) § Platforms). Linux is the product; the Windows
question is a consequence of Kiro's platform, not generic portability anxiety.

## What ships with it

The economics experiment (E2) — the same agentic harness doing the same tasks with its native
filesystem tools versus the Hermit MCP surface as its only hands, metered tokens per completed
task — was frozen ahead of this frontend and named it as its one prerequisite
([chapter 30](./30-benchmarks.md)). Now that this has shipped, running E2's collection is the
first thing to do with it.
