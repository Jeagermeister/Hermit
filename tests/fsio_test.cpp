#include <hermit/core/fsio.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using hermit::Fd;
using hermit::IoError;
using hermit::open_in_root;
using hermit::open_parent_in_root;
using hermit::read_file;
using hermit::Sandbox;

namespace {

class FsioTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermit_fsio_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));

    fs::create_directories(tmp_ / "root" / "sub");
    fs::create_directories(tmp_ / "outside");
    write_file(tmp_ / "root" / "plain.txt", "plain contents\n");
    write_file(tmp_ / "outside" / "secret.txt", "secret");

    auto sb = Sandbox::open(tmp_ / "root");
    ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
    box_ = std::make_unique<Sandbox>(std::move(*sb));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  static void write_file(const fs::path& p, std::string_view contents) {
    std::ofstream out(p, std::ios::binary);
    out << contents;
  }

  fs::path tmp_;
  std::unique_ptr<Sandbox> box_;
};

TEST_F(FsioTest, ReadFileReturnsExactBytes) {
  auto p = box_->resolve("plain.txt");
  ASSERT_TRUE(p.has_value());
  auto file = read_file(*p);
  ASSERT_TRUE(file.has_value()) << to_string(file.error());
  EXPECT_EQ(file->bytes, "plain contents\n");
  EXPECT_TRUE(S_ISREG(file->meta.st_mode));
  EXPECT_EQ(static_cast<std::size_t>(file->meta.st_size), file->bytes.size());
}

TEST_F(FsioTest, ReadFileKeepsEmbeddedNulBytes) {
  const std::string_view binary{"b\0in\0!", 6};
  write_file(tmp_ / "root" / "blob.bin", binary);
  auto p = box_->resolve("blob.bin");
  ASSERT_TRUE(p.has_value());
  auto file = read_file(*p);
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->bytes.size(), 6u);
  EXPECT_EQ(file->bytes, std::string(binary));
}

TEST_F(FsioTest, ReadFileRefusesADirectory) {
  auto p = box_->resolve("sub");
  ASSERT_TRUE(p.has_value());
  auto file = read_file(*p);
  ASSERT_FALSE(file.has_value());
  EXPECT_EQ(to_string(file.error()), "not a regular file");
}

TEST_F(FsioTest, ReadFileRefusesAFifoInsteadOfBlocking) {
  ASSERT_EQ(::mkfifo((tmp_ / "root" / "pipe").c_str(), 0600), 0);
  auto p = box_->resolve("pipe");
  ASSERT_TRUE(p.has_value());
  // Must return, not block waiting for a writer -- the suite's 30s timeout
  // is the backstop that makes a regression here loud.
  auto file = read_file(*p);
  ASSERT_FALSE(file.has_value());
  EXPECT_EQ(to_string(file.error()), "not a regular file");
}

TEST_F(FsioTest, ReadFileMissingReportsEnoent) {
  auto p = box_->resolve("nope.txt");
  ASSERT_TRUE(p.has_value()) << "nonexistent paths resolve; writes create files";
  auto file = read_file(*p);
  ASSERT_FALSE(file.has_value());
  EXPECT_EQ(file.error().code, ENOENT);
}

TEST_F(FsioTest, OpenRefusesAFinalComponentSwappedToASymlink) {
  // Resolve while plain.txt is a regular file, then retarget the name to a
  // symlink pointing outside the root -- the post-resolution swap D6 accepts
  // as a race. O_NOFOLLOW turns the cheap slice of it into a refusal.
  auto p = box_->resolve("plain.txt");
  ASSERT_TRUE(p.has_value());
  fs::remove(tmp_ / "root" / "plain.txt");
  fs::create_symlink(tmp_ / "outside" / "secret.txt", tmp_ / "root" / "plain.txt");

  auto fd = open_in_root(*p, O_RDONLY);
  ASSERT_FALSE(fd.has_value());
  EXPECT_EQ(fd.error().code, ELOOP) << "O_NOFOLLOW refuses, never follows";
}

