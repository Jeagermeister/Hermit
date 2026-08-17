// Per-turn state verification -- ROUTING.md section 6's half that holds unconditionally.
//
// Split like the code: `diff` is pure over two snapshots, so the whole classification
// table is exercised without a filesystem; the walk needs a real tree and gets one.

#include <hermes/supervisor/verify.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using hermes::IdentityTuple;
using hermes::Sandbox;
using hermes::supervisor::Change;
using hermes::supervisor::ChangeKind;
using hermes::supervisor::Changeset;
using hermes::supervisor::diff;
using hermes::supervisor::FileState;
using hermes::supervisor::TreeSnapshot;
using hermes::supervisor::TreeVerifier;

namespace {

FileState regular(std::string hash, std::uint64_t ino = 1, std::int64_t ctime = 100) {
  FileState state;
  state.sha256 = std::move(hash);
  state.tuple = IdentityTuple{.dev = 1, .ino = ino, .size = 10, .mtime_ns = 50, .ctime_ns = ctime};
  return state;
}

FileState directory(std::uint64_t ino = 2) {
  FileState state;
  state.is_dir = true;
  state.tuple = IdentityTuple{.dev = 1, .ino = ino};
  return state;
}

const Change* find_change(const Changeset& set, std::string_view path) {
  for (const Change& c : set.changes) {
    if (c.path == path) return &c;
  }
  return nullptr;
}

}  // namespace

// --- the classification table, pure ------------------------------------------

TEST(VerifyDiff, ReportsNothingForAnUnchangedTree) {
  TreeSnapshot before{{"a.txt", regular("aaa")}, {"dir", directory()}};
  EXPECT_TRUE(diff(before, before).empty());
}

TEST(VerifyDiff, ReportsACreatedFileWithItsNewHashAndNoOldOne) {
  TreeSnapshot before;
  TreeSnapshot after{{"new.txt", regular("bbb")}};

  const auto set = diff(before, after);
  ASSERT_EQ(set.changes.size(), 1u);
  EXPECT_EQ(set.changes[0].kind, ChangeKind::Created);
  EXPECT_TRUE(set.changes[0].before.empty());
  EXPECT_EQ(set.changes[0].after, "bbb");
}

TEST(VerifyDiff, ReportsADeletedFileWithItsOldHashAndNoNewOne) {
  TreeSnapshot before{{"gone.txt", regular("ccc")}};
  const auto set = diff(before, TreeSnapshot{});

  ASSERT_EQ(set.changes.size(), 1u);
  EXPECT_EQ(set.changes[0].kind, ChangeKind::Deleted);
  EXPECT_EQ(set.changes[0].before, "ccc");
  EXPECT_TRUE(set.changes[0].after.empty());
}

TEST(VerifyDiff, ContentChangeIsModifiedAndCarriesBothHashes) {
  TreeSnapshot before{{"f.txt", regular("old", 1, 100)}};
  TreeSnapshot after{{"f.txt", regular("new", 1, 200)}};

  const auto set = diff(before, after);
  ASSERT_EQ(set.changes.size(), 1u);
  EXPECT_EQ(set.changes[0].kind, ChangeKind::Modified);
  EXPECT_EQ(set.changes[0].before, "old");
  EXPECT_EQ(set.changes[0].after, "new");
}

TEST(VerifyDiff, AMovedTupleWithIdenticalContentIsTouchedNotModified) {
  // R3's rule read in the direction people forget: a changed mtime/ctime is not evidence
  // that bytes moved. Folding this into Modified would make every report noisier than the
  // signal in it, and `05_copy`'s lesson is that the report has to stay trustworthy.
  TreeSnapshot before{{"f.txt", regular("same", 1, 100)}};
  TreeSnapshot after{{"f.txt", regular("same", 1, 900)}};

  const auto set = diff(before, after);
  ASSERT_EQ(set.changes.size(), 1u);
  EXPECT_EQ(set.changes[0].kind, ChangeKind::TouchedOnly);
  EXPECT_EQ(set.substantive(), 0u) << "a touch moved no bytes";
}

