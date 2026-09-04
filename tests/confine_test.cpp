#include <hermit/core/confine.h>

#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "fd_baseline.h"

namespace fs = std::filesystem;
using hermit::ConfineError;
using hermit::ConfinementProbeResult;
using hermit::ConfineResult;
using hermit::assert_no_inheritable_fds;
using hermit::probe_confinement;
using hermit::run_confined;

namespace {

fs::path make_temp_dir(std::string_view tag) {
  std::string tpl = (fs::temp_directory_path() / (std::string("hermit_confine_") + std::string(tag) + "_XXXXXX")).string();
  std::vector<char> buf(tpl.begin(), tpl.end());
  buf.push_back('\0');
  EXPECT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
  return fs::canonical(fs::path(buf.data()));
}

using hermit::test::currently_open_fds;

}  // namespace

// --- assert_no_inheritable_fds -------------------------------------------------

TEST(AssertNoInheritableFds, IgnoresACloexecFd) {
  std::vector<int> baseline = currently_open_fds();  // pre-approve whatever the harness left open
  fs::path tmp = make_temp_dir("cloexec");
  int fd = ::open((tmp / "f").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
  ASSERT_GE(fd, 0);

  auto result = assert_no_inheritable_fds(baseline);
  EXPECT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));

  ::close(fd);
  std::error_code ec;
  fs::remove_all(tmp, ec);
}

TEST(AssertNoInheritableFds, RefusesALeakedFd) {
  std::vector<int> baseline = currently_open_fds();
  fs::path tmp = make_temp_dir("leak");
  int fd = ::open((tmp / "f").c_str(), O_WRONLY | O_CREAT, 0600);  // deliberately no O_CLOEXEC
  ASSERT_GE(fd, 0);

  // baseline was snapshotted BEFORE `fd` was opened, so `fd` itself is not pre-approved by it --
  // this is what makes the refusal below about the fd this test just leaked, not about ambient
  // ones the launching harness holds.
  auto result = assert_no_inheritable_fds(baseline);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, hermit::ConfineErrorKind::InheritedFd);

  ::close(fd);
  std::error_code ec;
  fs::remove_all(tmp, ec);
}

TEST(AssertNoInheritableFds, AcceptsAnExplicitlyAllowedFd) {
  std::vector<int> baseline = currently_open_fds();
  fs::path tmp = make_temp_dir("allow");
  int fd = ::open((tmp / "f").c_str(), O_WRONLY | O_CREAT, 0600);  // no O_CLOEXEC
  ASSERT_GE(fd, 0);

  baseline.push_back(fd);
  auto result = assert_no_inheritable_fds(baseline);
  EXPECT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));

  ::close(fd);
  std::error_code ec;
  fs::remove_all(tmp, ec);
}

// --- run_confined / probe_confinement -------------------------------------------
// D10's mechanism is only meaningfully testable where Landlock actually works. Every fixture
// below probes first and skips rather than asserting a specific kernel capability the CI/dev
// machine may not have -- per D11's fail-closed vocabulary, "could not determine it" is not the
// same claim as "it does not hold", so a skip is the honest response, not a forced failure.
//
// Every call below passes the same launch-time fd baseline for the same reason the fd-audit
// tests do above: run_confined/probe_confinement each run their own pre-fork audit internally,
// and the launching harness's own fds are exactly as out of THIS process's control as they were
// in the tests above.

class ConfineLandlockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    baseline_ = currently_open_fds();
    auto probe = probe_confinement(baseline_);
    if (!probe.has_value() || *probe != ConfinementProbeResult::Enforced) {
      GTEST_SKIP() << "Landlock is not enforced on this machine";
    }
    root_ = make_temp_dir("root");
    outside_ = make_temp_dir("outside");
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::remove_all(outside_, ec);
  }

  std::vector<int> baseline_;
  fs::path root_, outside_;
};

TEST_F(ConfineLandlockTest, WriteInsideRootSucceeds) {
  fs::path target = root_ / "file";
  auto result = run_confined(root_, std::vector<std::string>{"/usr/bin/touch", target.string()},
                              baseline_);
  ASSERT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));
  EXPECT_EQ(result->exit_code, 0);
  EXPECT_TRUE(fs::exists(target));
}

TEST_F(ConfineLandlockTest, WriteOutsideRootIsDenied) {
  fs::path target = outside_ / "file";
  auto result = run_confined(root_, std::vector<std::string>{"/usr/bin/touch", target.string()},
                              baseline_);
  ASSERT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));
  EXPECT_NE(result->exit_code, 0);
  EXPECT_FALSE(fs::exists(target));
}

TEST_F(ConfineLandlockTest, DevNullWriteSucceeds) {
  // The non-directory --rw grant D10 calls out explicitly: add_rule narrows a file-target grant
  // to file-compatible bits, and /dev/null is the one this project's own grant table relies on.
  // A shell redirect forces a real open(O_WRONLY|O_TRUNC) + write, unlike `touch`, whose update
  // of an existing file's mtime doesn't exercise WRITE_FILE at all (D10: utimes is not hooked).
  auto result = run_confined(
      root_, std::vector<std::string>{"/bin/sh", "-c", "echo hermit > /dev/null"}, baseline_);
  ASSERT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));
  EXPECT_EQ(result->exit_code, 0);
}

