#pragma once

// Not a public header -- included only by confine.cpp.
//
// The vendored landlock-run CLI (third_party/landlock-run/main.c, D10) exposes exactly one
// externally-visible symbol once compiled: its own `main`, renamed to `landlock_run_cli_main`
// at COMPILE TIME by the landlock_run_vendored CMake target
// (`target_compile_definitions(... PRIVATE main=landlock_run_cli_main)`), never by editing the
// source -- the vendored file must stay byte-identical to the sha256 CMakeLists.txt checks.
//
// Two things a caller must uphold that the C signature cannot enforce:
//
//  1. `landlock_run_cli_main` never frees the argv-parsing buffers it `calloc`s -- its own
//     source comment says why: "the launcher execs or exits, so no free." Call it from exactly
//     one place: a forked child that immediately `_exit()`s with its return value (a successful
//     path never returns at all -- it `execvp`s and the process image is replaced). Calling it
//     anywhere that returns control to the rest of Hermit leaks on every call.
//  2. The `argv` array must be NUL-terminated at `argv[argc]`, matching the guarantee a real
//     process's argv gets from the OS for free. `parse()` inside the vendored file slices a
//     pointer into this array and walks it looking for that sentinel; a hand-built
//     std::vector<char*> does not provide it automatically.

extern "C" int landlock_run_cli_main(int argc, char** argv);
