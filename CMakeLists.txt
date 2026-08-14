cmake_minimum_required(VERSION 3.25)  # SYSTEM on FetchContent_Declare

project(hermes-cpp
  VERSION 0.0.1
  DESCRIPTION "Supervisor for local models doing bounded filesystem work"
  LANGUAGES CXX)

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
set(HERMES_MIN_GNU   12 CACHE STRING "Minimum g++ (libstdc++ std::expected)")
set(HERMES_MIN_CLANG 16 CACHE STRING "Minimum clang++ (libc++ std::expected)")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS HERMES_MIN_GNU)
  message(FATAL_ERROR
    "g++ ${CMAKE_CXX_COMPILER_VERSION} is too old; ${HERMES_MIN_GNU}+ is required for <expected>.")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
       CMAKE_CXX_COMPILER_VERSION VERSION_LESS HERMES_MIN_CLANG)
  message(FATAL_ERROR
    "clang++ ${CMAKE_CXX_COMPILER_VERSION} is too old; ${HERMES_MIN_CLANG}+ is required.")
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
}" HERMES_HAS_REQUIRED_CXX23)
unset(CMAKE_REQUIRED_FLAGS)

if(NOT HERMES_HAS_REQUIRED_CXX23)
  message(FATAL_ERROR
    "This toolchain cannot build hermes-cpp: the C++23 standard library pieces it needs "
    "(std::expected, std::ranges, std::from_chars) are missing or incomplete.\n"
    "  compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})\n"
    "Note that clang may be using libstdc++ rather than libc++, in which case the "
    "limiting component is the installed GCC, not clang.")
endif()

option(HERMES_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(HERMES_SANITIZE    "Build with ASan + UBSan"             OFF)

# --- warning/sanitizer policy, applied via link rather than globally ----------
# Two targets, not one: warnings are our business and stay PRIVATE, while sanitizer
# flags are a usage requirement -- anything linking hermes_core must be built and
# linked with them too, or it fails with undefined __asan_* references.
add_library(hermes_warnings INTERFACE)
target_compile_options(hermes_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
  -Wold-style-cast -Woverloaded-virtual -Wdouble-promotion)

add_library(hermes_sanitizers INTERFACE)
if(HERMES_SANITIZE)
  # Applied globally as well as through the interface target, and deliberately so:
  # instrumenting our code but not googletest mixes instrumented and uninstrumented
  # std::string/std::vector across the test boundary, which produces ASan
  # container-overflow FALSE POSITIVES that read like real bugs in walk(). Must appear
  # before any add_subdirectory/FetchContent so dependencies inherit it.
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)

  # The interface target still carries them for anything consuming hermes::core from
  # another project, where the global flags above do not apply.
  target_compile_options(hermes_sanitizers INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(hermes_sanitizers    INTERFACE -fsanitize=address,undefined)
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
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.53.0
  GIT_SHALLOW    TRUE
  SYSTEM)

FetchContent_MakeAvailable(nlohmann_json httplib)

# --- the library -------------------------------------------------------------
# core knows nothing about models or the network (D7's layering table). The Ollama
# client is a separate target for that reason, and it is the only one that links
# httplib -- if core ever needs it, the layering has been broken.
add_library(hermes_core
  src/hermes/core/sandbox.cpp)

target_include_directories(hermes_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hermes_core
  PRIVATE hermes_warnings
  PUBLIC  hermes_sanitizers)
add_library(hermes::core ALIAS hermes_core)

add_library(hermes_ollama
  src/hermes/ollama/client.cpp
  src/hermes/ollama/preflight.cpp)

target_include_directories(hermes_ollama PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hermes_ollama
  PRIVATE hermes_warnings httplib::httplib
  PUBLIC  hermes_sanitizers nlohmann_json::nlohmann_json)
add_library(hermes::ollama ALIAS hermes_ollama)

# The frontend layer of D7's table. It names a model and a base URL, so it cannot be
# core; it is a library rather than code inside main() because the CLI and the eventual
# MCP-over-stdio server are supposed to compose themselves from the same settings.
add_library(hermes_app
  src/hermes/app/config.cpp)

target_include_directories(hermes_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hermes_app
  PRIVATE hermes_warnings
  PUBLIC  hermes_core hermes_ollama hermes_sanitizers nlohmann_json::nlohmann_json)
add_library(hermes::app ALIAS hermes_app)

# --- the binary --------------------------------------------------------------
add_executable(hermes-cpp src/main.cpp)
target_link_libraries(hermes-cpp PRIVATE hermes_app hermes_core hermes_ollama hermes_warnings)

# --- tests -------------------------------------------------------------------
if(HERMES_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