TEST(VerifyDiff, UnlinkAndRecreateWithTheSameContentIsStillOnlyATouch) {
  // dev:ino moves, content does not. Worth pinning: the tuple is what makes the
  // incremental hashing safe, and this is the case where it fires without bytes moving.
  TreeSnapshot before{{"f.txt", regular("same", /*ino=*/1)}};
  TreeSnapshot after{{"f.txt", regular("same", /*ino=*/999)}};

  const auto set = diff(before, after);
  ASSERT_EQ(set.changes.size(), 1u);
  EXPECT_EQ(set.changes[0].kind, ChangeKind::TouchedOnly);
}

TEST(VerifyDiff, AFileReplacedByADirectoryIsATypeChange) {
  TreeSnapshot before{{"x", regular("aaa")}};
  TreeSnapshot after{{"x", directory()}};

  const auto set = diff(before, after);
  ASSERT_EQ(set.changes.size(), 1u);
  EXPECT_EQ(set.changes[0].kind, ChangeKind::TypeChanged);
  EXPECT_EQ(set.substantive(), 1u);
}

TEST(VerifyDiff, BecomingUnreadableIsReportedRatherThanSwallowed) {
  FileState unreadable = regular("");
  unreadable.readable = false;
  TreeSnapshot before{{"f.txt", regular("aaa")}};
  TreeSnapshot after{{"f.txt", unreadable}};

  const auto set = diff(before, after);
  ASSERT_EQ(set.changes.size(), 1u);
  EXPECT_EQ(set.changes[0].kind, ChangeKind::ReadabilityChanged);
}

TEST(VerifyDiff, TwoFilesThatStayUnreadableAcrossATurnAreNotAChange) {
  FileState unreadable = regular("");
  unreadable.readable = false;
  TreeSnapshot both{{"f.txt", unreadable}};
  EXPECT_TRUE(diff(both, both).empty());
}

TEST(VerifyDiff, CountsOnlyByteMovingChangesAsSubstantive) {
  TreeSnapshot before{{"a", regular("1", 1, 1)}, {"b", regular("2")}, {"c", regular("3")}};
  TreeSnapshot after{{"a", regular("1", 1, 77)},   // touched
                     {"b", regular("CHANGED")},    // modified
                     {"d", regular("4")}};         // c deleted, d created

  const auto set = diff(before, after);
  EXPECT_EQ(set.changes.size(), 4u);
  EXPECT_EQ(set.substantive(), 3u);
}

TEST(VerifyDiff, ReportsInPathOrderSoTwoRunsRenderIdentically) {
  TreeSnapshot before;
  TreeSnapshot after{{"zeta.txt", regular("z")}, {"alpha.txt", regular("a")},
                     {"middle/beta.txt", regular("b")}};

  const auto set = diff(before, after);
  ASSERT_EQ(set.changes.size(), 3u);
  EXPECT_EQ(set.changes[0].path, "alpha.txt");
  EXPECT_EQ(set.changes[1].path, "middle/beta.txt");
  EXPECT_EQ(set.changes[2].path, "zeta.txt");
}

TEST(VerifyDiff, RenderAbbreviatesHashesWithoutLosingTheRealOnes) {
  TreeSnapshot before{{"f.txt", regular("0123456789abcdefdeadbeef")}};
  TreeSnapshot after{{"f.txt", regular("fedcba9876543210cafebabe")}};

  const auto set = diff(before, after);
  const std::string text = set.render();
  EXPECT_NE(text.find("modified"), std::string::npos);
  EXPECT_NE(text.find("0123456789ab"), std::string::npos);
  EXPECT_EQ(text.find("0123456789abcdefdeadbeef"), std::string::npos) << "render abbreviates";
  EXPECT_EQ(set.changes[0].before, "0123456789abcdefdeadbeef") << "the full digest is kept";
  EXPECT_EQ(text.back(), '\n');
}

