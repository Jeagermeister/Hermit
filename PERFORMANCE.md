# Performance and size: what was reviewed, and what's actually worth doing

A full "faster, more efficient, smaller" pass over the codebase, run 2026-08-26 at the
maintainer's request, with one constraint stated up front: **only report what's backed by a
real measurement, and say plainly when something isn't worth doing.** That's the same bar this
project already holds security work to — DECISIONS.md rejects `st_dev` mount-point checking as
"a check with no reachable attacker behind it"; the standard here is "a change with no
measured win behind it."

Two independent passes were run — one over the build configuration and binary size, one over
runtime hot paths — followed by a direct re-verification of the one finding that turned out to
matter, before writing it down here. If this file and the code ever disagree, rebuild and
re-measure; these are point-in-time numbers on one machine (gcc 12+/clang 16+ floor, see
CMakeLists.txt), not guarantees.

## The one thing worth doing: turn on LTO

`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`, added to `CMakeLists.txt`. Measured twice
(independently by the audit and again directly against the real tree), same result both times:

| | unstripped | stripped |
|---|---|---|
| without LTO (current `build/hermit`) | 37,616,808 B | 1,667,280 B |
| with LTO (scratch build, same flags otherwise) | 23,751,272 B | 974,736 B |
| change | **−37%** | **−42%** |

Build time was *not* worse — it was faster on this machine: ~8.2s wall to build the `hermit`
target with LTO on, versus ~14.9s without. That's link-time whole-program-analysis
parallelizing well on a 24-core box for a project this size (~20 translation units); don't
assume that holds on a smaller machine, and re-time it before treating "LTO is free" as settled
across every machine this is built on (MSI laptop, kitchen-desktop).

**Not yet applied.** It's a two-line CMakeLists.txt change with no observed downside here, but
two things are genuinely the maintainer's call, not a default worth setting unilaterally:

- Whether the size win is worth caring about at all for a tool that isn't distributed anywhere
  — it changes nothing about correctness or the local dev loop.
- Whether to make it the *default* (risking a slower or misbehaving link on a machine that
  hasn't been measured — the MSI laptop is unverified) or an opt-in `HERMIT_LTO` cache option,
  same shape as `HERMIT_SANITIZE`.

The runtime win is genuinely unmeasured, and unlikely to matter: the `.text` shrink is
cross-archive dead-code and duplicate-inlining removal (4 static archives linked into one
binary), not a hot-loop speedup, on a tool that runs a handful of tool calls per model turn.
Treat this as a size change with a compile-time bonus, not a speed fix.

## Everything else: reviewed, nothing to do

**Why the gcc binary is 36M and the clang one is 18M.** Not a bug or a misconfiguration —
both trees use identical `-O2 -g -DNDEBUG` (RelWithDebInfo). `.text` is 61% bigger under gcc
(1,411,856 B vs 874,471 B, real codegen difference), but that's the minority of the gap.
>97% of the ~19.5M difference is DWARF debug-info verbosity — gcc's `.debug_loclists` alone is
7.87M against clang's 1.54M for the same optimized, heavily-inlined code. `strip` erases the
question entirely (95.6% and 93.9% cut respectively, converging to a ~1.5x ratio that matches
the `.text` gap). `-g` is exactly what makes local debugging work, and disk space for a dev
binary is free — no case was found for changing the default RelWithDebInfo build.

**Runtime hot paths — no meaningful issue anywhere.** Checked against this tool's actual shape
(a low-QPS local CLI driving 6–107s-per-turn inference, not a service under load):

- `fsio.cpp`'s `openat`-per-component walk (added closing D7's gate) costs a few extra syscalls
  per open on a deep path — microseconds, and the entire point of the added calls is the
  symlink-swap refusal they buy. Not reverting security work to save microseconds against
  seconds of inference.
- `supervisor/verify.cpp`'s `TreeVerifier` is O(entries) per turn by design — one `fstatat`
  each, because that stat *is* the mechanism proving nothing changed — with hash bytes read
  only for entries that actually moved (D13's tuple-reuse hashing, already measured and
  tested). Not O(n²); nothing to fix.
- `supervisor/loop.cpp` builds the tool-call schema once per session and reuses it every turn,
  not per call. The chat payload is held in three representations per turn (acknowledged in a
  code comment at `ollama/client.cpp:341`) — real, but sub-millisecond memcpy-class work against
  a multi-second inference call, four to five orders of magnitude off from mattering.
- `core/observed.cpp`'s `ObservedState` is an `unordered_map` keyed by relative path — O(1)
  already, and a session touches tens of files, not thousands.
- `core/backup.cpp` and `supervisor/undo.cpp` both already use the same
  reflink-then-kernel-copy-then-byte-loop fallback ladder (`copy_file_range`); no naive
  read-into-memory-then-write path exists on either side.
- `nlohmann::json` usage (`ollama/client.cpp`, `supervisor/wire.cpp`, `app/config.cpp`) is
  already the cheap idiomatic path — manual DOM access, no ADL reflection magic, no
  serialize-then-reparse round-trips. Payloads are small (one config file, one chat turn) and
  the library's known compile-time cost is concentrated in exactly one translation unit
  (`ollama/client.cpp`, the only file that includes both it and `httplib.h`, ~14s of a ~15s
  total clean build) rather than spread everywhere. Swapping it for a leaner parser would only
  claw back part of that one file's compile time, and isn't worth the migration risk across
  every call site given [D3](./DECISIONS.md)'s pinned-dependency stance.

**Confirmed already correct, no change possible or needed:**

- libstdc++/libgcc/libm/libc all link dynamically already (`ldd` confirmed) — contributes
  nothing to the 36M and is the right default (smaller binary, gets the distro's security
  patches).
- The sanitizer INTERFACE target only applies flags under `HERMIT_SANITIZE` (default off);
  confirmed no leakage into the normal build.
- `HERMIT_BUILD_TESTS` (default on) builds a separate `hermit_tests` binary; doesn't touch
  `hermit`'s size or link line.

## What this file is not

Not a commitment to do the LTO change, and not a scheduled phase item — see the linked entry
in [ROADMAP.md](./ROADMAP.md) under "Open questions." If the maintainer decides it's worth it,
applying it is small: add the CMake option, rebuild all three trees (`build`, `build-asan`,
`build-clang`), confirm `ctest` still passes in each (LTO can occasionally surface an ODR
violation or missing-symbol bug that a non-LTO build hid — that's the actual risk, not size or
speed), and record the decision in DECISIONS.md if it's kept.