TEST_F(ConfineLandlockTest, ExecUnderUsrWorks) {
  auto result = run_confined(root_, std::vector<std::string>{"/usr/bin/true"}, baseline_);
  ASSERT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));
  EXPECT_EQ(result->exit_code, 0);
}

// --- R8: timeout + capture -------------------------------------------------------

using hermit::ConfineLimits;

TEST_F(ConfineLandlockTest, TimeoutKillsAHangingProcessAndReportsIt) {
  ConfineLimits limits{.timeout = std::chrono::milliseconds{200}};
  const auto started = std::chrono::steady_clock::now();
  auto result = run_confined(root_, std::vector<std::string>{"/bin/sh", "-c", "sleep 30"},
                              baseline_, limits);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  ASSERT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));
  EXPECT_TRUE(result->timed_out);
  // Well under the 30s the command itself asked to sleep -- proof the kill actually fired
  // rather than merely that the timeout field defaults true or the call happened to finish.
  EXPECT_LT(elapsed, std::chrono::seconds{10});
}

TEST_F(ConfineLandlockTest, TimeoutKillsTheWholeProcessGroup) {
  fs::path marker = root_ / "marker";
  ConfineLimits limits{.timeout = std::chrono::milliseconds{200}};
  // The `sh -c` command itself returns almost immediately (backgrounding is asynchronous), so a
  // 200ms timeout on the *parent* call proves nothing about the grandchild unless that
  // grandchild is also swept -- which is exactly what setpgid/killpg is for. If the group kill
  // did not work, the grandchild's `sleep 2 && touch marker` would go on to create the marker
  // long after this call returns.
  auto result = run_confined(
      root_,
      std::vector<std::string>{"/bin/sh", "-c",
                               "(sleep 2 && touch " + marker.string() + ") & sleep 30"},
      baseline_, limits);
  ASSERT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));
  EXPECT_TRUE(result->timed_out);

  std::this_thread::sleep_for(std::chrono::seconds{3});  // past the grandchild's own 2s sleep
  EXPECT_FALSE(fs::exists(marker))
      << "the backgrounded grandchild survived the timeout and created its marker";
}

TEST_F(ConfineLandlockTest, CapturesStdoutAndStderrExactly) {
  ConfineLimits limits{.stdout_cap = 4096, .stderr_cap = 4096};
  auto result = run_confined(
      root_, std::vector<std::string>{"/bin/sh", "-c", "printf hello; printf world 1>&2"},
      baseline_, limits);
  ASSERT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));
  EXPECT_FALSE(result->timed_out);
  EXPECT_EQ(result->exit_code, 0);
  EXPECT_EQ(result->stdout_capture.bytes, "hello");
  EXPECT_FALSE(result->stdout_capture.truncated);
  EXPECT_EQ(result->stderr_capture.bytes, "world");
  EXPECT_FALSE(result->stderr_capture.truncated);
}

TEST_F(ConfineLandlockTest, CaptureTruncatesAtCapAndFlags) {
  // A pure shell loop, not `/dev/zero` or an external generator: D10's fixed grant table has no
  // /dev entry that permits reading arbitrary device data, only `--rw /dev/null` and
  // `--ro /dev/urandom` (neither fits here), so the generator has to be something already
  // reachable under the standard grants -- the confined `sh` itself, looping. 50000 lines of 40
  // 'x's each is ~2MB, comfortably over both the 16-byte cap and a typical 64KiB pipe buffer, so
  // this is also the regression test for the back-pressure fix (draining past the cap instead of
  // stopping there): the old "stop reading at the cap" design would have deadlocked this child on
  // a full pipe instead of letting it finish.
  ConfineLimits limits{.stdout_cap = 16};
  auto result = run_confined(
      root_,
      std::vector<std::string>{
          "/bin/sh", "-c",
          "i=0; while [ $i -lt 50000 ]; do "
          "printf 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'; i=$((i+1)); done"},
      baseline_, limits);
  ASSERT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));
  EXPECT_FALSE(result->timed_out);
  EXPECT_EQ(result->exit_code, 0);
  EXPECT_LE(result->stdout_capture.bytes.size(), 16u);
  EXPECT_TRUE(result->stdout_capture.truncated);
}

TEST(ConfinementProbe, ReportsEnforcedOnThisMachine) {
  // Verified separately (this session, via /sys/kernel/security/lsm) that Landlock is active on
  // the machine these tests run on -- so unlike ConfineLandlockTest's fixture, which exists
  // specifically to tolerate a machine where that is not true, this test asserts the specific
  // result rather than skipping around it.
  auto result = probe_confinement(currently_open_fds());
  ASSERT_TRUE(result.has_value()) << (result ? "" : to_string(result.error()));
  EXPECT_EQ(*result, ConfinementProbeResult::Enforced);
}
