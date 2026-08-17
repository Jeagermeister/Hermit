#include <hermit/core/tools/edit.h>
#include <hermit/core/tools/move.h>
#include <hermit/core/tools/write.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>  // mkdtemp
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <sys/stat.h>

#include <hermit/core/backup.h>
#include <hermit/core/observed.h>
#include <hermit/core/sha256.h>
#include <hermit/core/tool.h>
#include <hermit/core/tools/find.h>
#include <hermit/core/tools/grep.h>
#include <hermit/core/tools/hash.h>
#include <hermit/core/tools/list.h>
#include <hermit/core/tools/read.h>

namespace fs = std::filesystem;
using hermit::BackupStore;
using hermit::EditTool;
using hermit::Field;
using hermit::ListTool;
using hermit::MoveTool;
using hermit::ObservedState;
using hermit::parse_args;
using hermit::RawArgs;
using hermit::ReadTool;
using hermit::Sandbox;
using hermit::sha256_hex;
using hermit::Tool;
using hermit::ToolOutput;
using hermit::ToolRow;
using hermit::WriteTool;

namespace {

// Layout built for every test:
//
//   <tmp>/root/            <- the sandbox root
//   <tmp>/root/seen.txt    "one two three\n"  (fixtures read it when needed)
//   <tmp>/root/sub/
//   <tmp>/backups/         <- the backup store, OUTSIDE the root
class MutateToolsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermit_mut_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));

    fs::create_directories(tmp_ / "root" / "sub");
    write_file(tmp_ / "root" / "seen.txt", "one two three\n");
    write_file(tmp_ / "root" / "sub" / "deep.txt", "delta one\n");

    auto sb = Sandbox::open(tmp_ / "root");
    ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
    box_ = std::make_unique<Sandbox>(std::move(*sb));
    store_ = std::make_unique<BackupStore>(tmp_ / "backups");
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  static void write_file(const fs::path& p, std::string_view contents) {
    std::ofstream out(p, std::ios::binary);
    out << contents;
  }

  static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
  }

  static const std::string* text(const ToolRow& row, std::string_view name) {
    for (const Field& f : row.fields) {
      if (f.name == name) return std::get_if<std::string>(&f.value);
    }
    return nullptr;
  }
  static const bool* boolean(const ToolRow& row, std::string_view name) {
    for (const Field& f : row.fields) {
      if (f.name == name) return std::get_if<bool>(&f.value);
    }
    return nullptr;
  }

  std::expected<ToolOutput, hermit::ToolError> call(Tool& tool, const RawArgs& raw) {
    auto parsed = parse_args(tool.spec(), raw, *box_);
    if (!parsed) return std::unexpected{hermit::ToolError{to_string(parsed.error())}};
    return tool.invoke(*parsed);
  }

  /// Observe seen.txt the way a session would: by reading it.
  void observe_seen() {
    ReadTool read{state_};
    auto out = call(read, {{"paths", std::vector<std::string>{"seen.txt"}}});
    ASSERT_TRUE(out.has_value()) << out.error().reason;
  }

  /// Every backup generation currently holding a file named `name`.
  std::vector<fs::path> backups_of(std::string_view name) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& entry :
         fs::recursive_directory_iterator(tmp_ / "backups", ec)) {
      if (entry.is_regular_file() && entry.path().filename() == name) {
        found.push_back(entry.path());
      }
    }
    std::sort(found.begin(), found.end());
    return found;
  }

  fs::path tmp_;
  std::unique_ptr<Sandbox> box_;
  std::unique_ptr<BackupStore> store_;
  ObservedState state_;
};

// --- write: create -----------------------------------------------------------

TEST_F(MutateToolsTest, WriteCreatesAnUnseenFileAndRecordsIt) {
  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"fresh.txt"}},
                          {"content", std::string{"hello\n"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 1u);
  EXPECT_EQ(*text(out->rows[0], "path"), "fresh.txt");
  EXPECT_TRUE(*boolean(out->rows[0], "created"));
  EXPECT_EQ(*text(out->rows[0], "hash"), sha256_hex("hello\n"));

  EXPECT_EQ(slurp(tmp_ / "root" / "fresh.txt"), "hello\n");
  EXPECT_EQ(state_.lookup("fresh.txt").status, ObservedState::Status::Present)
      << "a successful mutation records presence";
}

