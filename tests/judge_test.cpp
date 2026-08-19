#include <hermit/supervisor/judge.h>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

using hermit::supervisor::Expectation;
using hermit::supervisor::ExpectationSet;
using hermit::supervisor::FileState;
using hermit::supervisor::judge;
using hermit::supervisor::Outcome;
using hermit::supervisor::TreeSnapshot;
using hermit::supervisor::Verdict;

/// A plausible-looking distinct digest. The judge only ever compares hashes for equality
/// and emptiness, so the bytes do not matter -- the width does, because a 64-character hex
/// string is what a real snapshot holds and a test that passed only for short strings
/// would be testing something else.
std::string H(char c) { return std::string(64, c); }

FileState regular(const std::string& sha) {
  return FileState{.tuple = {}, .sha256 = sha, .is_dir = false, .is_symlink = false, .readable = true};
}
FileState directory() {
  return FileState{.tuple = {}, .sha256 = {}, .is_dir = true, .is_symlink = false, .readable = true};
}
FileState symlink() {
  return FileState{.tuple = {}, .sha256 = {}, .is_dir = false, .is_symlink = true, .readable = true};
}
FileState unreadable() {
  return FileState{.tuple = {}, .sha256 = {}, .is_dir = false, .is_symlink = false, .readable = false};
}
/// A FIFO, socket or device: not a directory, not a symlink, readable as far as the walk
/// is concerned, and never opened -- so no hash. The shape `FileState` cannot name,
/// because it carries no `is_regular`.
FileState not_regular() {
  return FileState{.tuple = {}, .sha256 = {}, .is_dir = false, .is_symlink = false, .readable = true};
}

Outcome outcome_of(const Verdict& v, std::size_t i) { return v.findings.at(i).outcome; }
const std::string& reason_of(const Verdict& v, std::size_t i) { return v.findings.at(i).reason; }

}  // namespace

// --- Exists ------------------------------------------------------------------