// --- the walk, against a real tree -------------------------------------------

namespace {

class VerifyFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermes_verify_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr);
    root_ = fs::path{buf.data()};
    fs::create_directories(root_ / "data");
    write("notes.txt", "alpha\nbeta\n");
    write("data/counts.txt", "1\n2\n3\n");

    auto box = Sandbox::open(root_);
    ASSERT_TRUE(box.has_value());
    sandbox_ = std::make_unique<Sandbox>(std::move(*box));
    verifier_ = std::make_unique<TreeVerifier>(*sandbox_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  void write(const std::string& rel, std::string_view bytes) {
    std::ofstream out{root_ / rel, std::ios::binary | std::ios::trunc};
    out << bytes;
  }

  fs::path root_;
  std::unique_ptr<Sandbox> sandbox_;
  std::unique_ptr<TreeVerifier> verifier_;
};

}  // namespace

TEST_F(VerifyFixture, ABaselineRecordsEveryEntryWithAHashForEachRegularFile) {
  const auto snap = verifier_->snapshot();
  ASSERT_TRUE(snap.has_value()) << snap.error().message();

  ASSERT_TRUE(snap->contains("notes.txt"));
  ASSERT_TRUE(snap->contains("data"));
  ASSERT_TRUE(snap->contains("data/counts.txt"));
  EXPECT_TRUE(snap->at("data").is_dir);
  EXPECT_TRUE(snap->at("data").sha256.empty()) << "a directory has no content hash";
  EXPECT_EQ(snap->at("notes.txt").sha256.size(), 64u) << "sha-256, hex";
  EXPECT_TRUE(snap->at("notes.txt").readable);
}

TEST_F(VerifyFixture, TheSecondWalkHashesOnlyWhatMoved) {
  // The performance claim in verify.h, asserted rather than hoped. A regression here is
  // silent otherwise: the diff stays correct and every turn just costs the whole tree.
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value());
  const std::uint64_t baseline_bytes = verifier_->last_hashed_bytes();
  EXPECT_GT(baseline_bytes, 0u);

  write("notes.txt", "CHANGED\n");
  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value());

  EXPECT_LT(verifier_->last_hashed_bytes(), baseline_bytes)
      << "the unchanged file was re-hashed";
  EXPECT_EQ(verifier_->last_hashed_bytes(), 8u) << "exactly the bytes of CHANGED\\n";
  EXPECT_EQ(verifier_->last_entries_walked(), before->size()) << "every entry is still stat'd";
}

TEST_F(VerifyFixture, ACarriedForwardHashIsTheSameHashAFullWalkWouldProduce) {
  // The optimisation must be invisible in the result, or it is not an optimisation.
  const auto incremental_base = verifier_->snapshot();
  ASSERT_TRUE(incremental_base.has_value());
  write("notes.txt", "CHANGED\n");

  const auto incremental = verifier_->snapshot(&*incremental_base);
  const auto cold = verifier_->snapshot(nullptr);
  ASSERT_TRUE(incremental.has_value());
  ASSERT_TRUE(cold.has_value());
  EXPECT_EQ(*incremental, *cold);
}

TEST_F(VerifyFixture, SeesAMutationNoToolReportedAndNoModelMentioned) {
  // The point of per-turn verification: it does not care who changed the tree, and it is
  // not reading anybody's account of it. Here nothing went through a tool at all.
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value());

  write("notes.txt", "overwritten behind the tools' back\n");
  write("sneaky.txt", "appeared from nowhere\n");
  fs::remove(root_ / "data" / "counts.txt");

  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value());
  const auto set = diff(*before, *after);

  ASSERT_NE(find_change(set, "notes.txt"), nullptr);
  EXPECT_EQ(find_change(set, "notes.txt")->kind, ChangeKind::Modified);
  EXPECT_EQ(find_change(set, "sneaky.txt")->kind, ChangeKind::Created);
  EXPECT_EQ(find_change(set, "data/counts.txt")->kind, ChangeKind::Deleted);
  EXPECT_EQ(set.substantive(), 3u);
}