TEST_F(FsioTest, OpenInRootRefusesAnInteriorComponentSwappedToASymlink) {
  // The primitive-level proof of D6's worked example: resolve while sub2 is a real
  // directory, then retarget sub2 itself -- not the final component -- to a symlink
  // pointing outside the root. open_in_root re-walks from sandbox_root() and must
  // refuse at that hop rather than follow it to a file the resolved path never named.
  fs::create_directories(tmp_ / "root" / "sub2");
  write_file(tmp_ / "root" / "sub2" / "target.txt", "inside");
  auto p = box_->resolve("sub2/target.txt");
  ASSERT_TRUE(p.has_value());

  fs::remove(tmp_ / "root" / "sub2" / "target.txt");
  fs::remove(tmp_ / "root" / "sub2");
  fs::create_symlink(tmp_ / "outside", tmp_ / "root" / "sub2");

  auto fd = open_in_root(*p, O_RDONLY);
  ASSERT_FALSE(fd.has_value());
  // Interior hops are walked with O_DIRECTORY|O_NOFOLLOW, not O_NOFOLLOW alone as the
  // final-component open above uses -- verified on this kernel to report ENOTDIR for a
  // symlinked directory component, not ELOOP. Either way it is Kind::Kernel and refused,
  // never followed: the distinction is real but doesn't change what the primitive does.
  EXPECT_EQ(fd.error().code, ENOTDIR) << "an interior swap is refused, never followed";
}

TEST_F(FsioTest, OpenParentInRootRefusesAnInteriorComponentSwappedToASymlink) {
  // Same attack, through the publish-side walk: proves it gets identical protection.
  fs::create_directories(tmp_ / "root" / "sub2");
  write_file(tmp_ / "root" / "sub2" / "target.txt", "inside");
  auto p = box_->resolve("sub2/target.txt");
  ASSERT_TRUE(p.has_value());

  fs::remove(tmp_ / "root" / "sub2" / "target.txt");
  fs::remove(tmp_ / "root" / "sub2");
  fs::create_symlink(tmp_ / "outside", tmp_ / "root" / "sub2");

  auto parent = open_parent_in_root(*p, /*create_missing=*/false);
  ASSERT_FALSE(parent.has_value());
  EXPECT_EQ(parent.error().code, ENOTDIR);  // see the sibling open_in_root test above
}

TEST_F(FsioTest, OpenParentInRootCreatesMissingIntermediateDirectories) {
  auto p = box_->resolve("a/b/c/file.txt");
  ASSERT_TRUE(p.has_value());

  auto parent = open_parent_in_root(*p, /*create_missing=*/true);
  ASSERT_TRUE(parent.has_value()) << to_string(parent.error());
  EXPECT_EQ(parent->name, "file.txt");
  EXPECT_TRUE(fs::is_directory(tmp_ / "root" / "a" / "b" / "c"));

  struct ::stat st {};
  EXPECT_EQ(::fstatat(parent->dir.get(), "file.txt", &st, AT_SYMLINK_NOFOLLOW), -1);
  EXPECT_EQ(errno, ENOENT) << "the parent is created, the leaf is not";
}

TEST_F(FsioTest, OpenParentInRootWithoutCreateMissingRefusesAMissingDirectory) {
  auto p = box_->resolve("a/b/file.txt");
  ASSERT_TRUE(p.has_value());

  auto parent = open_parent_in_root(*p, /*create_missing=*/false);
  ASSERT_FALSE(parent.has_value());
  EXPECT_EQ(parent.error().code, ENOENT);
  EXPECT_FALSE(fs::exists(tmp_ / "root" / "a"));
}

TEST_F(FsioTest, OpenParentInRootRefusesTheSandboxRootItselfAsATarget) {
  auto p = box_->resolve(".");
  ASSERT_TRUE(p.has_value());

  auto parent = open_parent_in_root(*p, /*create_missing=*/false);
  ASSERT_FALSE(parent.has_value());
  EXPECT_EQ(parent.error().kind, IoError::Kind::Refused);
}

TEST_F(FsioTest, FdOwnershipMovesAndReleases) {
  auto p = box_->resolve("plain.txt");
  ASSERT_TRUE(p.has_value());
  auto fd = open_in_root(*p, O_RDONLY);
  ASSERT_TRUE(fd.has_value());
  ASSERT_TRUE(fd->valid());

  Fd moved = std::move(*fd);
  EXPECT_FALSE(fd->valid()) << "moved-from Fd no longer owns the descriptor";
  EXPECT_TRUE(moved.valid());

  const int raw = moved.release();
  EXPECT_FALSE(moved.valid());
  EXPECT_EQ(::close(raw), 0) << "released descriptor was still open and ours";
}

}  // namespace