TEST(JudgeExists, IsMetWhenThePathIsPresent) {
  const TreeSnapshot after{{"final.md", regular(H('a'))}};
  const auto v = judge({}, after, {Expectation::exists("final.md")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
  EXPECT_TRUE(v.met());
}

TEST(JudgeExists, IsUnmetWhenThePathIsAbsent) {
  const auto v = judge({}, TreeSnapshot{}, {Expectation::exists("final.md")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(v, 0), "final.md does not exist");
}

TEST(JudgeExists, AsADirectoryRefusesARegularFile) {
  const TreeSnapshot after{{"project/tests", regular(H('a'))}};
  const auto v = judge({}, after, {Expectation::exists("project/tests", true)});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(v, 0), "project/tests is a regular file, not a directory");
}

TEST(JudgeExists, AsADirectoryRefusesASymlinkPointingAtOne) {
  // The snapshot records what lstat saw and this layer never follows a link, so a symlink
  // has is_dir false however it resolves. Refusing it is the intent, not a gap: a caller
  // asking for a directory should not be satisfied by a name that redirects somewhere.
  const TreeSnapshot after{{"release", symlink()}};
  const auto v = judge({}, after, {Expectation::exists("release", true)});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
}

TEST(JudgeExists, WithoutTheDirectoryFlagADirectorySatisfiesIt) {
  const TreeSnapshot after{{"release", directory()}};
  const auto v = judge({}, after, {Expectation::exists("release")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
}

// --- Absent ------------------------------------------------------------------

TEST(JudgeAbsent, IsMetWhenThePathIsGone) {
  const TreeSnapshot before{{"draft_v1.md", regular(H('a'))}};
  const auto v = judge(before, TreeSnapshot{}, {Expectation::absent("draft_v1.md")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
}

TEST(JudgeAbsent, IsUnmetWhenThePathIsStillThere) {
  const TreeSnapshot before{{"draft_v1.md", regular(H('a'))}};
  const auto v = judge(before, before, {Expectation::absent("draft_v1.md")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(v, 0), "draft_v1.md still exists");
}

TEST(JudgeAbsent, ADirectoryStillCountsAsPresent) {
  const TreeSnapshot after{{"scratch", directory()}};
  const auto v = judge({}, after, {Expectation::absent("scratch")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
}

// --- Preserved ---------------------------------------------------------------

TEST(JudgePreserved, IsMetWhenTheBytesSurvivedTheMove) {
  const TreeSnapshot before{{"report.txt", regular(H('a'))}};
  const TreeSnapshot after{{"archive/report.txt", regular(H('a'))}};
  const auto v = judge(before, after, {Expectation::preserved("report.txt", "archive/report.txt")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
}

TEST(JudgePreserved, IsUnmetWhenTheDestinationDoesNotExist) {
  const TreeSnapshot before{{"report.txt", regular(H('a'))}};
  const auto v = judge(before, TreeSnapshot{},
                       {Expectation::preserved("report.txt", "archive/report.txt")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(v, 0),
            "archive/report.txt does not exist; it should hold the bytes report.txt had");
}

TEST(JudgePreserved, IsUnmetWhenTheBytesDiffer) {
  const TreeSnapshot before{{"draft_v1.md", regular(H('a'))}};
  const TreeSnapshot after{{"final.md", regular(H('b'))}};
  const auto v = judge(before, after, {Expectation::preserved("draft_v1.md", "final.md")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(v, 0), "final.md does not have the bytes draft_v1.md had");
}

TEST(JudgePreserved, IsUndecidableWhenTheSourceWasNeverInTheBaseline) {
  // Nothing the model does can put bytes into the baseline, so this is the author's error
  // and must not reach the model as a retry.
  const TreeSnapshot after{{"final.md", regular(H('a'))}};
  const auto v = judge({}, after, {Expectation::preserved("never_existed.md", "final.md")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Undecidable);
  EXPECT_FALSE(v.first_unmet().has_value()) << "an undecidable finding must never be restated";
}

TEST(JudgePreserved, IsUndecidableWhenEitherSideCannotBeRead) {
  const TreeSnapshot before{{"a.txt", regular(H('a'))}};
  const TreeSnapshot after{{"b.txt", unreadable()}};
  const auto v = judge(before, after, {Expectation::preserved("a.txt", "b.txt")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Undecidable);
  EXPECT_EQ(reason_of(v, 0), "b.txt could not be read");

  const TreeSnapshot blind{{"a.txt", unreadable()}};
  const auto w = judge(blind, TreeSnapshot{{"b.txt", regular(H('a'))}},
                       {Expectation::preserved("a.txt", "b.txt")});
  EXPECT_EQ(outcome_of(w, 0), Outcome::Undecidable);
  EXPECT_EQ(reason_of(w, 0),
            "a.txt could not be read in the baseline, so its bytes cannot be compared")
      << "the old wording claimed it 'never had bytes to preserve', which is false: it had "
         "bytes and we were refused them";
}

TEST(JudgePreserved, TheWrongKindOfObjectIsADecidedNegativeNotAnUndecidableOne) {
  // The distinction review 2026-08-17 found collapsed. A directory, symlink or device
  // where a file's bytes were expected is a fact the judge *established* -- it does not
  // hold those bytes -- so it is Unmet and R7 can restate it. Reporting it Undecidable
  // made met() false while first_unmet() returned nothing, stalling the run on exactly the
  // case Preserved exists to catch: `config.ini` replaced by a symlink.
  struct Case {
    FileState state;
    std::string expected_reason;
  };
  const Case cases[] = {
      {directory(), "b is a directory; it should hold the bytes a had"},
      {symlink(), "b is a symlink; it should hold the bytes a had"},
      {not_regular(), "b is a FIFO, socket or device; it should hold the bytes a had"},
  };
  for (const auto& c : cases) {
    const TreeSnapshot before{{"a", regular(H('a'))}};
    const TreeSnapshot after{{"b", c.state}};
    const auto v = judge(before, after, {Expectation::preserved("a", "b")});
    EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet) << c.expected_reason;
    EXPECT_EQ(reason_of(v, 0), c.expected_reason);
    ASSERT_TRUE(v.first_unmet().has_value()) << "and R7 has something to restate";
  }
}

TEST(JudgePreserved, ReplacingAGuardedFileWithASymlinkIsCaughtAndRestatable) {
  // The self form of the above, which is the destructive case in its natural shape.
  const TreeSnapshot before{{"config.ini", regular(H('a'))}};
  const TreeSnapshot after{{"config.ini", symlink()}};
  const auto v = judge(before, after, {Expectation::preserved("config.ini", "config.ini")});
  EXPECT_FALSE(v.met());
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  ASSERT_TRUE(v.first_unmet().has_value())
      << "a verdict that is not met must always give R7 a sentence or an operator note";
}

TEST(JudgePreserved, TwoUnhashableSidesAreNotSilentlyEqual) {
  // The regression the guard is about: both sides have an empty sha256, and a bare string
  // comparison would call them identical. Here the *baseline* side is the unhashable one,
  // so this is the author-error branch rather than the wrong-object branch.
  const TreeSnapshot before{{"a", directory()}};
  const TreeSnapshot after{{"b", directory()}};
  const auto v = judge(before, after, {Expectation::preserved("a", "b")});
  EXPECT_NE(outcome_of(v, 0), Outcome::Met);
  EXPECT_EQ(outcome_of(v, 0), Outcome::Undecidable);
  EXPECT_EQ(reason_of(v, 0),
            "a was a directory in the baseline, so it never had bytes to preserve");
}

TEST(JudgePreserved, TheSelfFormIsTheDoNotTouchConstraint) {
  const TreeSnapshot before{{"config.ini", regular(H('a'))}};
  EXPECT_EQ(outcome_of(judge(before, before, {Expectation::preserved("config.ini", "config.ini")}), 0),
            Outcome::Met);

  const TreeSnapshot wrecked{{"config.ini", regular(H('b'))}};
  const auto v = judge(before, wrecked, {Expectation::preserved("config.ini", "config.ini")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(v, 0), "config.ini was changed; it no longer has its original bytes")
      << "the self form must not say 'config.ini does not have the bytes config.ini had'";
}

// --- Identical ---------------------------------------------------------------

TEST(JudgeIdentical, IsMetWhenBothSidesHoldTheSameBytes) {
  const TreeSnapshot after{{"config.ini", regular(H('a'))}, {"config.ini.bak", regular(H('a'))}};
  const auto v = judge({}, after, {Expectation::identical("config.ini", "config.ini.bak")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
}

TEST(JudgeIdentical, IsUnmetWhenTheyDiffer) {
  const TreeSnapshot after{{"config.ini", regular(H('a'))}, {"config.ini.bak", regular(H('b'))}};
  const auto v = judge({}, after, {Expectation::identical("config.ini", "config.ini.bak")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(v, 0), "config.ini and config.ini.bak differ");
}

TEST(JudgeIdentical, IsUnmetWhenEitherSideIsMissing) {
  const TreeSnapshot only_original{{"config.ini", regular(H('a'))}};
  const auto v = judge({}, only_original, {Expectation::identical("config.ini", "config.ini.bak")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(v, 0),
            "config.ini.bak does not exist; it should hold the same bytes as config.ini");
}

TEST(JudgeIdentical, IsUndecidableWhenASideHasNoContentHash) {
  const TreeSnapshot after{{"a", regular(H('a'))}, {"b", unreadable()}};
  const auto v = judge({}, after, {Expectation::identical("a", "b")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Undecidable);
}

// --- Vacuity -----------------------------------------------------------------

TEST(JudgeVacuity, AnExpectationTheBaselineAlreadySatisfiedIsMarked) {
  const TreeSnapshot before{{"README.md", regular(H('a'))}};
  const auto v = judge(before, before, {Expectation::exists("README.md")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
  EXPECT_TRUE(v.findings.at(0).vacuous) << "the model could pass this by doing nothing";
  EXPECT_EQ(v.vacuous(), 1u);
}

TEST(JudgeVacuity, AbsentIsVacuousWhenThePathNeverExisted) {
  const auto v = judge({}, TreeSnapshot{}, {Expectation::absent("never_here.txt")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
  EXPECT_TRUE(v.findings.at(0).vacuous);
}

TEST(JudgeVacuity, AGenuineGoalIsNotMarked) {
  const TreeSnapshot before{{"draft_v1.md", regular(H('a'))}};
  const TreeSnapshot after{{"final.md", regular(H('a'))}};
  const auto v = judge(before, after,
                       {Expectation::exists("final.md"), Expectation::absent("draft_v1.md")});
  EXPECT_TRUE(v.met());
  EXPECT_EQ(v.vacuous(), 0u) << "neither was true before the run";
}

TEST(JudgeVacuity, DoNotTouchIsNeverMarkedVacuous) {
  // Preserved(p -> p) is a constraint, not a goal: it is *meant* to hold before and after.
  // Marking it would fire on every legitimate "leave this alone" and teach a reader to
  // skim the flag -- the same argument that keeps TouchedOnly separate from Modified.
  const TreeSnapshot before{{"config.ini", regular(H('a'))}};
  const auto v = judge(before, before, {Expectation::preserved("config.ini", "config.ini")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
  EXPECT_FALSE(v.findings.at(0).vacuous);
  EXPECT_EQ(v.vacuous(), 0u);
}

TEST(JudgeVacuity, AMoveWhoseDestinationAlreadyHeldTheBytesIsMarked) {
  const TreeSnapshot before{{"a.txt", regular(H('a'))}, {"b.txt", regular(H('a'))}};
  const auto v = judge(before, before, {Expectation::preserved("a.txt", "b.txt")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
  EXPECT_TRUE(v.findings.at(0).vacuous);
}

// --- Verdict -----------------------------------------------------------------

TEST(JudgeVerdict, UndecidableIsNotMet) {
  // The whole reason Outcome has three states: a judge that could not look has not agreed.
  const TreeSnapshot after{{"final.md", regular(H('a'))}};
  const auto v = judge({}, after, {Expectation::preserved("gone.md", "final.md")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Undecidable);
  EXPECT_FALSE(v.met());
  EXPECT_EQ(v.count(Outcome::Met), 0u);
  EXPECT_EQ(v.count(Outcome::Undecidable), 1u);
}

TEST(JudgeVerdict, AnEmptyExpectationSetIsMet) {
  // Nothing stated is nothing to fail. A caller that supplies none gets verify.h's
  // report-only behaviour, which is D13's position 2 kept as the default.
  const auto v = judge({}, TreeSnapshot{}, ExpectationSet{});
  EXPECT_TRUE(v.met());
  EXPECT_TRUE(v.findings.empty());
  EXPECT_FALSE(v.first_unmet().has_value());
}

TEST(JudgeVerdict, FirstUnmetSkipsUndecidableAndFollowsDeclarationOrder) {
  const TreeSnapshot before{{"a.txt", regular(H('a'))}};
  const TreeSnapshot after{{"a.txt", regular(H('a'))}};
  const ExpectationSet expected{
      Expectation::preserved("never.txt", "a.txt"),  // undecidable -- skipped
      Expectation::exists("second.md"),              // unmet -- this is the one
      Expectation::exists("third.md"),               // also unmet, but later
  };
  const auto v = judge(before, after, expected);
  ASSERT_EQ(outcome_of(v, 0), Outcome::Undecidable);
  ASSERT_TRUE(v.first_unmet().has_value());
  EXPECT_EQ(v.first_unmet()->reason, "second.md does not exist")
      << "R7 must restate something the model can act on, in dependency order";
}

TEST(JudgeVerdict, FirstUnmetIsNullWhenEverythingIsMet) {
  const TreeSnapshot after{{"final.md", regular(H('a'))}};
  const auto v = judge({}, after, {Expectation::exists("final.md")});
  EXPECT_FALSE(v.first_unmet().has_value());
}

TEST(JudgeVerdict, RenderNamesEachFindingItsReasonAndItsVacuity) {
  const TreeSnapshot before{{"README.md", regular(H('a'))}};
  const auto v = judge(before, before,
                       {Expectation::exists("README.md"), Expectation::exists("missing.md")});
  EXPECT_EQ(v.render(),
            "met  README.md exists  (already true before the run)\n"
            "unmet  missing.md does not exist\n")
      << "an unmet line is the reason alone: it must stand by itself, because R7 hands "
         "exactly that string to a model with no other context";
}

TEST(JudgeVerdict, RenderShowsBothSidesForAnUndecidableFinding) {
  // The one outcome that needs the expectation *and* the reason: "a.txt could not be read"
  // does not say which expectation it defeated.
  const TreeSnapshot before{{"a.txt", unreadable()}};
  const TreeSnapshot after{{"b.txt", regular(H('a'))}};
  const auto v = judge(before, after, {Expectation::preserved("a.txt", "b.txt")});
  EXPECT_EQ(v.render(),
            "undecidable  b.txt has the bytes a.txt had: a.txt could not be read in the "
            "baseline, so its bytes cannot be compared\n");
}

TEST(JudgeVerdict, ExpectationsRenderAsReadableSentences) {
  EXPECT_EQ(Expectation::exists("final.md").render(), "final.md exists");
  EXPECT_EQ(Expectation::exists("release", true).render(), "release exists as a directory");
  EXPECT_EQ(Expectation::absent("draft_v1.md").render(), "draft_v1.md is absent");
  EXPECT_EQ(Expectation::preserved("a.txt", "b.txt").render(), "b.txt has the bytes a.txt had");
  EXPECT_EQ(Expectation::preserved("config.ini", "config.ini").render(), "config.ini is unchanged");
  EXPECT_EQ(Expectation::identical("a", "b").render(), "a and b are identical");
}

// --- The recorded failures, as scenarios -------------------------------------
//
// 04_rename: "Rename draft_v1.md to final.md. The content must not change and draft_v1.md
// must no longer exist." Every case below is a failure mode that actually occurred across
// the 259 recorded bench/fsops runs, with the count of times its post-condition failed.

namespace {
ExpectationSet rename_task() {
  return {Expectation::exists("final.md"), Expectation::absent("draft_v1.md"),
          Expectation::preserved("draft_v1.md", "final.md")};
}
const TreeSnapshot kRenameBaseline{{"draft_v1.md", regular(H('a'))}};
}  // namespace

TEST(JudgeRenameScenario, CopiedInsteadOfMoved) {
  // "draft_v1.md is gone" failed 15 times.
  const TreeSnapshot after{{"draft_v1.md", regular(H('a'))}, {"final.md", regular(H('a'))}};
  const auto v = judge(kRenameBaseline, after, rename_task());
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
  EXPECT_EQ(outcome_of(v, 1), Outcome::Unmet);
  EXPECT_EQ(outcome_of(v, 2), Outcome::Met);
  ASSERT_TRUE(v.first_unmet().has_value());
  EXPECT_EQ(v.first_unmet()->reason, "draft_v1.md still exists");
}

TEST(JudgeRenameScenario, ContentWasRewrittenRatherThanMoved) {
  // "content unchanged by rename" failed 17 times -- the most-failed post-condition in the
  // whole recorded set, and the one existence checks cannot see.
  const TreeSnapshot after{{"final.md", regular(H('b'))}};
  const auto v = judge(kRenameBaseline, after, rename_task());
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met);
  EXPECT_EQ(outcome_of(v, 1), Outcome::Met);
  EXPECT_EQ(outcome_of(v, 2), Outcome::Unmet);
  ASSERT_TRUE(v.first_unmet().has_value());
  EXPECT_EQ(v.first_unmet()->reason, "final.md does not have the bytes draft_v1.md had");
}

TEST(JudgeRenameScenario, DidNothingAndSaidDone) {
  // R6's headline evidence: llama32-3b replied DONE on an untouched tree in 18 of its 27
  // failed runs in sweep 1. Declaration order is what makes the restatement the useful
  // one rather than an arbitrary member of the set.
  const auto v = judge(kRenameBaseline, kRenameBaseline, rename_task());
  EXPECT_FALSE(v.met());
  EXPECT_EQ(v.count(Outcome::Unmet), 3u);
  ASSERT_TRUE(v.first_unmet().has_value());
  EXPECT_EQ(v.first_unmet()->reason, "final.md does not exist");
  EXPECT_EQ(v.vacuous(), 0u) << "nothing here was satisfiable by inaction";
}

TEST(JudgeRenameScenario, DoneCorrectly) {
  const TreeSnapshot after{{"final.md", regular(H('a'))}};
  const auto v = judge(kRenameBaseline, after, rename_task());
  EXPECT_TRUE(v.met());
  EXPECT_FALSE(v.first_unmet().has_value());
}

// --- 05_copy: why four predicates and not three ------------------------------

TEST(JudgeCopyScenario, PreservedCatchesTheDestructiveOverwriteThatIdenticalMisses) {
  // The case the fourth predicate is *not* sufficient for, and the reason both exist.
  //
  // bench/fsops grades 05_copy with "config.ini.bak exists", "original config.ini still
  // exists" (existence only) and "copy is identical to original". In both recorded
  // destructive runs the middle one PASSED while config.ini was being overwritten --
  // `collateral_modified: ['config.ini']` sits beside `[PASS] original config.ini still
  // exists`. A run that destroyed the original and then copied the corrupted bytes
  // satisfies all three, because both files hold the same wrong content.
  const TreeSnapshot before{{"config.ini", regular(H('a'))}};
  const TreeSnapshot after{{"config.ini", regular(H('b'))},       // overwritten
                           {"config.ini.bak", regular(H('b'))}};  // copy of the damage

  const ExpectationSet full{Expectation::exists("config.ini.bak"),
                            Expectation::preserved("config.ini", "config.ini"),
                            Expectation::identical("config.ini", "config.ini.bak")};
  const auto v = judge(before, after, full);
  EXPECT_FALSE(v.met());
  EXPECT_EQ(outcome_of(v, 0), Outcome::Met) << "the copy is there";
  EXPECT_EQ(outcome_of(v, 1), Outcome::Unmet) << "and the original was destroyed";
  EXPECT_EQ(outcome_of(v, 2), Outcome::Met) << "both sides hold the same wrong bytes";
  ASSERT_TRUE(v.first_unmet().has_value());
  EXPECT_EQ(v.first_unmet()->reason,
            "config.ini was changed; it no longer has its original bytes");

  // Stated as its own assertion because it is the argument for keeping both: Identical
  // alone passes this run outright.
  EXPECT_TRUE(judge(before, after, {Expectation::identical("config.ini", "config.ini.bak")}).met())
      << "Identical cannot see a destroyed original";
}

TEST(JudgeCopyScenario, IdenticalCatchesTheBadCopyThatPreservedMisses) {
  // The mirror, which is why Preserved alone is not sufficient either. The original is
  // untouched and the copy is invented.
  const TreeSnapshot before{{"config.ini", regular(H('a'))}};
  const TreeSnapshot after{{"config.ini", regular(H('a'))}, {"config.ini.bak", regular(H('c'))}};

  EXPECT_TRUE(judge(before, after, {Expectation::preserved("config.ini", "config.ini")}).met())
      << "Preserved cannot see a bad copy";
  EXPECT_FALSE(judge(before, after, {Expectation::identical("config.ini", "config.ini.bak")}).met());
}

TEST(JudgeCopyScenario, DoneCorrectly) {
  const TreeSnapshot before{{"config.ini", regular(H('a'))}};
  const TreeSnapshot after{{"config.ini", regular(H('a'))}, {"config.ini.bak", regular(H('a'))}};
  const auto v = judge(before, after,
                       {Expectation::exists("config.ini.bak"),
                        Expectation::preserved("config.ini", "config.ini"),
                        Expectation::identical("config.ini", "config.ini.bak")});
  EXPECT_TRUE(v.met());
  EXPECT_EQ(v.vacuous(), 0u);
}

// --- The residue, stated rather than absorbed --------------------------------

TEST(JudgeVerdict, UnjudgedRequirementsAreCarriedNotForgotten) {
  // 08_write_and_run_script needs "output contains TXTCOUNT=3", a reply marker and not a
  // tree fact. Without this counter a verdict on that task reads as success while a real
  // requirement went unexamined -- which is the confident-wrongness this layer exists to
  // avoid, one level up.
  const TreeSnapshot after{{"count.sh", regular(H('a'))}};
  auto v = judge({}, after, {Expectation::exists("count.sh")});
  EXPECT_EQ(v.unjudged, 0u) << "judge() never invents one; the caller states it";
  v.unjudged = 1;  // as a caller who knows a requirement is inexpressible would
  EXPECT_TRUE(v.met()) << "every stated expectation holds";
  EXPECT_EQ(v.unjudged, 1u) << "and one requirement was never looked at";
}

TEST(JudgeVerdict, AStructurallyPerfectRunCanStillBeWrong) {
  // D13's live failure, pinned so nobody mistakes this file for more than it is: asked for
  // a one-line summary, llama3.2-3b wrote the literal text of a shell command into
  // report.md. Exists() is satisfied by it perfectly. That is the semantic 10.7%, and it
  // is not what four structural predicates are for.
  // Asserted as the property it is -- content is not consulted -- rather than as prose:
  // two trees whose report.md differ in every byte reach the same verdict.
  const ExpectationSet asked{Expectation::exists("report.md")};
  const TreeSnapshot a_summary{{"report.md", regular(H('s'))}};
  const TreeSnapshot a_shell_command{{"report.md", regular(H('z'))}};  // "grep -oP ..."
  EXPECT_TRUE(judge({}, a_summary, asked).met());
  EXPECT_TRUE(judge({}, a_shell_command, asked).met());
  EXPECT_EQ(judge({}, a_summary, asked).render(), judge({}, a_shell_command, asked).render())
      << "structure holds identically; meaning is not judged here";
}

// --- Holes found by mutation testing, 2026-08-17 -----------------------------

TEST(JudgePreserved, AMissingBaselineOutranksAMissingDestination) {
  // Both sides absent. Swapping the two checks turns this into Unmet "b does not exist",
  // and R7 would spend a retry telling a model to create a file whose *source bytes* never
  // existed -- impossible to satisfy. The precedence is the point, and nothing pinned it:
  // every other missing-baseline test supplied a destination that was present.
  const auto v = judge({}, TreeSnapshot{}, {Expectation::preserved("never.txt", "b.txt")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Undecidable);
  EXPECT_EQ(reason_of(v, 0),
            "never.txt was not in the baseline, so there are no bytes to compare against")
      << "the operator's only sentence about this expectation";
  EXPECT_FALSE(v.first_unmet().has_value());
}

TEST(JudgeVacuity, IsNeverSetOnAFindingThatDidNotPass) {
  // `vacuous` means "the model could have passed this by doing nothing". A finding that
  // did not pass did not pass by any route, and "unmet ... (already true before the run)"
  // contradicts the line it sits on.
  const TreeSnapshot before{{"README.md", regular(H('a'))}};
  const auto deleted = judge(before, TreeSnapshot{}, {Expectation::exists("README.md")});
  EXPECT_EQ(outcome_of(deleted, 0), Outcome::Unmet);
  EXPECT_FALSE(deleted.findings.at(0).vacuous);
  EXPECT_EQ(deleted.vacuous(), 0u);
  EXPECT_EQ(deleted.render(), "unmet  README.md does not exist\n");

  const auto undecided = judge({}, TreeSnapshot{}, {Expectation::preserved("nope", "x")});
  EXPECT_EQ(outcome_of(undecided, 0), Outcome::Undecidable);
  EXPECT_FALSE(undecided.findings.at(0).vacuous)
      << "the judge could not evaluate it, so it cannot have been already true";
}

TEST(JudgeIdentical, ComparingAPathWithItselfIsRefused) {
  // Met for any readable regular file whatever its content, so it would carry a verdict
  // while proving only that the file exists -- and unlike Preserved(p -> p) it is not a
  // constraint, it is a tautology. Caller error, so it goes to the operator.
  const TreeSnapshot after{{"out.txt", regular(H('a'))}};
  const auto v = judge({}, after, {Expectation::identical("out.txt", "out.txt")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Undecidable);
  EXPECT_EQ(reason_of(v, 0), "out.txt is compared with itself, which decides nothing");
  EXPECT_FALSE(v.met());
}

TEST(JudgeIdentical, TwoShapesThatAreBothUnhashableCannotBeToldApart) {
  // One of each is decided -- a directory is not a file holding bytes. Two of a kind is
  // not: neither is hashed, so the judge has nothing to compare.
  const TreeSnapshot after{{"a", directory()}, {"b", directory()}};
  const auto v = judge({}, after, {Expectation::identical("a", "b")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Undecidable);
  EXPECT_EQ(reason_of(v, 0),
            "a is a directory and b is a directory; neither is a regular file, so their "
            "contents cannot be compared");

  // Each side named from its own state, so a mismatched pair reads correctly too.
  const TreeSnapshot mismatched{{"a", directory()}, {"b", not_regular()}};
  const auto m = judge({}, mismatched, {Expectation::identical("a", "b")});
  EXPECT_EQ(outcome_of(m, 0), Outcome::Undecidable);
  EXPECT_EQ(reason_of(m, 0),
            "a is a directory and b is a FIFO, socket or device; neither is a regular file, "
            "so their contents cannot be compared");

  const TreeSnapshot mixed{{"a", regular(H('a'))}, {"b", directory()}};
  EXPECT_EQ(outcome_of(judge({}, mixed, {Expectation::identical("a", "b")}), 0), Outcome::Unmet);
}

TEST(JudgeVerdict, AMetFindingCarriesNoReason) {
  // judge.h states it; nothing enforced it, and render() hides a stale reason on a met
  // line. The R7 consumer is where it would surface.
  const TreeSnapshot before{{"config.ini", regular(H('a'))}};
  const auto v = judge(before, before,
                       {Expectation::exists("config.ini"),
                        Expectation::preserved("config.ini", "config.ini")});
  ASSERT_TRUE(v.met());
  for (const auto& f : v.findings) EXPECT_TRUE(f.reason.empty()) << f.reason;
}

TEST(JudgeVerdict, FirstUnmetCarriesTheWholeFindingNotJustItsReason) {
  // Every call site so far reads only ->reason, so a mangled copy would pass unnoticed.
  const TreeSnapshot before{{"draft_v1.md", regular(H('a'))}};
  const auto v = judge(before, before,
                       {Expectation::exists("final.md"), Expectation::absent("draft_v1.md")});
  const auto unmet = v.first_unmet();
  ASSERT_TRUE(unmet.has_value());
  EXPECT_EQ(unmet->expectation, Expectation::exists("final.md"));
  EXPECT_EQ(unmet->outcome, Outcome::Unmet);
  EXPECT_FALSE(unmet->vacuous);
}

TEST(JudgeVerdict, FirstUnmetSurvivesTheTemporaryItCameFrom) {
  // The R7 call site's natural shape. This returned a pointer into the temporary's vector
  // until review 2026-08-17; ASan called it a heap-use-after-free on the first read.
  const TreeSnapshot before{{"a.txt", regular(H('a'))}};
  const auto unmet =
      judge(before, TreeSnapshot{}, {Expectation::exists("gone.md")}).first_unmet();
  ASSERT_TRUE(unmet.has_value());
  EXPECT_EQ(unmet->reason, "gone.md does not exist");
}

TEST(JudgeExpectation, TheNamedConstructorsLeaveTheIrrelevantFieldsEmpty) {
  // judge.h claims these "keep `other` meaningless on an Absent and `must_be_dir`
  // meaningless on a Preserved". Nothing read those fields, so nothing checked it -- and
  // the defaulted operator== makes an expectation comparable, so a stray field would make
  // two logically identical expectations compare unequal.
  const auto e = Expectation::exists("p");
  EXPECT_TRUE(e.other.empty());
  EXPECT_FALSE(e.must_be_dir);

  const auto a = Expectation::absent("p");
  EXPECT_TRUE(a.other.empty());
  EXPECT_FALSE(a.must_be_dir);

  const auto pr = Expectation::preserved("from", "to");
  EXPECT_FALSE(pr.must_be_dir);
  EXPECT_EQ(pr.path, "from");
  EXPECT_EQ(pr.other, "to");

  const auto id = Expectation::identical("x", "y");
  EXPECT_FALSE(id.must_be_dir);
  EXPECT_EQ(id.path, "x");
  EXPECT_EQ(id.other, "y");

  EXPECT_EQ(Expectation::exists("p"), Expectation::exists("p"));
  EXPECT_NE(Expectation::exists("p"), Expectation::exists("p", true));
  EXPECT_NE(Expectation::exists("p"), Expectation::absent("p"));
  EXPECT_NE(Expectation::preserved("a", "b"), Expectation::identical("a", "b"));
}

TEST(JudgeExists, TheDirectoryRequirementTravelsWithTheReason) {
  // "p does not exist" said to a model asked for a directory reliably gets a regular file
  // back, and correcting that costs a second retry naming it up front does not.
  const auto v = judge({}, TreeSnapshot{}, {Expectation::exists("release", true)});
  EXPECT_EQ(reason_of(v, 0), "release does not exist; a directory is expected there");

  const TreeSnapshot linked{{"release", symlink()}};
  const auto w = judge({}, linked, {Expectation::exists("release", true)});
  EXPECT_EQ(reason_of(w, 0), "release is a symlink, not a directory")
      << "saying only 'is not a directory' reads as false to anyone running ls -ld";
}

// --- Bugs introduced by the first round of fixes, found by review 2026-08-17 -----

TEST(JudgeIdentical, TheVerdictDoesNotDependOnArgumentOrder) {
  // Identity is symmetric and the verdict has to be. Checking refusal before kind made
  // Identical(a,b) Unmet and Identical(b,a) Undecidable on the *same tree* -- and the
  // second direction was the stall the three-state outcome exists to prevent: met() false
  // with nothing for R7 to restate.
  const TreeSnapshot t{{"a", directory()}, {"b", unreadable()}};
  const auto forward = judge({}, t, {Expectation::identical("a", "b")});
  const auto reversed = judge({}, t, {Expectation::identical("b", "a")});
  EXPECT_EQ(outcome_of(forward, 0), Outcome::Unmet);
  EXPECT_EQ(outcome_of(reversed, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(forward, 0), reason_of(reversed, 0)) << "same tree, same sentence";
  EXPECT_TRUE(forward.first_unmet().has_value());
  EXPECT_TRUE(reversed.first_unmet().has_value());
}

TEST(JudgeIdentical, ASideThatCanNeverSupplyBytesIsNamedRatherThanTheMissingOne) {
  // Telling a model "a does not exist; it should hold the same bytes as b" when b is a
  // directory sends it to copy bytes that do not exist. The unsatisfiable side is the one
  // worth naming. Same discipline the Preserved ordering already got.
  const TreeSnapshot t{{"b", directory()}};
  const auto v = judge({}, t, {Expectation::identical("a", "b")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(v, 0), "b is a directory; it cannot hold the same bytes as a");
}

TEST(JudgeIdentical, BothSidesMissingIsUndecidableNotAnErrandForTheModel) {
  const auto v = judge({}, TreeSnapshot{}, {Expectation::identical("a", "b")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Undecidable);
  EXPECT_EQ(reason_of(v, 0), "neither a nor b exists, so there is nothing to compare");
  EXPECT_FALSE(v.first_unmet().has_value());
}

TEST(JudgePreserved, AGuardedFileThatIsDeletedIsDecidedEvenIfItWasUnreadable) {
  // Deletion needs no baseline hash: whatever bytes it held, they did not survive. This
  // returned an operator-only note claiming the file "never had bytes to preserve" -- both
  // unactionable and false, since it had bytes and we were refused them.
  const TreeSnapshot before{{"config.ini", unreadable()}};
  const auto v = judge(before, TreeSnapshot{},
                       {Expectation::preserved("config.ini", "config.ini")});
  EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet);
  EXPECT_EQ(reason_of(v, 0), "config.ini no longer exists; its original bytes are gone");
  EXPECT_TRUE(v.first_unmet().has_value());
}

TEST(JudgePreserved, AGuardedFileSimplyDeletedSaysSoInItsOwnTerms) {
  const TreeSnapshot before{{"config.ini", regular(H('a'))}};
  const auto v = judge(before, TreeSnapshot{},
                       {Expectation::preserved("config.ini", "config.ini")});
  EXPECT_EQ(reason_of(v, 0), "config.ini no longer exists; its original bytes are gone");

  const TreeSnapshot swapped{{"config.ini", directory()}};
  const auto w = judge(before, swapped, {Expectation::preserved("config.ini", "config.ini")});
  EXPECT_EQ(reason_of(w, 0),
            "config.ini is now a directory; it should still hold its original bytes");
}

TEST(JudgeExists, TheDirectoryContrastReadsCorrectlyForEveryShape) {
  // "release is not a regular file, not a directory" stacked two negations, and
  // "release is unreadable, not a directory" blamed permissions for a type mismatch --
  // the plausible model response was a chmod when the answer is mkdir. Readability is not
  // part of what something *is*.
  struct Case {
    FileState state;
    std::string expected;
  };
  const Case cases[] = {
      {regular(H('a')), "release is a regular file, not a directory"},
      {symlink(), "release is a symlink, not a directory"},
      {not_regular(), "release is a FIFO, socket or device, not a directory"},
      {unreadable(), "release is a regular file, not a directory"},
  };
  for (const auto& c : cases) {
    const TreeSnapshot after{{"release", c.state}};
    const auto v = judge({}, after, {Expectation::exists("release", true)});
    EXPECT_EQ(outcome_of(v, 0), Outcome::Unmet) << c.expected;
    EXPECT_EQ(reason_of(v, 0), c.expected);
  }
}

TEST(JudgeContentGuard, AStaleHashOnAShapeThatCannotHoldOneNeverProducesAMet) {
  // The type flags are checked before the hash is looked at, so a hand-built map -- or a
  // future verify.cpp that carried a hash forward onto a directory or a now-unreadable
  // file -- cannot manufacture a false Met. Every term of the guard gets its own shape;
  // testing only the empty-hash case left the rest unpinned.
  const std::string sha = H('a');
  FileState dir_with_hash = directory();
  dir_with_hash.sha256 = sha;
  FileState link_with_hash = symlink();
  link_with_hash.sha256 = sha;
  FileState unreadable_with_hash = unreadable();
  unreadable_with_hash.sha256 = sha;

  const TreeSnapshot before{{"a", regular(sha)}};
  for (const auto& [name, state] : std::vector<std::pair<std::string, FileState>>{
           {"dir", dir_with_hash}, {"symlink", link_with_hash},
           {"unreadable", unreadable_with_hash}}) {
    const TreeSnapshot after{{"b", state}};
    const auto v = judge(before, after, {Expectation::preserved("a", "b")});
    EXPECT_NE(outcome_of(v, 0), Outcome::Met)
        << name << " carried a matching hash and must still not compare equal";
    const TreeSnapshot both{{"a", regular(sha)}, {"b", state}};
    EXPECT_NE(outcome_of(judge({}, both, {Expectation::identical("a", "b")}), 0), Outcome::Met)
        << name << " via Identical";
  }
}

// --- satisfies: never decided here --------------------------------------------

TEST(JudgeSatisfies, TheStructuralJudgeAnswersACriterionUndecidableNeverMet) {
  // Meaning is not decidable from snapshots. In production the parse layer routes
  // criteria to the semantic judge and this arm never runs; a caller that hands one in
  // anyway must get a fail-closed Undecidable, not a fabricated answer -- and never a
  // Met, which would count toward verdict.met().
  const auto v = judge({}, hermit::supervisor::TreeSnapshot{},
                       {Expectation::satisfies("report.md", "a one-line summary")});
  ASSERT_EQ(v.findings.size(), 1u);
  EXPECT_EQ(v.findings[0].outcome, Outcome::Undecidable);
  EXPECT_FALSE(v.met());
}

TEST(JudgeSatisfies, ADecidedCriterionRendersAsAJudgmentNotAMeasurement) {
  // The one report-surface rule of D15: a hash line and a judged line must never read
  // alike. Undecidable lines already say the judge did not decide, so the label sits
  // only where a decision was made.
  hermit::supervisor::Verdict v;
  v.findings.push_back({.expectation = Expectation::satisfies("report.md", "a summary"),
                        .outcome = Outcome::Met});
  v.findings.push_back({.expectation = Expectation::satisfies("tally.txt", "a count"),
                        .outcome = Outcome::Unmet,
                        .reason = "tally.txt holds a shell command"});
  v.findings.push_back({.expectation = Expectation::satisfies("notes.txt", "kept"),
                        .outcome = Outcome::Undecidable,
                        .reason = "not judged: the structural verdict did not pass"});

  const std::string text = v.render();
  const auto marks = [&](std::string_view line_key) {
    const auto at = text.find(line_key);
    if (at == std::string::npos) return false;
    const auto eol = text.find('\n', at);
    return text.substr(at, eol - at).find("the model's judgment, not a measurement") !=
           std::string::npos;
  };
  EXPECT_TRUE(marks("report.md"));
  EXPECT_TRUE(marks("tally.txt"));
  EXPECT_FALSE(marks("notes.txt"));
}