TEST_F(VerifyFixture, CatchesTheDestructiveOverwriteR3WasWrittenAbout) {
  // `05_copy`: two models overwrote config.ini with invented content and produced no copy.
  // The assertion "config.ini still exists" passed for both. This is that scenario, and
  // the changeset says plainly what the existence check could not.
  write("config.ini", "[server]\nport=8080\n");
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value());

  write("config.ini", "(unchanged)");  // hermes3-8b's actual output
  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value());

  EXPECT_TRUE(fs::exists(root_ / "config.ini")) << "the existence check still passes";
  const auto set = diff(*before, *after);
  const Change* change = find_change(set, "config.ini");
  ASSERT_NE(change, nullptr) << "the hash check must not";
  EXPECT_EQ(change->kind, ChangeKind::Modified);
  EXPECT_NE(change->before, change->after);
  EXPECT_EQ(find_change(set, "config.ini.bak"), nullptr) << "and no copy was produced";
}

TEST_F(VerifyFixture, RecordsASymlinkWithoutFollowingIt) {
  fs::create_symlink("/etc/passwd", root_ / "escape");
  const auto snap = verifier_->snapshot();
  ASSERT_TRUE(snap.has_value()) << snap.error().message();

  ASSERT_TRUE(snap->contains("escape"));
  EXPECT_TRUE(snap->at("escape").is_symlink);
  EXPECT_TRUE(snap->at("escape").sha256.empty()) << "a symlink's target is never read";
}

TEST_F(VerifyFixture, RefusesTheWholeSnapshotWhenADirectoryCannotBeRead) {
  // Fail closed: a hidden subtree would make every file under it look deleted, which is a
  // confidently wrong changeset -- worse than no changeset.
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores the permission bits this test sets";
  fs::create_directories(root_ / "locked" / "inner");
  write("locked/inner/secret.txt", "x");
  ::chmod((root_ / "locked").c_str(), 0000);

  const auto snap = verifier_->snapshot();
  ::chmod((root_ / "locked").c_str(), 0755);  // restore so TearDown can clean up

  ASSERT_FALSE(snap.has_value());
  EXPECT_EQ(snap.error().kind, hermes::supervisor::VerifyErrorKind::DirectoryUnreadable);
  EXPECT_NE(snap.error().message().find("locked"), std::string::npos);
  EXPECT_NE(snap.error().message().find("no snapshot"), std::string::npos);
}

TEST_F(VerifyFixture, AnUnreadableFileIsRecordedRatherThanTreatedAsAbsent) {
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores the permission bits this test sets";
  write("private.txt", "secret");
  ::chmod((root_ / "private.txt").c_str(), 0000);

  const auto snap = verifier_->snapshot();
  ::chmod((root_ / "private.txt").c_str(), 0644);

  ASSERT_TRUE(snap.has_value()) << "one unreadable file is not fatal; a directory is";
  ASSERT_TRUE(snap->contains("private.txt"));
  EXPECT_FALSE(snap->at("private.txt").readable);
  EXPECT_TRUE(snap->at("private.txt").sha256.empty());
}

TEST_F(VerifyFixture, AnEmptyTreeSnapshotsCleanlyAndDiffsToNothing) {
  fs::remove_all(root_ / "data");
  fs::remove(root_ / "notes.txt");
  const auto snap = verifier_->snapshot();
  ASSERT_TRUE(snap.has_value());
  EXPECT_TRUE(snap->empty());
  EXPECT_TRUE(diff(*snap, *snap).empty());
}
