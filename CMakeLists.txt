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
# nlohmann/json is settled as the JSON library (D2) but is deliberately NOT declared
# here yet: no source includes it, and declaring it costs a git clone per build
# directory for nothing. It arrives with the first code that parses JSON.

# --- the library -------------------------------------------------------------
add_library(hermes_core
  src/hermes/core/sandbox.cpp)

target_include_directories(hermes_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hermes_core
  PRIVATE hermes_warnings
  PUBLIC  hermes_sanitizers)
add_library(hermes::core ALIAS hermes_core)

# --- the binary --------------------------------------------------------------
add_executable(hermes-cpp src/main.cpp)
target_link_libraries(hermes-cpp PRIVATE hermes_core hermes_warnings)

# --- tests -------------------------------------------------------------------
if(HERMES_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
