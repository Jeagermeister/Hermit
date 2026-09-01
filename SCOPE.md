# Scope

What gets built, what gets read, and what gets ignored — with the reasoning, so it can be
argued with later rather than rediscovered.

Companion to [REQUIREMENTS.md](./REQUIREMENTS.md), which says *why* from measured evidence.
The machine-readable version is [`parity.tsv`](./parity.tsv); this file is the argument behind it.

## The number that sets the frame

Upstream is ~870k lines of non-test Python. Marking `hermes_cli/`, `gateway/`, `plugins/`,
`skills/`, `cron/`, `acp_adapter/` and `tui_gateway/` out of scope was necessary but nowhere near
sufficient — it still leaves **266k lines** across `agent/` (136,525), `tools/` (128,965) and
`providers/` (452).

Going module by module cuts that to about **38k lines to learn from**, and only ~25k of it is
code we would actually reimplement.

| | files | LOC | share of upstream |
|---|---|---|---|
| upstream, non-test | 1,135 | ~867,000 | 100% |
| `agent/` + `tools/` + `providers/` | 326 | 266,000 | 31% |
| **modules actually in scope** | **12** | **~38,000** | **4%** |

## Keep — `tools/` (4 of 136 files, ~12.7k lines)

| module | LOC | why |
|---|---|---|
| `file_operations.py` | 3,227 | read / write / edit — the core of "bounded filesystem work" |
| `file_tools.py` | 2,673 | the tool-facing surface over the above |
| `terminal_tool.py` | 3,821 | shell. **Not optional** — see below |
| `process_registry.py` | 2,987 | process lifecycle; needed for timeouts and R8 wall-clock bounds |

### Why `terminal` survives, despite shell being the dangerous path

The instinct is to drop shell and expose only structured file tools, on the theory that shell is
where destruction happens. **The measurements say otherwise, twice over.**

`qwen3.5:4b` performed every filesystem operation through `terminal` — `cat`, `printf >>` —
and never touched a file tool. `llama3.2:3b` used `write_file`. **Tool preference varies by
model and is not under our control.** Remove shell and the qwen-shaped models fail; ship only
shell and the llama-shaped ones do.

And shell is not where the damage came from: both destructive `05_copy` incidents — the ones
that overwrote `config.ini` with invented content — came through **file tools**, not shell. So
did every escape into the repository root.

The conclusion is that *which tool* is the wrong lever. **Verification belongs at the filesystem
layer, not the tool layer**: snapshot before the turn (R4), hash-diff after (R3), and decide
completion from that rather than from the model's claim (R6) — regardless of which tool the
model reached for. That is what makes this question mostly disappear — which is fortunate,
because it is not a question that can be won by picking a side.

## Keep — `agent/` (7 of 188 files, ~25k lines)

| module | LOC | why |
|---|---|---|
| `conversation_loop.py` | 7,757 | the core loop |
| `chat_completion_helpers.py` | 4,631 | request/response handling |
| `model_metadata.py` | 3,367 | context limits and capabilities — this is R9 preflight |
| `agent_init.py` | 2,858 | startup and wiring |
| `tool_executor.py` | 2,429 | dispatch |
| `prompt_builder.py` | 2,380 | system prompt assembly |
| `error_classifier.py` | 1,842 | feeds the supervisor's retry decisions |

## Reference only — read, do not port

| module | LOC | why not ported |
|---|---|---|
| `context_compressor.py` | 7,386 | see below |
| `conversation_compression.py` | 4,133 | see below |

