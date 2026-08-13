include(FetchContent)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
  GIT_SHALLOW    TRUE
  SYSTEM)
FetchContent_MakeAvailable(googletest)

add_executable(hermes_tests
  sandbox_test.cpp)

target_link_libraries(hermes_tests
  PRIVATE hermes_core hermes_warnings GTest::gtest_main)

include(GoogleTest)
# A symlink-loop bug shows up as non-termination, not as a wrong answer, so the loop
# test needs a clock behind it or it hangs the suite instead of failing it.
gtest_discover_tests(hermes_tests PROPERTIES TIMEOUT 30)