TEST_F(MutateToolsTest, WriteCreateRefusesAnExistingUnreadFile) {
  // seen.txt exists on disk but this session never read it: the create path
  // must refuse, and the original bytes must survive untouched.
  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"seen.txt"}},
                          {"content", std::string{"clobber"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("read it first"), std::string::npos);
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "one two three\n")
      << "the 05_copy shape, stopped";
}

TEST_F(MutateToolsTest, WriteCreatesMissingParentDirectories) {
  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"a/b/c/new.txt"}},
                          {"content", std::string{"deep"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "a" / "b" / "c" / "new.txt"), "deep");
}

TEST_F(MutateToolsTest, WriteCreateAfterAnObservedMissSucceeds) {
  // Read miss records absence; absent -> create-if-absent.
  ReadTool read{state_};
  auto miss = call(read, {{"paths", std::vector<std::string>{"gone.txt"}}});
  ASSERT_FALSE(miss.has_value());
  ASSERT_EQ(state_.lookup("gone.txt").status, ObservedState::Status::Absent);

  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"gone.txt"}},
                          {"content", std::string{"now here"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 1u);
  EXPECT_TRUE(*boolean(out->rows[0], "created"));
}

// --- write: replace ----------------------------------------------------------

TEST_F(MutateToolsTest, WriteReplacesAnObservedFileAndBacksUpTheOld) {
  observe_seen();
  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"seen.txt"}},
                          {"content", std::string{"replaced\n"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_FALSE(*boolean(out->rows[0], "created"));
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "replaced\n");

  auto kept = backups_of("seen.txt");
  ASSERT_EQ(kept.size(), 1u) << "R4: the old content was preserved first";
  EXPECT_EQ(slurp(kept[0]), "one two three\n");
}

TEST_F(MutateToolsTest, WriteRefusesWhenTheFileChangedSinceObservation) {
  observe_seen();
  write_file(tmp_ / "root" / "seen.txt", "changed behind the session's back");

  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"seen.txt"}},
                          {"content", std::string{"stomp"}}});
  ASSERT_FALSE(out.has_value()) << "the staleness guard is the point";
  EXPECT_NE(out.error().reason.find("changed since last observed"), std::string::npos);
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "changed behind the session's back");
  EXPECT_TRUE(backups_of("seen.txt").empty()) << "nothing mutated, nothing backed up";
}

TEST_F(MutateToolsTest, WriteAfterAListObservationSucceeds) {
  // section 4's one-currency property: a listing's tuple is directly usable.
  ListTool list{state_};
  auto listed = call(list, {{"path", std::string{"."}}});
  ASSERT_TRUE(listed.has_value());
  ASSERT_EQ(state_.lookup("seen.txt").status, ObservedState::Status::Present);

  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"seen.txt"}},
                          {"content", std::string{"via list\n"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "via list\n");
}

TEST_F(MutateToolsTest, WriteReplacePreservesTheFilesMode) {
  ASSERT_EQ(::chmod((tmp_ / "root" / "seen.txt").c_str(), 0640), 0);
  observe_seen();
  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"seen.txt"}},
                          {"content", std::string{"mode kept"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;

  struct ::stat st {};
  ASSERT_EQ(::stat((tmp_ / "root" / "seen.txt").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 07777, 0640u);
}

TEST_F(MutateToolsTest, WriteRepeatedReplacesKeepEveryBackupGeneration) {
  observe_seen();
  WriteTool write{state_, *store_};
  ASSERT_TRUE(call(write, {{"path", std::string{"seen.txt"}},
                           {"content", std::string{"v2"}}})
                  .has_value());
  ASSERT_TRUE(call(write, {{"path", std::string{"seen.txt"}},
                           {"content", std::string{"v3"}}})
                  .has_value());

  auto kept = backups_of("seen.txt");
  ASSERT_EQ(kept.size(), 2u);
  EXPECT_EQ(slurp(kept[0]), "one two three\n");
  EXPECT_EQ(slurp(kept[1]), "v2");
}

// --- edit --------------------------------------------------------------------

TEST_F(MutateToolsTest, EditRefusesAnUnseenFile) {
  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{"two"}},
                         {"new", std::string{"2"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("read it first"), std::string::npos);
}

TEST_F(MutateToolsTest, EditRefusesAnObservedAbsentFile) {
  ReadTool read{state_};
  ASSERT_FALSE(call(read, {{"paths", std::vector<std::string>{"gone.txt"}}}).has_value());

  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"gone.txt"}},
                         {"old", std::string{"x"}},
                         {"new", std::string{"y"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("not found"), std::string::npos);
}

TEST_F(MutateToolsTest, EditReplacesExactlyOneOccurrence) {
  observe_seen();
  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{"two"}},
                         {"new", std::string{"2"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "one 2 three\n");
  EXPECT_EQ(*text(out->rows[0], "hash"), sha256_hex("one 2 three\n"));

  auto kept = backups_of("seen.txt");
  ASSERT_EQ(kept.size(), 1u);
  EXPECT_EQ(slurp(kept[0]), "one two three\n") << "R4 holds for edit too";
}

TEST_F(MutateToolsTest, EditRefusesZeroOccurrences) {
  observe_seen();
  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{"absent-text"}},
                         {"new", std::string{"x"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("not found (0 occurrences)"), std::string::npos);
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "one two three\n");
}

