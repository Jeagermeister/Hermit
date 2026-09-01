// Per-turn state verification -- ROUTING.md section 6's half that holds unconditionally.
//
// Split like the code: `diff` is pure over two snapshots, so the whole classification
// table is exercised without a filesystem; the walk needs a real tree and gets one.

#include <hermit/supervisor/verify.h>

#include <gtest/gtest.h>

#include <algorithm>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using hermit::IdentityTuple;
using hermit::Sandbox;
using hermit::supervisor::Change;
using hermit::supervisor::ChangeKind;
using hermit::supervisor::Changeset;
using hermit::supervisor::diff;
using hermit::supervisor::FileState;
using hermit::supervisor::TreeSnapshot;
using hermit::supervisor::TreeVerifier;

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
    std::string tpl = (fs::temp_directory_path() / "hermit_verify_XXXXXX").string();
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

  /// D13's amendment, reproduced: overwrite `rel` in place through a `MAP_SHARED` mapping
  /// rather than through `write()`/`open(O_TRUNC)`. `replacement` must be the same length
  /// as the file's current content -- this is what keeps `st_size` (and with it the whole
  /// identity tuple) from moving, which is the entire point of the repro. The kernel stamps
  /// mtime/ctime on the page *fault*, not on a later store to an already-dirty page, so this
  /// changes the file's bytes with no timestamp movement at all.
  void overwrite_via_mmap_shared(const std::string& rel, std::string_view replacement) {
    const fs::path path = root_ / rel;
    const int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0) << "open: " << std::strerror(errno);
    struct ::stat st {};
    ASSERT_EQ(::fstat(fd, &st), 0);
    ASSERT_EQ(static_cast<std::uint64_t>(st.st_size), replacement.size())
        << "the repro requires an in-place, same-length overwrite -- a size change would "
           "move the identity tuple through st_size, which is not the gap being tested";
    void* mapping = ::mmap(nullptr, replacement.size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(mapping, MAP_FAILED) << "mmap: " << std::strerror(errno);
    std::memcpy(mapping, replacement.data(), replacement.size());
    ASSERT_EQ(::msync(mapping, replacement.size(), MS_SYNC), 0);
    ::munmap(mapping, replacement.size());
    ::close(fd);
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

  // The baseline is pinned so the arithmetic below is checkable by eye: 11 bytes of
  // notes.txt plus 6 of data/counts.txt.
  EXPECT_EQ(baseline_bytes, 17u);
  // There used to be an EXPECT_LT(last_hashed_bytes, baseline_bytes) here with the message
  // "the unchanged file was re-hashed". It was decoration: a *full* re-hash of the second
  // walk costs 8 + 6 = 14, which is already less than 17, so the assertion passed under
  // precisely the regression its own message named (found by mutation testing 2026-08-17).
  // An inequality cannot express this whenever the changed file shrank; only the exact
  // count can, so the exact count is the whole test.
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

TEST_F(VerifyFixture, TheStreamingHashCoversEveryByteNotJustTheFirstBuffer) {
  // hash_regular reads in 64 KiB chunks. Every other file in this fixture is a few dozen
  // bytes, so that loop never iterated twice anywhere in the suite -- a mutation
  // truncating it to a single chunk survived all 468 tests (mutation testing 2026-08-17).
  // R3's promise is content, so a file larger than the buffer, differing only past it, is
  // the only thing that pins it.
  std::string big(200u * 1024, 'a');
  write("big.bin", big);
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value()) << before.error().message();

  big.back() = 'b';  // one byte at offset 204799 -- three buffers past the first read
  write("big.bin", big);
  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value()) << after.error().message();

  const Changeset set = diff(*before, *after);
  ASSERT_EQ(set.changes.size(), 1u) << set.render();
  EXPECT_EQ(set.changes[0].path, "big.bin");
  EXPECT_EQ(set.changes[0].kind, ChangeKind::Modified)
      << "a byte past the first 64 KiB buffer still has to move the hash";
  EXPECT_EQ(set.substantive(), 1u);
}

TEST_F(VerifyFixture, ALargeUnchangedFileIsNotReReadWhenASmallOneMoves) {
  // The incremental claim stated so that a regression to full re-hashing cannot hide in
  // the arithmetic: the tree is now dominated by one 200 KiB file that does not change,
  // so re-reading it would be off by four orders of magnitude rather than by three bytes.
  write("big.bin", std::string(200u * 1024, 'a'));
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value()) << before.error().message();
  EXPECT_EQ(verifier_->last_hashed_bytes(), 200u * 1024 + 17u);

  write("notes.txt", "CHANGED\n");
  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value()) << after.error().message();

  EXPECT_EQ(verifier_->last_hashed_bytes(), 8u)
      << "the 200 KiB file was re-read; carrying hashes forward is what makes a walk cheap";
  EXPECT_EQ(verifier_->last_entries_walked(), before->size()) << "every entry is still stat'd";
}

