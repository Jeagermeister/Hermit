// R4's operator side -- enumeration, restore and retention over a real store
// on a real filesystem, because every policy in undo.h is about what happens
// to bytes on disk.
//
// The store under test is written by the real BackupStore, not hand-built:
// what undo reads must be what preserve writes, and building the layout by
// hand would let the two drift apart with both sides' tests still green. The
// hand-edited cases (ambiguity, escape) are then deliberate corruptions of a
// store that started out genuine.

#include <hermit/supervisor/undo.h>

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>  // mkdtemp
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <hermit/core/backup.h>
#include <hermit/core/sandbox.h>

namespace fs = std::filesystem;
using hermit::BackupStore;
using hermit::kStoreMarker;
using hermit::Sandbox;
using hermit::supervisor::enumerate;
using hermit::supervisor::Generation;
using hermit::supervisor::prune;
using hermit::supervisor::restore;

using namespace std::chrono_literals;

namespace {

class UndoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermit_undo_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));

    fs::create_directories(tmp_ / "root");
    auto sb = Sandbox::open(tmp_ / "root");
    ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
    box_ = std::make_unique<Sandbox>(std::move(*sb));
    store_dir_ = tmp_ / "backups";
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  static void write_file(const fs::path& p, std::string_view contents) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << contents;
  }

  static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
  }

  /// Preserve through the real store, the way the write and edit tools do.
  void preserve(std::string_view relative, std::string_view bytes) {
    BackupStore store{store_dir_};
    auto kept = store.preserve(fs::path{std::string{relative}}, bytes);
    ASSERT_TRUE(kept.has_value()) << to_string(kept.error());
  }

  fs::path tmp_;
  fs::path store_dir_;
  std::unique_ptr<Sandbox> box_;
};

TEST_F(UndoTest, AMissingStoreEnumeratesEmpty) {
  const auto rows = enumerate(store_dir_);
  ASSERT_TRUE(rows.has_value()) << rows.error();
  EXPECT_TRUE(rows->empty());
}

TEST_F(UndoTest, TheFirstPreservationMarksTheStore) {
  preserve("a.txt", "old");
  EXPECT_TRUE(fs::exists(store_dir_ / kStoreMarker));
}

TEST_F(UndoTest, AnUnmarkedDirectoryIsRefusedByEveryOperation) {
  // A directory that merely contains numeric names -- what --backups pointed
  // at the wrong place looks like. Nothing may list it, and nothing may
  // delete from it.
  fs::create_directories(store_dir_ / "2024");
  write_file(store_dir_ / "2024" / "taxes.txt", "important");

  const auto rows = enumerate(store_dir_);
  ASSERT_FALSE(rows.has_value());
  EXPECT_NE(rows.error().find(std::string{kStoreMarker}), std::string::npos);

  const auto removed = prune(store_dir_, 72h, fs::file_time_type::clock::now());
  ASSERT_FALSE(removed.has_value());
  EXPECT_TRUE(fs::exists(store_dir_ / "2024" / "taxes.txt"));

  const auto put_back = restore(store_dir_, *box_, 2024);
  EXPECT_FALSE(put_back.has_value());
}

TEST_F(UndoTest, GenerationsAreListedOldestFirstWithTheirPaths) {
  preserve("a.txt", "v1");
  preserve("b/c.txt", "second");

  const auto rows = enumerate(store_dir_);
  ASSERT_TRUE(rows.has_value()) << rows.error();
  ASSERT_EQ(rows->size(), 2u);
  EXPECT_EQ((*rows)[0].seq, 0u);
  EXPECT_EQ((*rows)[0].name, "0000");
  EXPECT_EQ((*rows)[0].relative, fs::path{"a.txt"});
  EXPECT_EQ((*rows)[0].bytes, 2u);
  EXPECT_EQ((*rows)[1].seq, 1u);
  EXPECT_EQ((*rows)[1].relative, fs::path{"b/c.txt"});
  EXPECT_EQ((*rows)[1].bytes, 6u);
}

TEST_F(UndoTest, RestorePutsTheBytesBackAndPreservesTheCurrentContent) {
  write_file(tmp_ / "root" / "a.txt", "the new content");
  preserve("a.txt", "the old content");

  const auto result = restore(store_dir_, *box_, 0);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(slurp(tmp_ / "root" / "a.txt"), "the old content");

  // Backup-before-mutate applied to the restore itself: the overwritten bytes
  // became a new generation, so this restore is undoable too.
  ASSERT_TRUE(result->preserved.has_value());
  EXPECT_EQ(slurp(*result->preserved), "the new content");
  const auto rows = enumerate(store_dir_);
  ASSERT_TRUE(rows.has_value()) << rows.error();
  ASSERT_EQ(rows->size(), 2u);
  EXPECT_EQ(rows->back().relative, fs::path{"a.txt"});
}