These 11.5k lines exist because upstream runs long, open-ended sessions against cloud models.
**Long sessions are real here too; summarisation is not the fix.** The tournaments concluded
"break larger work into fresh sessions," and the supervisor re-invokes with *one concrete
remaining failure* — so accumulation *across* sessions was never the problem these files solve.
Within one session the window still fills, and
[D17](./DECISIONS.md#d17--compaction-rebuilds-the-window-from-the-tree-it-never-summarizes)
answers that without porting a compressor: the window is rebuilt from the filesystem — task
kept verbatim, changed paths re-read, the model's own narration discarded, never a summary of
what it claims happened. Built at a few hundred lines against upstream's 11.5k, and the size
gap is the point, not an accident: a summary is exactly the unverified completion claim R6
exists to distrust.

The one case D17 does not reach is **a single file larger than the context window.** That is
chunk-and-summarise, worth reading upstream for ideas and not worth porting on its own —
hundreds of lines, not eleven thousand.

Recorded as `REFERENCE` in `parity.tsv` rather than `OUT_OF_SCOPE`, because if upstream learns
something here it is worth knowing.

## Discard — everything else in `agent/` and `tools/`

The single largest win is **`auxiliary_client.py`, 10,306 lines** — the biggest file in `agent/`,
and almost entirely multi-provider client plumbing. Hermit talks to one OpenAI-compatible
endpoint. Note `providers/` is only **452 lines**, which tells you where upstream's provider
complexity actually lives.

Discarded for the same reason:

| module | LOC | what it is |
|---|---|---|
| `auxiliary_client.py` | 10,306 | multi-provider client |
| `mcp_tool.py` | 7,752 | MCP integration |
| `browser_tool.py` | 5,140 | browser automation |
| `skills_hub.py` | 4,620 | `skills/` is already out of scope |
| `approval.py` | 4,618 | human-in-the-loop approval — see below |
| `tts_tool.py` / `transcription_tools.py` / `voice_mode.py` | 9,897 | audio |
| `delegate_tool.py` | 4,356 | sub-agent delegation |
| `anthropic_adapter` / `bedrock_adapter` / `codex_*` | 7,997 | cloud provider adapters |
| `credential_pool.py` | 3,178 | key rotation — meaningless against local Ollama |
| `iron_proxy.py` | 2,494 | egress firewall for cloud credentials |
| `computer_use/` | 7,146 | screen control |
| `moa_loop.py` | 2,384 | mixture of agents |
| `display.py` | 1,580 | TUI rendering — this runs headless |

**`approval.py` is a deliberate discard-and-replace, not a plain discard.** It is built for a
human answering prompts, and the supervisor runs unattended — but R4 (backup before mutate)
still needs *something* deciding what a turn is allowed to do. That is a small policy engine
written fresh, not 4.6k lines adapted.

## Build new — no upstream equivalent these runs revealed

This is the actual product, and none of it is a port:

| requirement | what it is |
|---|---|
| **R3** | verify by content hash, never by existence |
| **R4** | backup before mutate; undo as a first-class operation |
| **R5** | read back after every write — note its classification moved; see [REQUIREMENTS.md](./REQUIREMENTS.md) R5 |
| **R6** | poll filesystem state; never trust a completion claim |
| **R7** | re-invoke with one concrete remaining failure |

Hermes Agent did not do any of these in any observed run — inferred from behaviour, not from
auditing upstream's code, every module of which `parity.tsv` still lists as `NOT_PORTED`. That
is precisely why `bench/fsops` could observe the failures it did — a model replying `DONE` on an untouched tree, an `05_copy` that destroyed the
original while "the file still exists" passed, a run that finished its work and then hung.

**Re-tested against four unrelated harnesses, 2026-08-15.** The claim above is scoped to
upstream, which is a narrow test for a load-bearing statement — so it was re-run against
DeepSeek Harness, Prime Agent and the two Recursive Language Model repos, all actively developed
this month. R3, R4 and R6 hold: every `sha256` in Prime Agent hashes socket paths, credentials
and session leases rather than file content; neither harness snapshots before mutating; nothing
polls the filesystem to decide completion.

**R5's wording does not survive intact, and the neighbour is worth knowing.** DeepSeek Harness's
`fs-observation-policy` is 130 lines that refuse an edit to a file the model has not read, and a
write to a file it has not observed. It is not R5 — it *prevents* the stale write where R5
*confirms* the fresh one — but "no upstream equivalent" reads as "nobody does anything in this
area", which is now false. What that policy became here is the staleness guard on `write` and
`edit`, in
[ROUTING.md](./ROUTING.md) §4, recorded there as a second layer under §6 rather than as a
substitute for it.

**The rewrite is a thin, correct agent loop plus a supervisor upstream never had.** The porting
is supporting cast.

## Consequence for the parity ledger

`parity.tsv` previously tracked whole subsystems, with `agent/` and `tools/` marked in scope. At
that granularity `tools/parity` would report drift dominated by `browser_tool.py`, `tts_tool.py`
and `mcp_tool.py` — 129k lines that will never be written — which destroys the signal the ledger
exists to provide.

Rows are now **module-granular for in-scope code**, and whole-subsystem for what is ignored.
Anything under `agent/` or `tools/` not listed as a row is implicitly out of scope. This is the
same argument [UPSTREAM-PARITY.md](./UPSTREAM-PARITY.md) makes for tag-granularity over
commit-granularity: pick the
unit that keeps the report answerable.

---

## Platforms — Linux is the product, Windows is a separate package

**Decided 2026-08-16.** Hermit targets **Linux**. That is not a placeholder for "portable
eventually": it is the substrate [D6](./DECISIONS.md#d6--the-sandbox-is-a-capability-type-and-resolution-is-posix-order),
[D10](./DECISIONS.md#d10--kernel-confinement-for-shell-landlock-vendored-one-writable-root) and R1
were designed against, and the only one where the guarantees have been measured.

**If Windows happens, it is a separate package and a separate release on Gitea — never an
`#ifdef` in this tree.** The reason is not tidiness. It is that D6 does not *port*; it gets
**rewritten**, because Win32 path parsing has no POSIX analogue and every difference is a
containment question:

8.3 short names (`PROGRA~1`) · alternate data streams (`file.txt:evil`) · reserved device names
(`CON`, `NUL`, `COM1`) · silently stripped trailing dots and spaces · `\\?\` bypassing
normalisation entirely · UNC and drive-relative paths (`C:foo`) · junctions behaving unlike
symlinks · case-insensitivity by default.

Each is a distinct way for *the path that was resolved* to differ from *the path that is opened*,
which is precisely the property D6 exists to guarantee. A speculative Windows D6 written before
the Linux one has proved its value would be wrong, and wrong in a way that is expensive to
discover.

**What appears to port cleanly**, recorded so the eventual effort is not re-derived from scratch.
⚠️ **All of this is unverified — neither this laptop nor Kitchen runs Windows, and nothing below
has been compiled or executed.** It is a research note, not a measurement, and is marked as such
deliberately in the manner this repo already distinguishes the two:

| Need | Apparent Windows answer |
|---|---|
| `realpath(3)` | `GetFinalPathNameByHandleW` — resolves from an open handle, arguably *stronger* than realpath |
| `openat` (handle-relative) | `NtCreateFile` with `RootDirectory` in `OBJECT_ATTRIBUTES` (NT native, not Win32) |
| `O_NOFOLLOW` | `FILE_FLAG_OPEN_REPARSE_POINT` |
| `st_dev` / `st_ino` | `GetFileInformationByHandleEx` → `FILE_ID_INFO` (128-bit; ReFS needs it) |
| `ctime` staleness guard | NTFS **does** carry `ChangeTime`, via `FileBasicInfo` — [ROUTING.md](./ROUTING.md) §4's guard survives |

**Landlock has no Windows equivalent**, and that degrades correctly rather than breaking:
ROUTING.md §8 gates on the confinement probe rather than the platform, so absence is *reported*
and `shell` stays off the MCP surface — which is already the default. Nothing else in the tool
surface changes.

**macOS is not a target.** Not because it is hard — it is the easy one — but because the
workplace case that raised the question has no Mac dev teams, and an untargeted platform with no
user is scope without a beneficiary.

**The boundary is made explicit rather than assumed**, which is
[D11](./DECISIONS.md#d11--the-substrate-is-probed-not-assumed)'s job: WSL becomes usable on its
Linux side and explicitly refused on `/mnt/c`, instead of quietly offering guarantees that do not
hold there.

**Why this ordering.** This is first and foremost a personal tool, built on this machine, for
this machine's evidence. A workplace is a possible beneficiary, not a requirement — and building
portability before value is backwards.
