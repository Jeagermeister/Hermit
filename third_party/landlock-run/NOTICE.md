# Vendored: landlock-run

Pinned by [D10](../../DECISIONS.md#d10--kernel-confinement-for-shell-landlock-vendored-one-writable-root).

| | |
|---|---|
| repo | `https://github.com/deepseek-ai/deepseek-harness.git` |
| path | `native/landlock-run/packages/entry/src/main.c` |
| commit | `6e05cb7ff5bc9834fcf303800264fa3cdb3724e8` (2026-08-13) |
| sha256 | `c2d6f330e31924ccba7c9b70416e05b427bb7aacfc21dcd8d610cf22c20bd53a` |
| license | BSD-3-Clause — *not* the MIT of its parent monorepo; see `LICENSE` in this directory |

`main.c` is **byte-identical** to the pinned commit. `CMakeLists.txt` checks its sha256 at
configure time and refuses to build on a mismatch, so the file must never be hand-edited —
any change belongs in a re-vendor from a new pinned commit, with this table updated to match.

`main`'s only externally-visible symbol is renamed to `landlock_run_cli_main` at **compile
time**, via `target_compile_definitions(... PRIVATE main=landlock_run_cli_main)` on the
`landlock_run_vendored` CMake target — not by editing the source. This is how the file stays
callable as a library (D10: "vendored... rather than shipped as a helper binary, so D3's
single-artifact property survives") while remaining byte-identical to the hash above.