TEST_F(UndoTest, RestoringTheRestoreRoundTrips) {
  write_file(tmp_ / "root" / "a.txt", "new");
  preserve("a.txt", "old");

  auto first = restore(store_dir_, *box_, 0);
  ASSERT_TRUE(first.has_value()) << first.error();
  ASSERT_EQ(slurp(tmp_ / "root" / "a.txt"), "old");

  // The redo: generation 0001 now holds "new"; restoring it brings it back.
  auto second = restore(store_dir_, *box_, 1);
  ASSERT_TRUE(second.has_value()) << second.error();
  EXPECT_EQ(slurp(tmp_ / "root" / "a.txt"), "new");
}

TEST_F(UndoTest, RestoreOfAMissingFileRecreatesItAndPreservesNothing) {
  preserve("gone.txt", "the erased bytes");
  ASSERT_FALSE(fs::exists(tmp_ / "root" / "gone.txt"));

  const auto result = restore(store_dir_, *box_, 0);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(slurp(tmp_ / "root" / "gone.txt"), "the erased bytes");
  EXPECT_FALSE(result->preserved.has_value());

  const auto rows = enumerate(store_dir_);
  ASSERT_TRUE(rows.has_value()) << rows.error();
  EXPECT_EQ(rows->size(), 1u);  // absence preserved nothing
}

TEST_F(UndoTest, RestoreRecreatesDeletedParentDirectories) {
  write_file(tmp_ / "root" / "sub" / "deep.txt", "current");
  preserve("sub/deep.txt", "preserved");
  fs::remove_all(tmp_ / "root" / "sub");

  const auto result = restore(store_dir_, *box_, 0);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(slurp(tmp_ / "root" / "sub" / "deep.txt"), "preserved");
}

TEST_F(UndoTest, RestoreKeepsTheTargetsMode) {
  const fs::path target = tmp_ / "root" / "script.sh";
  write_file(target, "#!/bin/sh\necho new\n");
  ASSERT_EQ(::chmod(target.c_str(), 0750), 0);
  preserve("script.sh", "#!/bin/sh\necho old\n");

  const auto result = restore(store_dir_, *box_, 0);
  ASSERT_TRUE(result.has_value()) << result.error();
  struct ::stat st {};
  ASSERT_EQ(::stat(target.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0750u);
}

TEST_F(UndoTest, AHandEditedEscapePathIsRefused) {
  // The store said "link", and inside the root "link" is a symlink pointing
  // out. R1 resolution must catch it on the way back exactly as it would have
  // on the way in.
  fs::create_directories(tmp_ / "outside");
  fs::create_symlink(tmp_ / "outside", tmp_ / "root" / "link");
  preserve("innocent.txt", "bytes");
  fs::rename(store_dir_ / "0000" / "innocent.txt", store_dir_ / "0000" / "link");

  const auto result = restore(store_dir_, *box_, 0);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("does not resolve"), std::string::npos)
      << result.error();
  EXPECT_FALSE(fs::exists(tmp_ / "outside" / "link"));
}

TEST_F(UndoTest, AnAmbiguousGenerationIsRefused) {
  preserve("a.txt", "genuine");
  write_file(store_dir_ / "0000" / "planted.txt", "hand-edited");

  const auto result = restore(store_dir_, *box_, 0);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("2 files"), std::string::npos) << result.error();
}

TEST_F(UndoTest, RestoreRefusesToOverwriteADirectory) {
  preserve("thing", "was a file");
  fs::create_directories(tmp_ / "root" / "thing");

  const auto result = restore(store_dir_, *box_, 0);
  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(fs::is_directory(tmp_ / "root" / "thing"));
}

TEST_F(UndoTest, RestoreOfAnUnknownGenerationSaysSo) {
  preserve("a.txt", "old");
  const auto result = restore(store_dir_, *box_, 42);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("no generation 42"), std::string::npos)
      << result.error();
}

TEST_F(UndoTest, PruneRemovesOnlyGenerationsPastTheCutoff) {
  preserve("old.txt", "aged bytes");
  preserve("new.txt", "fresh bytes");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(store_dir_ / "0000" / "old.txt", now - 100h);

  const auto removed = prune(store_dir_, 72h, now);
  ASSERT_TRUE(removed.has_value()) << removed.error();
  EXPECT_EQ(removed->generations, 1u);
  EXPECT_EQ(removed->bytes, 10u);
  EXPECT_FALSE(fs::exists(store_dir_ / "0000"));
  EXPECT_TRUE(fs::exists(store_dir_ / "0001" / "new.txt"));
}

TEST_F(UndoTest, PruneTouchesOnlyNumericDirectories) {
  preserve("old.txt", "aged bytes");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(store_dir_ / "0000" / "old.txt", now - 100h);
  // Things an operator might drop beside the generations: none are ours.
  write_file(store_dir_ / "notes" / "why.txt", "keep me");
  fs::last_write_time(store_dir_ / "notes" / "why.txt", now - 100h);
  write_file(store_dir_ / "9999", "a numeric-named FILE, not a generation");
  fs::last_write_time(store_dir_ / "9999", now - 100h);

  const auto removed = prune(store_dir_, 72h, now);
  ASSERT_TRUE(removed.has_value()) << removed.error();
  EXPECT_EQ(removed->generations, 1u);
  EXPECT_TRUE(fs::exists(store_dir_ / "notes" / "why.txt"));
  EXPECT_TRUE(fs::exists(store_dir_ / "9999"));
  EXPECT_TRUE(fs::exists(store_dir_ / kStoreMarker));
}

