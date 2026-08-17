include(FetchContent)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
  GIT_SHALLOW    TRUE
  SYSTEM)
FetchContent_MakeAvailable(googletest)

add_executable(hermit_tests
  sandbox_test.cpp
  tool_test.cpp
  fsio_test.cpp
  sha256_test.cpp
  observe_tools_test.cpp
  search_tools_test.cpp
  mutate_tools_test.cpp
  ollama_client_test.cpp
  preflight_test.cpp
  config_test.cpp
  session_test.cpp
  wire_test.cpp
  loop_test.cpp
  verify_test.cpp
  judge_test.cpp)

target_link_libraries(hermit_tests
  PRIVATE hermit_app hermit_core hermit_ollama hermit_supervisor hermit_warnings
          GTest::gtest_main)

include(GoogleTest)
# A symlink-loop bug shows up as non-termination, not as a wrong answer, so the loop
# test needs a clock behind it or it hangs the suite instead of failing it.
gtest_discover_tests(hermit_tests PROPERTIES TIMEOUT 30)