// --- D13's amendment: MAP_SHARED writes and force_rehash ---------------------

TEST_F(VerifyFixture, DefaultReuseMissesAMapSharedWrite) {
  // Documents the gap as an executable fact, not just prose: this is what a caller gets
  // if `shell` is registered without also turning force_rehash on. notes.txt is 11 bytes
  // ("alpha\nbeta\n"); the replacement keeps that length so the identity tuple genuinely
  // does not move.
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value()) << before.error().message();
  const std::string original_hash = before->at("notes.txt").sha256;

  overwrite_via_mmap_shared("notes.txt", "PWNED!!!!!!");

  struct ::stat st_after {};
  ASSERT_EQ(::stat((root_ / "notes.txt").c_str(), &st_after), 0);
  // The repro's own precondition, checked rather than assumed. D13's amendment measured
  // mtime/ctime staying byte-identical across a MAP_SHARED write on the kernel it was
  // written against; this session's own kernel (7.2.0-1-cachyos) was independently
  // checked by hand -- on both tmpfs and this repo's own btrfs -- and does NOT reproduce
  // that: mtime moves on this kernel even with no msync() at all. That is new, dated
  // information (2026-08-26), not a contradiction of what D13 measured on an earlier one --
  // recorded in DECISIONS.md rather than silently assumed to still hold everywhere. A skip
  // here is the honest response to "could not reproduce today", per this codebase's own
  // D11 vocabulary -- not a claim that the gap is closed, which is exactly why force_rehash
  // stays unconditional rather than gated on a live check of this behavior.
  if (static_cast<std::int64_t>(st_after.st_mtim.tv_sec) * 1000000000 + st_after.st_mtim.tv_nsec !=
      before->at("notes.txt").tuple.mtime_ns) {
    GTEST_SKIP() << "this kernel/filesystem moves mtime on a MAP_SHARED write, so the "
                    "reuse-blind-spot this test demonstrates does not reproduce here today "
                    "(see DECISIONS.md D13's amendment, and the note dated 2026-08-26 beside it)";
  }

  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value()) << after.error().message();
  EXPECT_EQ(after->at("notes.txt").sha256, original_hash)
      << "the reused (stale) hash, not the true content -- this is the gap D13 named, "
         "reachable once shell lands and force_rehash is not on";
  EXPECT_TRUE(diff(*before, *after).empty())
      << "the report says nothing changed, even though it did";
}

TEST_F(VerifyFixture, ForceRehashCatchesAMapSharedWriteTheDefaultMisses) {
  TreeVerifier rehashing{*sandbox_, /*force_rehash=*/true};

  const auto before = rehashing.snapshot();
  ASSERT_TRUE(before.has_value()) << before.error().message();
  const std::string original_hash = before->at("notes.txt").sha256;

  overwrite_via_mmap_shared("notes.txt", "PWNED!!!!!!");

  const auto after = rehashing.snapshot(&*before);
  ASSERT_TRUE(after.has_value()) << after.error().message();
  EXPECT_NE(after->at("notes.txt").sha256, original_hash)
      << "force_rehash must re-read every regular file regardless of its identity tuple";

  const Changeset set = diff(*before, *after);
  const Change* change = find_change(set, "notes.txt");
  ASSERT_NE(change, nullptr) << "the write must be visible in the report";
  EXPECT_EQ(change->kind, ChangeKind::Modified);
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
  EXPECT_EQ(snap.error().kind, hermit::supervisor::VerifyErrorKind::DirectoryUnreadable);
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

// --- Permission bits: a chmod is an act, not an artefact ---------------------

TEST_F(VerifyFixture, ABaselineRecordsThePermissionBits) {
  const auto snap = verifier_->snapshot();
  ASSERT_TRUE(snap.has_value()) << snap.error().message();
  // Only the permission bits: the file-type bits live in is_dir/is_symlink, and a mode
  // carrying S_IFREG here would make every comparison below meaningless.
  EXPECT_EQ(snap->at("notes.txt").mode & ~0777u, 0u);
  EXPECT_TRUE(snap->at("data").is_dir);
  EXPECT_NE(snap->at("data").mode, 0u) << "a directory has permissions too";
}

TEST_F(VerifyFixture, MakingAFileExecutableIsItsOwnKindAndIsSubstantive) {
  // The gap this closes: chmod moves ctime and no content, so before the mode was recorded
  // this was reported as `touched` -- the kind verify.h tells readers to skim -- and
  // substantive() did not count it. `08_write_and_run_script` asks a model to make a script
  // executable, so this is ordinary behaviour rather than an exotic case.
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value()) << before.error().message();

  ASSERT_EQ(::chmod((root_ / "notes.txt").c_str(), 0755), 0);
  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value()) << after.error().message();

  const Changeset set = diff(*before, *after);
  ASSERT_EQ(set.changes.size(), 1u) << set.render();
  EXPECT_EQ(set.changes[0].path, "notes.txt");
  EXPECT_EQ(set.changes[0].kind, ChangeKind::PermissionsChanged);
  EXPECT_NE(set.changes[0].kind, ChangeKind::TouchedOnly) << "this is not noise";
  EXPECT_EQ(set.changes[0].mode_after & 0111u, 0111u) << "the executable bit is what moved";
  EXPECT_EQ(set.substantive(), 1u);
  EXPECT_EQ(set.render(), "permissions  notes.txt  0644 -> 0755\n");
}