TEST_F(UndoTest, PruneAgesAnEmptyGenerationByItsOwnMtime) {
  preserve("a.txt", "x");  // marks the store
  const auto now = fs::file_time_type::clock::now();
  fs::create_directories(store_dir_ / "0007");
  fs::last_write_time(store_dir_ / "0007", now - 100h);

  const auto removed = prune(store_dir_, 72h, now);
  ASSERT_TRUE(removed.has_value()) << removed.error();
  EXPECT_EQ(removed->generations, 1u);
  EXPECT_FALSE(fs::exists(store_dir_ / "0007"));
  EXPECT_TRUE(fs::exists(store_dir_ / "0000" / "a.txt"));
}

TEST_F(UndoTest, AFreshGenerationInAnAgedDirectorySurvives) {
  // The parent directory's mtime moves when files land in it; only the FILES
  // date a generation. A store whose directory was restocked a moment ago
  // must still prune its old generations -- and the inverse: a young file in
  // a directory with an old mtime survives.
  preserve("young.txt", "fresh");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(store_dir_ / "0000", now - 100h);  // dir aged, file fresh

  const auto removed = prune(store_dir_, 72h, now);
  ASSERT_TRUE(removed.has_value()) << removed.error();
  EXPECT_EQ(removed->generations, 0u);
  EXPECT_TRUE(fs::exists(store_dir_ / "0000" / "young.txt"));
}

TEST_F(UndoTest, PruneOfAMissingStoreIsANoOp) {
  const auto removed = prune(store_dir_, 72h, fs::file_time_type::clock::now());
  ASSERT_TRUE(removed.has_value()) << removed.error();
  EXPECT_EQ(removed->generations, 0u);
}

TEST_F(UndoTest, NumberingContinuesAcrossPruning) {
  // Retention must never cause generation reuse: pruning 0000 and then
  // preserving again continues at 0002, because a reused name would let a new
  // backup silently take an old one's identity in the operator's listing.
  preserve("a.txt", "first");
  preserve("b.txt", "second");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(store_dir_ / "0000" / "a.txt", now - 100h);
  const auto removed = prune(store_dir_, 72h, now);
  ASSERT_TRUE(removed.has_value()) << removed.error();
  ASSERT_EQ(removed->generations, 1u);

  preserve("c.txt", "third");
  const auto rows = enumerate(store_dir_);
  ASSERT_TRUE(rows.has_value()) << rows.error();
  ASSERT_EQ(rows->size(), 2u);
  EXPECT_EQ(rows->back().name, "0002");
}

TEST_F(UndoTest, NumberingSurvivesATotalPrune) {
  // The reviewer's demonstrated failure, pinned: retention that removes EVERY
  // generation -- the normal state of any store idle past the window -- must not let
  // the next preservation restart at 0000 and take a pruned backup's identity. The
  // floor lives in the marker and outlives the scan.
  preserve("good-config.ini", "the good bytes");
  preserve("also.txt", "more");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(store_dir_ / "0000" / "good-config.ini", now - 100h);
  fs::last_write_time(store_dir_ / "0001" / "also.txt", now - 100h);

  const auto removed = prune(store_dir_, 72h, now);
  ASSERT_TRUE(removed.has_value()) << removed.error();
  ASSERT_EQ(removed->generations, 2u);

  preserve("evil-output.bin", "new bytes");
  const auto rows = enumerate(store_dir_);
  ASSERT_TRUE(rows.has_value()) << rows.error();
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(rows->front().name, "0002") << "numbering rewound across a total prune";
}

TEST_F(UndoTest, AnOverflowLengthNumericNameIsNotAGeneration) {
  // 25 digits is numeric but nothing generation_name could ever have written; parsing
  // it modulo 2^64 would collide it with generation 0000 in the listing, and prune
  // must not treat a directory the store could not have created as its own to delete.
  preserve("a.txt", "real");
  const std::string alien = "9999999999999999999999999";
  write_file(store_dir_ / alien / "planted.txt", "not ours");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(store_dir_ / alien / "planted.txt", now - 100h);

  const auto rows = enumerate(store_dir_);
  ASSERT_TRUE(rows.has_value()) << rows.error();
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(rows->front().relative, fs::path{"a.txt"});

  const auto removed = prune(store_dir_, 72h, now);
  ASSERT_TRUE(removed.has_value()) << removed.error();
  EXPECT_EQ(removed->generations, 0u);
  EXPECT_TRUE(fs::exists(store_dir_ / alien / "planted.txt"));
}

}  // namespace
