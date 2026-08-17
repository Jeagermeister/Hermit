#pragma once

// R6's judgment half -- deciding whether stated expectations hold, from the tree.
//
// [verify.h](./verify.h) answers *what changed*. It says outright that it does not answer
// *is the work done*, because that needs a post-condition and a free-text instruction does
// not carry one. This is the other half, and it is deliberately narrow.
//
// --- Why this is smaller than "did the model do the job" ---------------------
//
// D13 framed the judgment half as blocked on post-conditions and then treated
// "post-condition" as one thing. It is two, and they have very different costs:
//
//   * **Structural** -- does this path exist, is that one gone, did these bytes survive the
//     move. A closed vocabulary, decidable from two snapshots, no content knowledge.
//   * **Semantic** -- is this a *correct* one-line summary of `notes.txt`. An open
//     vocabulary that needs a judge of meaning.
//
// D13's live example is semantic: asked for a summary, `llama3.2-3b` wrote the literal
// text `grep -oP '(?<=^).*' notes.txt` into `report.md`. Nothing here catches that, and
// nothing here pretends to -- `Exists("report.md")` is satisfied by it perfectly.
//
// The reason to build the structural half anyway is that it is where the failures are.
// Classifying every failing post-condition across the 259 recorded `bench/fsops` runs:
//
//   existence / absence            286   69.2%   this file
//   hash preserved across a move    43   10.4%   this file
//   content semantics               44   10.7%   not this file
//   reply / script output           40    9.7%   not the filesystem at all
//
// Four predicates cover 329 of 413 observed failures. The residue is real and is named
// rather than absorbed -- see `Verdict::unjudged`.
//
// --- Snapshots, not changesets ----------------------------------------------
//
// This takes two `TreeSnapshot`s, not a `Changeset`, and the distinction is not
// bookkeeping. A file that existed at baseline and was never touched produces *no entry*
// in a changeset, so `Exists("keep.md")` is unanswerable from one. A changeset is what a
// diff is *for*; judgment reads the snapshots the diff was derived from.
//
// --- The three failure kinds do not behave alike ------------------------------
//
//   the tree cannot be *seen*        fail closed -- the run stops (verify.h, existing)
//   an expectation is *unmet*        data -- feeds R7 re-invocation, never stops hard
//   an expectation is *undecidable*  reported to the operator, never sent to the model
//
// The line between the last two is *not* "did we get a hash". It is whether the judge
// established the fact. Refused a file by its permissions, we did not -- the bytes may be
// right and we were not allowed to look, so `Undecidable`. Handed a directory where a
// file's bytes were expected, we did -- a directory does not hold those bytes, so `Unmet`,
// and the model can act on it. Review 2026-08-17 found these collapsed together, which
// made `met()` false while `first_unmet()` returned nothing: a run stalled on a verdict it
// could not act on, in exactly the case `Preserved` exists to catch.
//
// Measurement fails closed. Judgment never does. Collapsing the last two into a boolean
// would force a wrong answer whenever the judge cannot look -- and would send R7 to
// restate "one side could not be read" to a model that can do nothing with it, spending a
// retry on noise. That is why `Outcome` has three states and `first_unmet()` skips the
// third.
//
// Nothing here reads a reply, opens a file, or touches the filesystem. `judge` is a pure
// function of its arguments, so the whole table is testable from two hand-built maps.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <hermes/supervisor/verify.h>

namespace hermes::supervisor {

enum class ExpectationKind {
  /// The path is present in the tree afterwards. With `must_be_dir`, present *and* a
  /// directory -- a symlink to a directory does not satisfy it, because the snapshot
  /// records what `lstat` saw and this layer never follows links.
  Exists,

  /// The path is not present afterwards.
  Absent,

  /// The bytes at `other` afterwards are the bytes that were at `path` in the baseline.
  /// This is the move/rename/copy predicate, and `Preserved(p -> p)` is "do not touch p".
  Preserved,

  /// Two paths hold the same bytes as the tree stands now.
  ///
  /// Not a duplicate of `Preserved`, and `05_copy` is the proof. Its grader checks
  /// "original config.ini still exists" (existence only) and "copy is identical to
  /// original". In both recorded destructive runs the first PASSED while `config.ini` was
  /// being overwritten -- `collateral_modified: ['config.ini']` sits beside
  /// `[PASS] original config.ini still exists`. A run that destroyed the original and
  /// copied the corrupted bytes would satisfy `Identical` too, because both sides hold
  /// the same wrong content. Only `Preserved(config.ini -> config.ini)` catches that.
  /// Neither predicate implies the other; the pair is what closes R3's case.
  Identical,
};

/// One thing a caller states should be true when the work is done.
///
/// Built through the named constructors, never by filling in fields: they are what keeps
/// `other` meaningless on an `Absent` and `must_be_dir` meaningless on a `Preserved`.
struct Expectation {
  ExpectationKind kind = ExpectationKind::Exists;

  /// `Exists`/`Absent`: the path. `Preserved`: the source. `Identical`: the first side.
  std::string path{};

  /// `Preserved`: the destination. `Identical`: the second side. Empty otherwise.
  std::string other{};

