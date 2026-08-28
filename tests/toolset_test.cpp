#include <hermit/app/toolset.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

#include <hermit/core/sandbox.h>

namespace fs = std::filesystem;
using hermit::Sandbox;
using hermit::app::resolve_backup_dir;
using hermit::app::to_string;

namespace {

// <tmp>/root/  is the sandbox root; <tmp>/ itself is a natural "beside the root"
// location callers reach for, and <tmp>/root/nested is a natural mistake to refuse.
class ToolsetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermit_tsbx_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));
    root_ = tmp_ / "root";
    fs::create_directories(root_);

    auto sb = Sandbox::open(root_);
    ASSERT_TRUE(sb.has_value());
    box_ = std::make_unique<Sandbox>(std::move(*sb));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  fs::path tmp_, root_;
  std::unique_ptr<Sandbox> box_;
};

TEST_F(ToolsetTest, DefaultsBesideTheRootWhenNoBackupDirIsGiven) {
  const auto store = resolve_backup_dir(*box_, std::nullopt);
  ASSERT_TRUE(store.has_value());
  EXPECT_EQ(*store, root_.parent_path() / (".hermit-backups-" + root_.filename().string()));
}

TEST_F(ToolsetTest, AcceptsAnExplicitPathOutsideTheRoot) {
  const fs::path explicit_dir = tmp_ / "elsewhere";
  const auto store = resolve_backup_dir(*box_, explicit_dir);
  ASSERT_TRUE(store.has_value());
  EXPECT_EQ(*store, explicit_dir);
}

TEST_F(ToolsetTest, RefusesAPathInsideTheRoot) {
  const auto store = resolve_backup_dir(*box_, root_ / "nested-backups");
  ASSERT_FALSE(store.has_value());
  EXPECT_EQ(store.error().root, root_);
  EXPECT_EQ(store.error().store, root_ / "nested-backups");
}

TEST_F(ToolsetTest, RefusesTheRootItself) {
  const auto store = resolve_backup_dir(*box_, root_);
  ASSERT_FALSE(store.has_value());
}

TEST_F(ToolsetTest, ToStringNamesBothPathsForTheCallersMessage) {
  const auto store = resolve_backup_dir(*box_, root_ / "nested-backups");
  ASSERT_FALSE(store.has_value());
  const std::string message = to_string(store.error());
  EXPECT_NE(message.find((root_ / "nested-backups").string()), std::string::npos);
  EXPECT_NE(message.find(root_.string()), std::string::npos);
}

}  // namespace