TEST_F(VerifyFixture, AChmodIsNotHiddenBehindARewrite) {
  // When content moves as well, the larger fact wins the kind -- but the permission delta
  // still rides along, because a model that rewrote a file *and* made it executable has
  // done the second thing whether or not the first is more interesting.
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value());

  write("notes.txt", "CHANGED\n");
  ASSERT_EQ(::chmod((root_ / "notes.txt").c_str(), 0755), 0);
  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value());

  const Changeset set = diff(*before, *after);
  ASSERT_EQ(set.changes.size(), 1u) << set.render();
  EXPECT_EQ(set.changes[0].kind, ChangeKind::Modified) << "the rewrite is the bigger fact";
  EXPECT_EQ(set.changes[0].mode_before & 0111u, 0u);
  EXPECT_EQ(set.changes[0].mode_after & 0111u, 0111u) << "and the chmod still shows";
  EXPECT_NE(set.render().find("0644 -> 0755"), std::string::npos) << set.render();
}

TEST_F(VerifyFixture, APermissionChangeThatChangesNothingElseIsNotAModification) {
  // The mirror of the above: R3's rule is content, so a chmod must never claim bytes moved.
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value());
  ASSERT_EQ(::chmod((root_ / "notes.txt").c_str(), 0600), 0);
  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value());

  const Changeset set = diff(*before, *after);
  ASSERT_EQ(set.changes.size(), 1u) << set.render();
  EXPECT_NE(set.changes[0].kind, ChangeKind::Modified);
  EXPECT_TRUE(set.changes[0].before.empty()) << "no hash is claimed on either side";
  EXPECT_TRUE(set.changes[0].after.empty());
  EXPECT_EQ(before->at("notes.txt").sha256, after->at("notes.txt").sha256);
}

TEST_F(VerifyFixture, TheSetuidAndStickyBitsAreVisibleNotTruncated) {
  // Four octal digits, so a file that gains setuid does not render identically to one that
  // gained nothing. Recorded as its own test because a three-digit format would pass every
  // other assertion in this file.
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value());
  ASSERT_EQ(::chmod((root_ / "notes.txt").c_str(), 04644), 0);
  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value());

  const Changeset set = diff(*before, *after);
  ASSERT_EQ(set.changes.size(), 1u) << set.render();
  EXPECT_EQ(set.changes[0].kind, ChangeKind::PermissionsChanged);
  EXPECT_EQ(set.render(), "permissions  notes.txt  0644 -> 4644\n");
}

TEST_F(VerifyFixture, AnUnchangedTreeStillReportsNoPermissionChange) {
  const auto before = verifier_->snapshot();
  ASSERT_TRUE(before.has_value());
  const auto after = verifier_->snapshot(&*before);
  ASSERT_TRUE(after.has_value());
  EXPECT_TRUE(diff(*before, *after).empty()) << "recording mode must not invent changes";
}

TEST(ChangesetRender, ACraftedFilenameCannotForgeAReportLine) {
  // `render()` feeds the "what actually changed (hash-verified, R6)" block, which is the
  // artifact this whole supervisor exists to make trustworthy. A filename carrying a
  // newline would add a line to it that a reader cannot tell from a real one -- and it is
  // the *more* trusted of the two fields on that line, since nothing in it comes from the
  // model's reply. Sandbox::resolve closes this for model-supplied paths; a name already
  // on disk never passed that gate.
  hermit::supervisor::Changeset set;
  set.changes.push_back(hermit::supervisor::Change{
      .path = "a.txt\n        ~ Deleted  important.db", .kind = hermit::supervisor::ChangeKind::Created});

  const std::string text = set.render();
  EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 1)
      << "one change rendered as more than one line: " << text;
  EXPECT_NE(text.find("important.db"), std::string::npos) << "the name itself must still show";
}
