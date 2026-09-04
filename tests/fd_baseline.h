#pragma once

// Every fd currently open in this process, read the same way assert_no_inheritable_fds itself
// does. The test binary is launched by whatever runs it -- direct invocation, or ctest, which in
// practice leaves an extra fd of its own open without O_CLOEXEC (observed empirically building
// D10: fd 3, presumably ctest's own output-capture plumbing). That fd is ctest's business, not
// any test's, and not something a real `hermit` process launched normally would ever have --
// so tests snapshot "what's already open" as their baseline and only assert about fds THEY
// deliberately add on top of it, rather than assuming a pristine table no test harness owes
// them.
//
// Shared by every fixture that forks under D10's pre-fork audit (confine_test.cpp,
// shell_test.cpp). It lived in confine_test.cpp alone until 2026-09-04, when the shell fixture
// -- probing without it -- turned out to skip all seven of its tests under `ctest` and run them
// only when the binary was launched directly.

#include <charconv>
#include <string_view>
#include <system_error>
#include <vector>

#include <dirent.h>

namespace hermit::test {

inline std::vector<int> currently_open_fds() {
  std::vector<int> fds;
  DIR* dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) return fds;
  int self_fd = ::dirfd(dir);
  while (::dirent* entry = ::readdir(dir)) {
    std::string_view name{entry->d_name};
    if (name == "." || name == "..") continue;
    int fd = -1;
    auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), fd);
    if (ec == std::errc{} && ptr == name.data() + name.size() && fd != self_fd) {
      fds.push_back(fd);
    }
  }
  ::closedir(dir);
  return fds;
}

}  // namespace hermit::test
