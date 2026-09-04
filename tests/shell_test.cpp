#include <hermit/core/tools/shell.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>  // mkdtemp
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <hermit/core/confine.h>
#include <hermit/core/fsio.h>
#include <hermit/core/sandbox.h>
#include <hermit/core/tool.h>

#include "fd_baseline.h"

namespace fs = std::filesystem;
using hermit::ConfinementProbeResult;
using hermit::Field;
using hermit::kDefaultMaxReadBytes;
using hermit::parse_args;
using hermit::probe_confinement;
using hermit::RawArgs;
using hermit::Sandbox;
using hermit::ShellTool;
using hermit::Tool;
using hermit::ToolOutput;
using hermit::ToolRow;
using hermit::test::currently_open_fds;

namespace {

const std::string* text(const ToolRow& row, std::string_view name) {
  for (const Field& f : row.fields) {
    if (f.name == name) return std::get_if<std::string>(&f.value);
  }
  return nullptr;
}
const bool* boolean(const ToolRow& row, std::string_view name) {
  for (const Field& f : row.fields) {
    if (f.name == name) return std::get_if<bool>(&f.value);
  }
  return nullptr;
}
const std::int64_t* integer(const ToolRow& row, std::string_view name) {
  for (const Field& f : row.fields) {
    if (f.name == name) return std::get_if<std::int64_t>(&f.value);
  }
  return nullptr;
}

std::expected<ToolOutput, hermit::ToolError> call(Tool& tool, const Sandbox& box,
                                                   const RawArgs& raw) {
  auto parsed = parse_args(tool.spec(), raw, box);
  if (!parsed) return std::unexpected{hermit::ToolError{to_string(parsed.error())}};
  return tool.invoke(*parsed);
}

// D10's mechanism is only meaningfully testable where Landlock actually works -- same fail-
// closed-vocabulary reasoning ConfineLandlockTest (confine_test.cpp) already documents: a skip
// here is "could not determine it," never a claim that shell itself is broken.
//
// The probe, and every ShellTool the fixture builds, are handed the launch-time fd baseline for
// the reason fd_baseline.h gives: the pre-fork audit fails closed on any inheritable descriptor,
// and ctest leaves one of its own open. Without the baseline this fixture skipped under the
// documented `ctest` invocation and ran only when the binary was launched directly -- every
// "all tests pass" via ctest had run seven fewer tests than it claimed (found 2026-09-04). The
// two audit tests at the bottom are the regression: one fd added on top of the baseline, named
// or not named, and the tool's answer to each.
class ShellToolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    baseline_ = currently_open_fds();
    auto probe = probe_confinement(baseline_);
    if (!probe.has_value()) {
      GTEST_SKIP() << "confinement could not be probed on this machine: "
                   << to_string(probe.error());
    }
    if (*probe != ConfinementProbeResult::Enforced) {
      GTEST_SKIP() << "Landlock is not enforced on this machine";
    }

    std::string tpl = (fs::temp_directory_path() / "hermit_shell_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));

    auto sb = Sandbox::open(tmp_);
    ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
    box_ = std::make_unique<Sandbox>(std::move(*sb));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  /// A ShellTool over the fixture's root that pre-approves whatever the launching harness
  /// left open -- the same baseline the probe above used. The only fds any test here asserts
  /// about are ones it opens itself, on top of that.
  ShellTool shell(std::chrono::milliseconds timeout) const {
    return ShellTool{tmp_, timeout, kDefaultMaxReadBytes, baseline_};
  }

  /// One inheritable descriptor deliberately added on top of the baseline: opened without
  /// O_CLOEXEC, which is exactly what the pre-fork audit exists to catch.
  int open_inheritable(std::string_view name) const {
    return ::open((tmp_ / name).c_str(), O_WRONLY | O_CREAT, 0600);
  }

  fs::path tmp_;
  std::unique_ptr<Sandbox> box_;
  std::vector<int> baseline_;
};

}  // namespace

// --- spec / argument validation -- no Landlock required for these ----------------

TEST(ShellToolSpec, TakesOneRequiredCommandString) {
  ShellTool shell{"/tmp", std::chrono::seconds{5}};
  const auto& spec = shell.spec();
  ASSERT_EQ(spec.args.size(), 1u);
  EXPECT_EQ(spec.args[0].name, "command");
  EXPECT_TRUE(spec.args[0].required);
}

