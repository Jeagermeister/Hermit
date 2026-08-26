cmake_minimum_required(VERSION 3.25)  # SYSTEM on FetchContent_Declare

project(hermit
  VERSION 0.0.1
  DESCRIPTION "Supervisor for local models doing bounded filesystem work"
  LANGUAGES C CXX)  # C: the vendored Landlock launcher (D10) is C11

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# --- toolchain floor ---------------------------------------------------------
# Two checks, and the second is the one that means anything.
#
# The version numbers are a *courtesy*: they turn "no member named 'expected' in
# namespace 'std'" -- a hundred lines deep in a template -- into one sentence at
# configure time. They are the documented first releases with the C++23 library
# pieces this uses, and neither has been tested here.
#
# The feature check below is the real gate, because the binding constraint is the
# standard *library*, not the compiler. Clang on this machine uses libstdc++, so its
# std::expected is GCC's; a version check on the compiler alone would be asking the
# wrong component. try_compile asks the toolchain that will actually build this,
# whatever combination it turns out to be.
set(HERMIT_MIN_GNU   12 CACHE STRING "Minimum g++ (libstdc++ std::expected)")
set(HERMIT_MIN_CLANG 16 CACHE STRING "Minimum clang++ (libc++ std::expected)")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS HERMIT_MIN_GNU)
  message(FATAL_ERROR
    "g++ ${CMAKE_CXX_COMPILER_VERSION} is too old; ${HERMIT_MIN_GNU}+ is required for <expected>.")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS HERMIT_MIN_CLANG)
  message(FATAL_ERROR
    "clang++ ${CMAKE_CXX_COMPILER_VERSION} is too old; ${HERMIT_MIN_CLANG}+ is required.")
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++23")
check_cxx_source_compiles("
#include <expected>
#include <algorithm>
#include <string_view>
#include <charconv>
int main() {
  std::expected<int, int> ok{1};
  std::expected<int, int> bad{std::unexpected{2}};
  if (!ok || bad.has_value()) return bad.error() + *ok;
  std::string_view sv{\"num_ctx 1\"};
  if (!sv.starts_with(\"num\") || !sv.ends_with(\"1\")) return 1;
  int value = 0;
  (void)std::from_chars(sv.data(), sv.data() + sv.size(), value);
  int items[] = {3, 1, 2};
  return std::ranges::find_if(items, [](int i) { return i == 2; }) == std::end(items);
}" HERMIT_HAS_REQUIRED_CXX23)
unset(CMAKE_REQUIRED_FLAGS)

if(NOT HERMIT_HAS_REQUIRED_CXX23)
  message(FATAL_ERROR
    "This toolchain cannot build hermit: the C++23 standard library pieces it needs "
    "(std::expected, std::ranges, std::from_chars) are missing or incomplete.\n"
    "  compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})\n"
    "Note that clang may be using libstdc++ rather than libc++, in which case the "
    "limiting component is the installed GCC, not clang.")
endif()

option(HERMIT_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(HERMIT_SANITIZE    "Build with ASan + UBSan"             OFF)
option(HERMIT_LTO         "Build with link-time (interprocedural) optimization" OFF)

# Measured in PERFORMANCE.md: ~37% smaller unstripped binary, ~42% stripped, no build-time
# cost on the machine it was measured on. Left opt-in rather than default-on because that
# result is one machine's number, not a promise -- see PERFORMANCE.md before flipping the
# default.
#
# A plain variable, set before any target is declared -- deliberately not a CACHE one.
# add_library/add_executable read it when the target is created, and add_subdirectory hands
# it down, so it reaches hermit_core through hermit_tests without being named per target.
# Keeping it out of the cache keeps it scoped to us and our children rather than leaking
# into a superproject that add_subdirectory'd this one. Note the reach is everything built
# from source below, which includes the vendored landlock C file and googletest; the
# header-only dependencies have no objects for it to touch.
if(HERMIT_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT HERMIT_IPO_SUPPORTED OUTPUT HERMIT_IPO_ERROR)
  if(NOT HERMIT_IPO_SUPPORTED)
    message(FATAL_ERROR "HERMIT_LTO=ON but this toolchain doesn't support IPO: ${HERMIT_IPO_ERROR}")
  endif()
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# --- warning/sanitizer policy, applied via link rather than globally ----------
# Two targets, not one: warnings are our business and stay PRIVATE, while sanitizer
# flags are a usage requirement -- anything linking hermit_core must be built and
# linked with them too, or it fails with undefined __asan_* references.
add_library(hermit_warnings INTERFACE)
target_compile_options(hermit_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
  -Wold-style-cast -Woverloaded-virtual -Wdouble-promotion)

add_library(hermit_sanitizers INTERFACE)
if(HERMIT_SANITIZE)
  # Applied globally as well as through the interface target, and deliberately so:
  # instrumenting our code but not googletest mixes instrumented and uninstrumented
  # std::string/std::vector across the test boundary, which produces ASan
  # container-overflow FALSE POSITIVES that read like real bugs in walk(). Must appear
  # before any add_subdirectory/FetchContent so dependencies inherit it.
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)

  # The interface target still carries them for anything consuming hermit::core from
  # another project, where the global flags above do not apply.
  target_compile_options(hermit_sanitizers INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(hermit_sanitizers    INTERFACE -fsanitize=address,undefined)
endif()

# --- dependencies ------------------------------------------------------------
# Fetched at pinned tags rather than taken from the system, so the MSI laptop and
# kitchen-desktop compile identical sources (see DECISIONS.md, D3).
#
# Both arrive here with the Ollama client, which is the first code to parse JSON (D2)
# or speak HTTP (D7). SYSTEM suppresses their warnings: our -Wold-style-cast and
# -Wpedantic are a policy for our code, and applying them to vendored headers only
# produces noise nobody can act on.
include(FetchContent)

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE
  SYSTEM)

# Loopback Ollama only, per D7: no TLS, no auth, no proxies. OpenSSL support stays
# off deliberately -- linking it would add a dependency purely to encrypt traffic to
# 127.0.0.1, and would put certificate handling in a codebase that has no use for it.
#
# Every *_IF_AVAILABLE toggle is forced off, and the reason is D3, not just D7:
# "if available" makes the binary depend on what the build machine happens to have
# installed, which is exactly the machine-difference D3 pins dependencies to remove.
# This list went stale once already -- httplib grew a zstd toggle and libzstd
# silently joined every link line on the machine that had it installed. When bumping
# the httplib pin, grep its CMakeLists for new *_IF_AVAILABLE toggles and force them
# off here.
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.53.0
  GIT_SHALLOW    TRUE
  SYSTEM)

FetchContent_MakeAvailable(nlohmann_json httplib)

# --- vendored Landlock launcher (D10) -----------------------------------------
# Not fetched via FetchContent: it is one file, pulled out of an unrelated monorepo, pinned to a
# specific commit D10 records (third_party/landlock-run/NOTICE.md). The sha256 check below gives
# it D3's "a dependency that differs silently is one more uncontrolled variable" guarantee by
# another means -- FATAL_ERROR on any hand-edit of the vendored file, since confine.cpp's callers
# only make sense if it is byte-identical to the audited, licensed source.
file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/third_party/landlock-run/main.c HERMIT_LANDLOCK_RUN_ACTUAL_SHA256)
set(HERMIT_LANDLOCK_RUN_SHA256 c2d6f330e31924ccba7c9b70416e05b427bb7aacfc21dcd8d610cf22c20bd53a)
if(NOT HERMIT_LANDLOCK_RUN_ACTUAL_SHA256 STREQUAL HERMIT_LANDLOCK_RUN_SHA256)
  message(FATAL_ERROR
    "third_party/landlock-run/main.c does not match the sha256 D10 pins "
    "(got ${HERMIT_LANDLOCK_RUN_ACTUAL_SHA256}, expected ${HERMIT_LANDLOCK_RUN_SHA256}). "
    "The vendored file must stay byte-identical to the audited source -- see NOTICE.md.")
endif()

# main -> landlock_run_cli_main at COMPILE TIME, not by editing the vendored file -- see
# confine_internal.h and NOTICE.md for why. No hermit_warnings: this is audited, hash-pinned
# third-party C, and our warning policy is for our own code, same reasoning already applied to
# nlohmann/httplib above (SYSTEM).
add_library(landlock_run_vendored third_party/landlock-run/main.c)
set_target_properties(landlock_run_vendored PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_definitions(landlock_run_vendored PRIVATE main=landlock_run_cli_main)

# --- the library -------------------------------------------------------------
# core knows nothing about models or the network (D7's layering table). The Ollama
# client is a separate target for that reason, and it is the only one that links
# httplib -- if core ever needs it, the layering has been broken.
add_library(hermit_core
  src/hermit/core/backup.cpp
  src/hermit/core/confine.cpp
  src/hermit/core/fsio.cpp
  src/hermit/core/observed.cpp
  src/hermit/core/sandbox.cpp
  src/hermit/core/sha256.cpp
  src/hermit/core/tool.cpp
  src/hermit/core/tools/edit.cpp
  src/hermit/core/tools/find.cpp
  src/hermit/core/tools/grep.cpp
  src/hermit/core/tools/hash.cpp
  src/hermit/core/tools/list.cpp
  src/hermit/core/tools/move.cpp
  src/hermit/core/tools/read.cpp
  src/hermit/core/tools/shell.cpp
  src/hermit/core/tools/write.cpp)

target_include_directories(hermit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hermit_core
  PRIVATE hermit_warnings landlock_run_vendored
  PUBLIC  hermit_sanitizers)
add_library(hermit::core ALIAS hermit_core)

add_library(hermit_ollama
  src/hermit/ollama/client.cpp
  src/hermit/ollama/preflight.cpp)

target_include_directories(hermit_ollama PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hermit_ollama
  PRIVATE hermit_warnings httplib::httplib
  PUBLIC  hermit_sanitizers nlohmann_json::nlohmann_json)
add_library(hermit::ollama ALIAS hermit_ollama)

# The supervisor layer of D7's table: it drives the model, so it sits above the
# transport and below the frontends. It links hermit_ollama for the request and reply
# types but opens no socket itself -- every policy decision in it is meant to be
# testable without a daemon.
#
# hermit_core is the first of the two edges ROUTING.md section 7 recorded missing.
# In-tree the gap was masked -- every target shares one include dir and the final
# binaries link all four archives, so a Tier 1 tool here would have compiled by
# accident. The edge turns that accident into a declared dependency; an out-of-tree
# consumer of hermit::supervisor alone is who it would actually have broken.
add_library(hermit_supervisor
  src/hermit/supervisor/judge.cpp
  src/hermit/supervisor/loop.cpp
  src/hermit/supervisor/reinvoke.cpp
  src/hermit/supervisor/semantic.cpp
  src/hermit/supervisor/session.cpp
  src/hermit/supervisor/undo.cpp
  src/hermit/supervisor/verify.cpp
  src/hermit/supervisor/wire.cpp)

target_include_directories(hermit_supervisor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hermit_supervisor
  PRIVATE hermit_warnings
  PUBLIC  hermit_core hermit_ollama hermit_sanitizers)
add_library(hermit::supervisor ALIAS hermit_supervisor)

# The frontend layer of D7's table. It names a model and a base URL, so it cannot be
# core; it is a library rather than code inside main() because the CLI and the eventual
# MCP-over-stdio server are supposed to compose themselves from the same settings.
#
# hermit_supervisor is the second of ROUTING.md section 7's missing edges: registry
# composition happens in this layer, and before it no library target saw all four --
# only the two final binaries did -- so composition code here would have leaned on
# the binaries' link lines rather than a declared dependency.
add_library(hermit_app
  src/hermit/app/config.cpp
  src/hermit/app/expect.cpp
  src/hermit/app/toolset.cpp)

target_include_directories(hermit_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hermit_app
  PRIVATE hermit_warnings
  PUBLIC  hermit_core hermit_ollama hermit_sanitizers hermit_supervisor
          nlohmann_json::nlohmann_json)
add_library(hermit::app ALIAS hermit_app)

# --- the binary --------------------------------------------------------------
add_executable(hermit src/main.cpp)
target_link_libraries(hermit PRIVATE hermit_app hermit_core hermit_ollama hermit_supervisor hermit_warnings)

# --- tests -------------------------------------------------------------------
if(HERMIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