TEST_F(MutateToolsTest, EditRefusesAmbiguousOccurrences) {
  write_file(tmp_ / "root" / "seen.txt", "twin twin\n");
  observe_seen();
  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{"twin"}},
                         {"new", std::string{"solo"}}});
  ASSERT_FALSE(out.has_value()) << "replace-first would guess; section 3 forbids guessing";
  EXPECT_NE(out.error().reason.find("ambiguous"), std::string::npos);
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "twin twin\n");
}

TEST_F(MutateToolsTest, EditRefusesWhenTheFileChangedSinceObservation) {
  observe_seen();
  write_file(tmp_ / "root" / "seen.txt", "one two three four\n");

  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{"two"}},
                         {"new", std::string{"2"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("changed since last observed"), std::string::npos);
}

TEST_F(MutateToolsTest, EditRefusesAnEmptyOldString) {
  observe_seen();
  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{}},
                         {"new", std::string{"x"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("empty"), std::string::npos);
}

TEST_F(MutateToolsTest, EditAllowsDeletionViaEmptyNew) {
  observe_seen();
  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{" two"}},
                         {"new", std::string{}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "one three\n");
}

// --- move --------------------------------------------------------------------

TEST_F(MutateToolsTest, MoveIsUngatedAndVerifiesAndRecordsBothEnds) {
  // Deliberately no read first: move is ungated (settled 2026-08-16).
  MoveTool move{state_};
  auto out = call(move, {{"from", std::string{"seen.txt"}},
                         {"to", std::string{"sub/renamed.txt"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 1u);
  EXPECT_EQ(*text(out->rows[0], "from"), "seen.txt");
  EXPECT_EQ(*text(out->rows[0], "to"), "sub/renamed.txt");
  EXPECT_EQ(*text(out->rows[0], "hash"), sha256_hex("one two three\n"))
      << "R3: the hash travelled with the file";

  EXPECT_FALSE(fs::exists(tmp_ / "root" / "seen.txt"));
  EXPECT_EQ(slurp(tmp_ / "root" / "sub" / "renamed.txt"), "one two three\n");
  EXPECT_EQ(state_.lookup("seen.txt").status, ObservedState::Status::Absent);
  EXPECT_EQ(state_.lookup("sub/renamed.txt").status, ObservedState::Status::Present);
}

TEST_F(MutateToolsTest, MoveRefusesAnExistingDestinationAndSuggestsAFreeName) {
  write_file(tmp_ / "root" / "target.txt", "already here");
  MoveTool move{state_};
  auto out = call(move, {{"from", std::string{"seen.txt"}},
                         {"to", std::string{"target.txt"}}});
  ASSERT_FALSE(out.has_value()) << "silent destruction, structurally impossible";
  EXPECT_NE(out.error().reason.find("destination exists"), std::string::npos);
  EXPECT_NE(out.error().reason.find("target (1).txt"), std::string::npos)
      << "the file-manager affordance, as a suggestion the caller acts on";

  EXPECT_EQ(slurp(tmp_ / "root" / "target.txt"), "already here");
  EXPECT_TRUE(fs::exists(tmp_ / "root" / "seen.txt")) << "nothing moved";
}

TEST_F(MutateToolsTest, MoveSuggestionSkipsNamesAlreadyTaken) {
  write_file(tmp_ / "root" / "target.txt", "v0");
  write_file(tmp_ / "root" / "target (1).txt", "v1");
  MoveTool move{state_};
  auto out = call(move, {{"from", std::string{"seen.txt"}},
                         {"to", std::string{"target.txt"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("target (2).txt"), std::string::npos);
}

TEST_F(MutateToolsTest, MoveToTheSuggestedNameSucceeds) {
  write_file(tmp_ / "root" / "target.txt", "already here");
  MoveTool move{state_};
  auto out = call(move, {{"from", std::string{"seen.txt"}},
                         {"to", std::string{"target (1).txt"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "target (1).txt"), "one two three\n");
}

TEST_F(MutateToolsTest, MoveRefusesADirectorySource) {
  MoveTool move{state_};
  auto out = call(move, {{"from", std::string{"sub"}},
                         {"to", std::string{"sub2"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("not a regular file"), std::string::npos);
}

TEST_F(MutateToolsTest, MoveRefusesAMissingSource) {
  MoveTool move{state_};
  auto out = call(move, {{"from", std::string{"gone.txt"}},
                         {"to", std::string{"anywhere.txt"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("gone.txt"), std::string::npos);
}

TEST_F(MutateToolsTest, MoveCreatesMissingDestinationParents) {
  MoveTool move{state_};
  auto out = call(move, {{"from", std::string{"seen.txt"}},
                         {"to", std::string{"new/depth/here.txt"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "new" / "depth" / "here.txt"), "one two three\n");
}

// --- R4 ordering, vanished files, and the observed-state edges ---------------

TEST_F(MutateToolsTest, WriteRefusesWhenBackupFailsAndLeavesTheFileUntouched) {
  // A FILE where the store's directory should be makes every preserve fail,
  // which turns R4's ordering -- backup strictly before mutation -- into an
  // observable fact: the target must be byte-intact after the refusal.
  fs::remove_all(tmp_ / "backups");
  write_file(tmp_ / "backups", "not a directory");

  observe_seen();
  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"seen.txt"}},
                          {"content", std::string{"stomp"}}});
  ASSERT_FALSE(out.has_value()) << "no backup, no mutation";
  EXPECT_NE(out.error().reason.find("backup failed"), std::string::npos);
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "one two three\n");
}

TEST_F(MutateToolsTest, EditRefusesWhenBackupFails) {
  fs::remove_all(tmp_ / "backups");
  write_file(tmp_ / "backups", "not a directory");

  observe_seen();
  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{"two"}},
                         {"new", std::string{"2"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("backup failed"), std::string::npos);
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "one two three\n");
}

TEST_F(MutateToolsTest, WriteRefusesWhenAnObservedFileVanishedThenCreateSucceeds) {
  observe_seen();
  fs::remove(tmp_ / "root" / "seen.txt");

  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"seen.txt"}},
                          {"content", std::string{"reborn"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("now missing"), std::string::npos);
  EXPECT_EQ(state_.lookup("seen.txt").status, ObservedState::Status::Absent)
      << "the miss records absence";

  // absent -> create-if-absent: the deliberate retry creates fresh, exactly
  // as the refusal message promises.
  auto again = call(write, {{"path", std::string{"seen.txt"}},
                            {"content", std::string{"reborn"}}});
  ASSERT_TRUE(again.has_value()) << again.error().reason;
  ASSERT_EQ(again->rows.size(), 1u);
  EXPECT_TRUE(*boolean(again->rows[0], "created"));
}

TEST_F(MutateToolsTest, EditRefusesWhenAnObservedFileVanished) {
  observe_seen();
  fs::remove(tmp_ / "root" / "seen.txt");

  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{"two"}},
                         {"new", std::string{"2"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("now missing"), std::string::npos);

  // The recorded absence makes a retry a clean "not found", not a loop.
  auto again = call(edit, {{"path", std::string{"seen.txt"}},
                           {"old", std::string{"two"}},
                           {"new", std::string{"2"}}});
  ASSERT_FALSE(again.has_value());
  EXPECT_NE(again.error().reason.find("not found"), std::string::npos);
}

TEST_F(MutateToolsTest, TwoSuccessiveEditsWithoutAReread) {
  // The first edit's read-back observation must satisfy the second edit's
  // staleness gate -- forgetting record_present would refuse every second
  // edit as stale.
  observe_seen();
  EditTool edit{state_, *store_};
  ASSERT_TRUE(call(edit, {{"path", std::string{"seen.txt"}},
                          {"old", std::string{"one"}},
                          {"new", std::string{"1"}}})
                  .has_value());
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{"three"}},
                         {"new", std::string{"3"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "1 two 3\n");
  EXPECT_EQ(backups_of("seen.txt").size(), 2u);
}

TEST_F(MutateToolsTest, EditAfterAListObservationSucceeds) {
  // The one-currency property at depth: a listing of sub/ must record
  // sub/deep.txt under the same key an edit's resolved path produces.
  ListTool list{state_};
  ASSERT_TRUE(call(list, {{"path", std::string{"sub"}}}).has_value());
  ASSERT_EQ(state_.lookup("sub/deep.txt").status, ObservedState::Status::Present);

  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"sub/deep.txt"}},
                         {"old", std::string{"delta"}},
                         {"new", std::string{"DELTA"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "sub" / "deep.txt"), "DELTA one\n");
}

TEST_F(MutateToolsTest, WriteRefusesAfterAMetadataOnlyChange) {
  // chmod moves ctime and nothing else -- the exact reason ctime is in the
  // tuple (D10: mtime is forgeable, ctime is not settable directly). A
  // comparison "simplified" to dev:ino:size:mtime passes here; the real one
  // must refuse.
  observe_seen();
  ASSERT_EQ(::chmod((tmp_ / "root" / "seen.txt").c_str(), 0620), 0);

  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"seen.txt"}},
                          {"content", std::string{"stomp"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("changed since last observed"), std::string::npos);
}

TEST_F(MutateToolsTest, WriteReplaceDropsSetuidAndFriends) {
  ASSERT_EQ(::chmod((tmp_ / "root" / "seen.txt").c_str(), 04755), 0);
  observe_seen();
  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"seen.txt"}},
                          {"content", std::string{"no privilege rides along"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;

  struct ::stat st {};
  ASSERT_EQ(::stat((tmp_ / "root" / "seen.txt").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 07777, 0755u) << "permission bits kept, setuid dropped";
}

TEST_F(MutateToolsTest, EditTreatsOverlappingOccurrencesAsAmbiguous) {
  write_file(tmp_ / "root" / "tri.txt", "aaa\n");
  ReadTool read{state_};
  ASSERT_TRUE(call(read, {{"paths", std::vector<std::string>{"tri.txt"}}}).has_value());

  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"tri.txt"}},
                         {"old", std::string{"aa"}},
                         {"new", std::string{"b"}}});
  ASSERT_FALSE(out.has_value()) << "overlap is genuine ambiguity";
  EXPECT_NE(out.error().reason.find("ambiguous"), std::string::npos);
  EXPECT_EQ(slurp(tmp_ / "root" / "tri.txt"), "aaa\n");
}

TEST_F(MutateToolsTest, EditRefusesAFileOverItsCap) {
  observe_seen();
  EditTool edit{state_, *store_, 8};
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{"two"}},
                         {"new", std::string{"2"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("read cap"), std::string::npos);
}

TEST_F(MutateToolsTest, EditAtExactlyTheCapSucceeds) {
  observe_seen();
  EditTool edit{state_, *store_, 14};  // "one two three\n" is 14 bytes
  auto out = call(edit, {{"path", std::string{"seen.txt"}},
                         {"old", std::string{"two"}},
                         {"new", std::string{"2"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
}

TEST_F(MutateToolsTest, NoTempOrphansRemainAfterWrites) {
  WriteTool write{state_, *store_};
  ASSERT_TRUE(call(write, {{"path", std::string{"fresh.txt"}},
                           {"content", std::string{"a"}}})
                  .has_value());
  ASSERT_FALSE(call(write, {{"path", std::string{"seen.txt"}},
                            {"content", std::string{"refused"}}})
                   .has_value());
  observe_seen();
  ASSERT_TRUE(call(write, {{"path", std::string{"seen.txt"}},
                           {"content", std::string{"replaced"}}})
                  .has_value());

  for (const auto& entry : fs::recursive_directory_iterator(tmp_ / "root")) {
    EXPECT_EQ(entry.path().filename().string().find("hermit-tmp"),
              std::string::npos)
        << "orphaned temp file: " << entry.path();
  }
}

TEST_F(MutateToolsTest, ListRefusesWhenAnEntryCannotBeStatted) {
  fs::create_directories(tmp_ / "root" / "locked");
  write_file(tmp_ / "root" / "locked" / "inner.txt", "sealed");
  ASSERT_EQ(::chmod((tmp_ / "root" / "locked").c_str(), 0400), 0);

  ListTool list{state_};
  auto out = call(list, {{"path", std::string{"locked"}}});
  ::chmod((tmp_ / "root" / "locked").c_str(), 0700);  // let TearDown clean up

  ASSERT_FALSE(out.has_value()) << "a silent skip is the adjacent success";
  EXPECT_NE(out.error().reason.find("inner.txt"), std::string::npos);
  EXPECT_EQ(state_.lookup("locked/inner.txt").status, ObservedState::Status::Unseen)
      << "a refused listing records nothing";
}

TEST_F(MutateToolsTest, ReadPartialFailureRecordsNoPresence) {
  ReadTool read{state_};
  auto out = call(read, {{"paths", std::vector<std::string>{"seen.txt", "nope.txt"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(state_.lookup("seen.txt").status, ObservedState::Status::Unseen)
      << "no bytes were delivered, so no presence may be recorded";
  EXPECT_EQ(state_.lookup("nope.txt").status, ObservedState::Status::Absent)
      << "absence is the safe direction and records immediately";
}

// --- the backup store --------------------------------------------------------

TEST_F(MutateToolsTest, BackupStoreSurvivesReuseOfItsDirectory) {
  BackupStore first{tmp_ / "backups"};
  auto a = first.preserve("seen.txt", "generation zero");
  ASSERT_TRUE(a.has_value()) << to_string(a.error());

  // A second session over the same configured directory must continue the
  // numbering, not collide with (or overwrite) the first session's archive.
  BackupStore second{tmp_ / "backups"};
  auto b = second.preserve("seen.txt", "generation one");
  ASSERT_TRUE(b.has_value()) << to_string(b.error());

  EXPECT_NE(*a, *b);
  EXPECT_EQ(slurp(*a), "generation zero");
  EXPECT_EQ(slurp(*b), "generation one");
}

TEST_F(MutateToolsTest, BackupPathsMirrorTheSandboxLayout) {
  ListTool list{state_};
  ASSERT_TRUE(call(list, {{"path", std::string{"sub"}}}).has_value());
  WriteTool write{state_, *store_};
  ASSERT_TRUE(call(write, {{"path", std::string{"sub/deep.txt"}},
                           {"content", std::string{"new"}}})
                  .has_value());

  const fs::path expected = tmp_ / "backups" / "0000" / "sub" / "deep.txt";
  ASSERT_TRUE(fs::exists(expected))
      << "the archive mirrors the sandbox tree, generation by generation";
  EXPECT_EQ(slurp(expected), "delta one\n");
}

TEST_F(MutateToolsTest, BackupStoreRefusesEscapingPaths) {
  auto absolute = store_->preserve("/etc/hostname", "x");
  ASSERT_FALSE(absolute.has_value());
  EXPECT_NE(to_string(absolute.error()).find("sandbox-relative"), std::string::npos);

  auto dotted = store_->preserve("a/../../escape.txt", "x");
  ASSERT_FALSE(dotted.has_value());
  EXPECT_NE(to_string(dotted.error()).find(".."), std::string::npos);
}

TEST_F(MutateToolsTest, WriteCreateHonorsTheUmask) {
  const ::mode_t previous = ::umask(027);
  WriteTool write{state_, *store_};
  auto out = call(write, {{"path", std::string{"masked.txt"}},
                          {"content", std::string{"x"}}});
  ::umask(previous);
  ASSERT_TRUE(out.has_value()) << out.error().reason;

  struct ::stat st {};
  ASSERT_EQ(::stat((tmp_ / "root" / "masked.txt").c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0640u) << "0666 & ~027";
}

// --- move edges --------------------------------------------------------------

TEST_F(MutateToolsTest, MoveThenEditTheDestinationWithoutARead) {
  // The tuple move records must be the destination's real post-rename tuple,
  // or this edit spuriously refuses as stale.
  MoveTool move{state_};
  ASSERT_TRUE(call(move, {{"from", std::string{"seen.txt"}},
                          {"to", std::string{"sub/renamed.txt"}}})
                  .has_value());

  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"sub/renamed.txt"}},
                         {"old", std::string{"two"}},
                         {"new", std::string{"2"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "sub" / "renamed.txt"), "one 2 three\n");
}

TEST_F(MutateToolsTest, MoveOntoItselfRefusesAndLeavesTheFileIntact) {
  MoveTool move{state_};
  auto out = call(move, {{"from", std::string{"seen.txt"}},
                         {"to", std::string{"seen.txt"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("same file"), std::string::npos);
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "one two three\n");
}

TEST_F(MutateToolsTest, MoveOntoADirectoryNamesTheFileInsideIt) {
  MoveTool move{state_};
  auto out = call(move, {{"from", std::string{"seen.txt"}},
                         {"to", std::string{"sub"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("directory"), std::string::npos);
  EXPECT_NE(out.error().reason.find("sub/seen.txt"), std::string::npos)
      << "the guidance names the path the model almost certainly meant";
  EXPECT_TRUE(fs::exists(tmp_ / "root" / "seen.txt"));
}

// --- byte-exactness edges ----------------------------------------------------

TEST_F(MutateToolsTest, EmptyContentWriteCreatesAndReplaces) {
  WriteTool write{state_, *store_};
  auto created = call(write, {{"path", std::string{"empty.txt"}},
                              {"content", std::string{}}});
  ASSERT_TRUE(created.has_value()) << created.error().reason;
  EXPECT_EQ(fs::file_size(tmp_ / "root" / "empty.txt"), 0u);

  auto replaced = call(write, {{"path", std::string{"empty.txt"}},
                               {"content", std::string{}}});
  ASSERT_TRUE(replaced.has_value()) << replaced.error().reason;
  ASSERT_EQ(replaced->rows.size(), 1u);
  EXPECT_FALSE(*boolean(replaced->rows[0], "created"));
}

TEST_F(MutateToolsTest, WriteAndEditCarryNulBytes) {
  const std::string binary{"a\0b", 3};
  WriteTool write{state_, *store_};
  ASSERT_TRUE(call(write, {{"path", std::string{"bin.dat"}},
                           {"content", binary}})
                  .has_value());
  EXPECT_EQ(slurp(tmp_ / "root" / "bin.dat"), binary);

  EditTool edit{state_, *store_};
  auto out = call(edit, {{"path", std::string{"bin.dat"}},
                         {"old", std::string{"\0", 1}},
                         {"new", std::string{"X"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "bin.dat"), "aXb");
}

// --- the full surface --------------------------------------------------------

TEST_F(MutateToolsTest, AllEightToolsComposeIntoOneRegistry) {
  hermit::ToolRegistry registry;
  ASSERT_TRUE(registry.add(std::make_unique<ReadTool>(state_)).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<ListTool>(state_)).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<hermit::FindTool>()).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<hermit::GrepTool>()).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<hermit::HashTool>()).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<WriteTool>(state_, *store_)).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<EditTool>(state_, *store_)).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<MoveTool>(state_)).has_value());
  EXPECT_EQ(registry.tools().size(), 8u) << "the whole settled surface, one registry";

  // read -> edit through the registry, the observed-state loop end to end.
  Tool* read = registry.find("read");
  auto parsed = parse_args(read->spec(),
                           {{"paths", std::vector<std::string>{"seen.txt"}}}, *box_);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(read->invoke(*parsed).has_value());

  Tool* edit = registry.find("edit");
  auto edit_args = parse_args(edit->spec(),
                              {{"path", std::string{"seen.txt"}},
                               {"old", std::string{"three"}},
                               {"new", std::string{"3"}}},
                              *box_);
  ASSERT_TRUE(edit_args.has_value());
  auto out = edit->invoke(*edit_args);
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(slurp(tmp_ / "root" / "seen.txt"), "one two 3\n");
}

}  // namespace