TEST_F(ShellToolTest, EmptyCommandIsRefusedWithoutRunningAnything) {
  ShellTool tool = shell(std::chrono::seconds{5});
  auto out = call(tool, *box_, {{"command", std::string{""}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("empty"), std::string::npos) << out.error().reason;
}

// --- execution ---------------------------------------------------------------------

TEST_F(ShellToolTest, SuccessfulCommandReturnsExitZeroAsData) {
  ShellTool tool = shell(std::chrono::seconds{5});
  auto out = call(tool, *box_, {{"command", std::string{"printf hi"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 1u);
  const auto* code = integer(out->rows[0], "exit_code");
  ASSERT_NE(code, nullptr);
  EXPECT_EQ(*code, 0);
  const auto* out_bytes = text(out->rows[0], "stdout");
  ASSERT_NE(out_bytes, nullptr);
  EXPECT_EQ(*out_bytes, "hi");
}

TEST_F(ShellToolTest, FailingCommandReportsNonzeroExitNotAToolError) {
  ShellTool tool = shell(std::chrono::seconds{5});
  auto out = call(tool, *box_, {{"command", std::string{"exit 7"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 1u);
  const auto* code = integer(out->rows[0], "exit_code");
  ASSERT_NE(code, nullptr);
  EXPECT_EQ(*code, 7);
}

TEST_F(ShellToolTest, WriteInsideRootIsVisibleAndConfinedOutsideIsNot) {
  ShellTool tool = shell(std::chrono::seconds{5});
  auto out =
      call(tool, *box_, {{"command", std::string{"touch inside && touch /root_of_no_return"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_TRUE(fs::exists(tmp_ / "inside"));
  EXPECT_FALSE(fs::exists("/root_of_no_return"));
  const auto* code = integer(out->rows[0], "exit_code");
  ASSERT_NE(code, nullptr);
  EXPECT_NE(*code, 0) << "the second touch, outside the root, should have been denied";
}

TEST_F(ShellToolTest, TimeoutIsAToolErrorNotAToolOutput) {
  ShellTool tool = shell(std::chrono::milliseconds{200});
  auto out = call(tool, *box_, {{"command", std::string{"sleep 30"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("killed"), std::string::npos)
      << out.error().reason;  // R8's own vocabulary ("a timeout treated as a failure"), not a crash
}

TEST_F(ShellToolTest, TimeoutErrorReportsWhatWasCapturedBeforeTheKill) {
  // A retry decides differently on "produced nothing" vs "was partway through something useful"
  // -- discarding the partial capture on the error path would waste what run_confined already
  // had before the kill.
  ShellTool tool = shell(std::chrono::milliseconds{300});
  auto out = call(tool, *box_, {{"command", std::string{"printf started; sleep 30"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("7 bytes stdout"), std::string::npos) << out.error().reason;
}

TEST_F(ShellToolTest, StderrIsCapturedSeparatelyFromStdout) {
  ShellTool tool = shell(std::chrono::seconds{5});
  auto out = call(tool, *box_,
                  {{"command", std::string{"printf out_bytes; printf err_bytes 1>&2"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  const auto* stdout_bytes = text(out->rows[0], "stdout");
  const auto* stderr_bytes = text(out->rows[0], "stderr");
  ASSERT_NE(stdout_bytes, nullptr);
  ASSERT_NE(stderr_bytes, nullptr);
  EXPECT_EQ(*stdout_bytes, "out_bytes");
  EXPECT_EQ(*stderr_bytes, "err_bytes");
  const auto* truncated = boolean(out->rows[0], "stdout_truncated");
  ASSERT_NE(truncated, nullptr);
  EXPECT_FALSE(*truncated);
}

// --- the pre-fork fd audit, seen from the tool -------------------------------------
// D10: "audit /proc/self/fd before the first spawn rather than trusting that." These two are
// the same fact from both sides: an inheritable descriptor the tool was not told about refuses
// the call before any fork; the same descriptor, named, crosses.

TEST_F(ShellToolTest, AnInheritableFdTheToolWasNotToldAboutFailsClosed) {
  const int fd = open_inheritable("leak");
  ASSERT_GE(fd, 0);
  ShellTool tool = shell(std::chrono::seconds{5});  // baseline only: `fd` is not in it
  auto out = call(tool, *box_, {{"command", std::string{"printf hi"}}});
  ::close(fd);
  ASSERT_FALSE(out.has_value()) << "the audit should have refused before forking";
  EXPECT_NE(out.error().reason.find("inherited-fd"), std::string::npos) << out.error().reason;
  EXPECT_NE(out.error().reason.find("fd " + std::to_string(fd)), std::string::npos)
      << out.error().reason;
}

TEST_F(ShellToolTest, AnInheritableFdNamedInTheAllowedListIsPermitted) {
  const int fd = open_inheritable("leak");
  ASSERT_GE(fd, 0);
  std::vector<int> allowed = baseline_;
  allowed.push_back(fd);
  ShellTool tool{tmp_, std::chrono::seconds{5}, kDefaultMaxReadBytes, allowed};
  auto out = call(tool, *box_, {{"command", std::string{"printf hi"}}});
  ::close(fd);
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 1u);
  const auto* out_bytes = text(out->rows[0], "stdout");
  ASSERT_NE(out_bytes, nullptr);
  EXPECT_EQ(*out_bytes, "hi");
}