  /// `Exists` only.
  bool must_be_dir = false;

  [[nodiscard]] static Expectation exists(std::string path, bool as_directory = false);
  [[nodiscard]] static Expectation absent(std::string path);
  [[nodiscard]] static Expectation preserved(std::string from, std::string to);
  [[nodiscard]] static Expectation identical(std::string a, std::string b);

  /// How the expectation reads in a report: "final.md exists", "draft_v1.md is absent".
  [[nodiscard]] std::string render() const;

  friend bool operator==(const Expectation&, const Expectation&) = default;
};

/// Declaration order is significant and is preserved through to the verdict: R7 restates
/// the *first* unmet finding, so an author who states expectations in dependency order --
/// create the directory before moving into it -- gets the next actionable step for free,
/// rather than an arbitrary one out of a set.
using ExpectationSet = std::vector<Expectation>;

enum class Outcome {
  Met,
  /// The judge looked and the answer is no. The model can act on this.
  Unmet,
  /// The judge could not look. Never counted as met, never sent to the model.
  Undecidable,
};

std::string_view to_string(Outcome o) noexcept;

struct Finding {
  Expectation expectation{};
  Outcome outcome = Outcome::Unmet;

  /// Why, in the words R7 would restate. Empty when `Met`.
  std::string reason{};

  /// The baseline already satisfied this, so the model *could* have passed it by doing
  /// nothing. Only ever set on a `Met` finding: a finding that did not pass did not pass by
  /// any route, and "unmet ... (already true before the run)" contradicts its own line.
  ///
  /// Not an error -- an expectation set that is *entirely* vacuous is, though, and that is
  /// only visible if each one is marked. It is the `EXPECT_LT` problem in another form: an
  /// assertion that holds under the failure it describes.
  ///
  /// **Known limit, stated rather than discovered later.** Vacuity is syntactic, and only
  /// `Preserved(p -> p)` announces itself as a constraint. `Exists(p)` meaning "do not
  /// delete p" and `Absent(p)` meaning "do not create p" are the same shape as goals and
  /// are indistinguishable from them here, so a set that is *all* guard-rails reports as
  /// entirely vacuous. That reading is correct -- such a set really is satisfiable by
  /// inaction -- but it is not the authoring error the flag is usually pointing at.
  ///
  /// `Preserved(p -> p)` is excluded by construction. It asserts *non*-change, so it is a
  /// constraint rather than a goal and is meant to hold before and after; flagging it
  /// would fire on every legitimate "do not touch this" and teach a reader to skim the
  /// flag -- the same reasoning that keeps `TouchedOnly` separate from `Modified`.
  bool vacuous = false;
};

/// What the stated expectations came to. Evidence about structure, never a claim about
/// meaning -- see `met()`.
struct Verdict {
  std::vector<Finding> findings;

  /// Requirements the caller knew about and could not express in four predicates, carried
  /// so that "all expectations met" cannot be mistaken for "the work is done".
  /// `08_write_and_run_script` needs "output contains TXTCOUNT=3", which is a reply
  /// marker and not a tree fact; without this counter a verdict on that task would read as
  /// success while a real requirement went unexamined.
  std::size_t unjudged = 0;

  /// Every finding is `Met`. `Undecidable` is **not** `Met` -- a judge that could not look
  /// has not agreed.
  ///
  /// True of an empty set, which is deliberate: no expectations means nothing to fail, and
  /// a caller that supplied none gets verify.h's report-only behaviour rather than a
  /// verdict. Read this as "the structure you stated holds", never as "the work is
  /// correct" -- a `report.md` containing a shell command satisfies `Exists("report.md")`.
  [[nodiscard]] bool met() const noexcept;

  [[nodiscard]] std::size_t count(Outcome o) const noexcept;

  /// Findings the baseline already satisfied.
  [[nodiscard]] std::size_t vacuous() const noexcept;

  /// The finding R7 restates: the first `Unmet` in declaration order, or nullopt.
  ///
  /// Skips `Undecidable` on purpose. Restating "one side could not be read" to a model
  /// spends a retry on a sentence it cannot act on.
  ///
  /// Returns a *copy*, not a pointer into `findings`. The natural R7 call site is
  /// `judge(before, after, expected).first_unmet()`, and a pointer into a temporary's
  /// vector is a use-after-free the moment it is dereferenced -- caught by ASan during
  /// review 2026-08-17, before any caller existed. A Finding is three short strings; one
  /// copy per turn is not a cost worth a dangling pointer.
  [[nodiscard]] std::optional<Finding> first_unmet() const;

  /// One line per finding, in declaration order, ending in a newline.
  [[nodiscard]] std::string render() const;
};

/// Decide whether `expected` holds, reading only the two snapshots.
///
/// `baseline` is the tree before the run -- `Preserved` needs it, and it is what makes the
/// vacuity check possible. `current` is the tree as it stands now.
[[nodiscard]] Verdict judge(const TreeSnapshot& baseline, const TreeSnapshot& current,
                            const ExpectationSet& expected);

}  // namespace hermes::supervisor
