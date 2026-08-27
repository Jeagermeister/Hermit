# 10. Building

## What you need

- **Linux.** This is not a placeholder for "portable eventually" — the guarantees are designed
  against POSIX semantics and Landlock ([SCOPE.md](../SCOPE.md) § Platforms).
- **A C++23 compiler.** Verified on GCC 16.2.1 and clang 22.1.8 (both against libstdc++;
  libc++ is untested). The build's real gate is a `try_compile` feature check for the library
  pieces it needs; the version floor (g++ 12, clang 16) exists only to turn a template error
  into one sentence.
- **CMake 3.25+** and a generator — the examples use Ninja.
- **Network access on the first configure.** Dependencies are fetched at pinned versions
  rather than taken from the system ([D3](../DECISIONS.md)), so a fresh clone builds with no
  package-manager step, and both build machines resolve the identical dependency set.

Ollama is not needed to build or to run the unit tests — only to run anything that talks to a
model.

## The plain build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The test suite is GoogleTest; a handful of tests are environment-gated (they skip rather than
fail where the machine cannot support what they check — a kernel without Landlock, for
instance).

## The sanitizer build

Every substantive change in this repo's history has run under AddressSanitizer and
UndefinedBehaviorSanitizer — it is the stated compensating discipline for choosing C++ over
Rust ([FAQ](../FAQ.md)), and it has caught a real use-after-free before it shipped. Keep it in
its own directory:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DHERMIT_SANITIZE=ON
cmake --build build-asan && ./build-asan/tests/hermit_tests
```

## The LTO build (optional)

`HERMIT_LTO=ON` shrinks the binary ~37% unstripped / ~42% stripped, measured on one machine
([PERFORMANCE.md](../PERFORMANCE.md), [D16](../DECISIONS.md)). It is opt-in, and it wants its
own build directory — the option is cached, so pointing it at `build` would quietly convert
your plain build and leave it that way:

```bash
cmake -S . -B build-lto -G Ninja -DHERMIT_LTO=ON
cmake --build build-lto
```

Treat it as a size change with a compile-time bonus, not a speed fix — the runtime win is
unmeasured and unlikely to matter on a tool that is blocked on inference.

## What you end up with

`build/hermit` — one binary, no runtime dependencies beyond the distro's shared libraries.
There is no install target and no per-machine build variant: machine differences are
configuration, never code ([ROUTING.md](../ROUTING.md) §9). The binary's subcommands are the
subject of the [CLI reference](./13-cli-reference.md); the fastest way to something running is
the [Quickstart](./11-quickstart.md).
