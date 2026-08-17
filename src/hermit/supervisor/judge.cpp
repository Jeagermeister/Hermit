#include <hermit/supervisor/judge.h>

#include <algorithm>
#include <utility>

namespace hermit::supervisor {
namespace {

/// What an entry is, by type alone.
///
/// Readability is deliberately not part of this. Whether we were allowed to read a file
/// has nothing to do with whether it is a directory, and mixing the two produced
/// "release is unreadable, not a directory" -- a sentence that sends a model to chmod when
/// the answer is mkdir.
std::string_view describe_type(const FileState& s) noexcept {
  if (s.is_dir) return "a directory";
  if (s.is_symlink) return "a symlink";
  if (s.readable && s.sha256.empty()) return "a FIFO, socket or device";
  return "a regular file";
}

/// Structurally incapable of holding content, known from the type flags -- so it is a
/// *decided* fact whatever we were or were not allowed to read.
bool holds_no_bytes(const FileState& s) noexcept {
  return s.is_dir || s.is_symlink || (s.readable && s.sha256.empty());
}

/// A regular file we were refused. It may hold exactly the right bytes; we were not
/// allowed to look. This is the one shape that is genuinely undecidable.
bool was_refused(const FileState& s) noexcept { return !holds_no_bytes(s) && !s.readable; }

// `holds_no_bytes`, `was_refused` and "has comparable content" partition every FileState
// exactly, and both hash-comparing predicates below exclude the first two before comparing.
// So a side that reaches a comparison is readable, is neither a directory nor a symlink,
// and is not the readable-but-unhashed shape -- which leaves a non-empty sha256 by
// construction, with no appeal to any invariant living over in verify.cpp. That matters
// because `judge`'s selling point is purity over hand-built maps: a snapshot that ever
// carried a stale hash forward onto a directory or a now-unreadable file must not be able
// to produce a false `Met` here, and the type flags are checked before the hash is looked
// at, so it cannot.

struct Judgement {
  Outcome outcome = Outcome::Unmet;
  std::string reason;
};

Judgement met() { return Judgement{.outcome = Outcome::Met, .reason = {}}; }
Judgement unmet(std::string why) {
  return Judgement{.outcome = Outcome::Unmet, .reason = std::move(why)};
}
Judgement undecidable(std::string why) {
  return Judgement{.outcome = Outcome::Undecidable, .reason = std::move(why)};
}

const FileState* find(const TreeSnapshot& snapshot, const std::string& path) {
  const auto it = snapshot.find(path);
  return it == snapshot.end() ? nullptr : &it->second;
}

Judgement judge_one(const Expectation& e, const TreeSnapshot& baseline,
                    const TreeSnapshot& current) {
  const bool self = e.path == e.other;
  switch (e.kind) {
    case ExpectationKind::Exists: {
      const FileState* entry = find(current, e.path);
      if (entry == nullptr) {
        // The requirement travels with the reason. Saying only "does not exist" to a model
        // asked for a directory reliably gets a regular file back, and the correction costs
        // a second retry that naming it up front does not.
        return unmet(e.path + " does not exist" +
                     (e.must_be_dir ? "; a directory is expected there" : ""));
      }
      // A symlink to a directory has is_dir false: the snapshot records what lstat saw and
      // this layer never follows a link. Refusing it is the point -- but the sentence has
      // to say *why*, or "release exists but is not a directory" reads as simply false to
      // anyone who runs `ls -ld release`. Type only: readability is irrelevant to whether
      // something is a directory.
      if (e.must_be_dir && !entry->is_dir) {
        return unmet(e.path + " is " + std::string{describe_type(*entry)} + ", not a directory");
      }
      return met();
    }

    case ExpectationKind::Absent: {
      if (find(current, e.path) != nullptr) return unmet(e.path + " still exists");
      return met();
    }

    case ExpectationKind::Preserved: {
      const FileState* was = find(baseline, e.path);
      // Both baseline checks come before anything about the destination. With the order
      // reversed, an expectation whose source never had bytes escaped as "dst does not
      // exist" -- the model dutifully created it, and only then did the verdict turn
      // Undecidable, so the retry was spent on a provably impossible errand.
      if (was == nullptr) {
        return undecidable(e.path + " was not in the baseline, so there are no bytes to " +
                           "compare against");
      }
      if (holds_no_bytes(*was)) {
        return undecidable(e.path + " was " + std::string{describe_type(*was)} +
                           " in the baseline, so it never had bytes to preserve");
      }

      const FileState* now = find(current, e.other);
      // Deletion and wrong-kind are decided even when the baseline could not be read:
      // whatever bytes it held, a missing file and a directory do not hold them. Only a
      // comparison needs the baseline's bytes, so the refusal check waits until one is
      // actually required -- otherwise a guarded file that was unreadable and then deleted
      // came back as an operator note claiming it "never had bytes to preserve", which is
      // both unactionable and false.
      if (now == nullptr) {
        return unmet(self ? e.path + " no longer exists; its original bytes are gone"
                          : e.other + " does not exist; it should hold the bytes " + e.path +
                                " had");
      }
      if (holds_no_bytes(*now)) {
        return unmet(self ? e.path + " is now " + std::string{describe_type(*now)} +
                                "; it should still hold its original bytes"
                          : e.other + " is " + std::string{describe_type(*now)} +
                                "; it should hold the bytes " + e.path + " had");
      }
      if (was_refused(*was)) {
        return undecidable(e.path +
                           " could not be read in the baseline, so its bytes cannot be compared");
      }
      if (was_refused(*now)) return undecidable(e.other + " could not be read");

      if (was->sha256 != now->sha256) {
        // The self form has to read as what it is. "config.ini does not have the bytes
        // config.ini had" is the sentence R7 hands a model as its only context, and it is
        // nonsense; render() already special-cased the same shape and the reason did not.
        return unmet(self ? e.path + " was changed; it no longer has its original bytes"
                          : e.other + " does not have the bytes " + e.path + " had");
      }
      return met();
    }

    case ExpectationKind::Identical: {
      // A path compared with itself establishes nothing -- it is Met for any readable
      // regular file whatever its content, so it would pass a verdict while proving only
      // that the file exists. Caller error, and it goes to the operator.
      if (self) return undecidable(e.path + " is compared with itself, which decides nothing");

      const FileState* lhs = find(current, e.path);
      const FileState* rhs = find(current, e.other);

      // Wrong kind is checked on *both* sides before refusal on either, and before
      // existence. Identity is symmetric and the verdict has to be: checking refusal first
      // made Identical(a,b) Unmet and Identical(b,a) Undecidable on the same tree, and the
      // second direction was the stall this whole outcome split exists to prevent.
      // Checking existence first sent R7 to tell a model to make `a` hold bytes that a
      // directory, a refused file, or a missing file could never supply.
      const bool lhs_kindless = lhs != nullptr && holds_no_bytes(*lhs);
      const bool rhs_kindless = rhs != nullptr && holds_no_bytes(*rhs);
      if (lhs_kindless && rhs_kindless) {
        return undecidable(e.path + " is " + std::string{describe_type(*lhs)} + " and " +
                           e.other + " is " + std::string{describe_type(*rhs)} +
                           "; neither is a regular file, so their contents cannot be compared");
      }
      if (lhs_kindless) {
        return unmet(e.path + " is " + std::string{describe_type(*lhs)} +
                     "; it cannot hold the same bytes as " + e.other);
      }
      if (rhs_kindless) {
        return unmet(e.other + " is " + std::string{describe_type(*rhs)} +
                     "; it cannot hold the same bytes as " + e.path);
      }

      if (lhs != nullptr && was_refused(*lhs)) return undecidable(e.path + " could not be read");
      if (rhs != nullptr && was_refused(*rhs)) return undecidable(e.other + " could not be read");

      if (lhs == nullptr && rhs == nullptr) {
        return undecidable("neither " + e.path + " nor " + e.other +
                           " exists, so there is nothing to compare");
      }
      if (lhs == nullptr) {
        return unmet(e.path + " does not exist; it should hold the same bytes as " + e.other);
      }
      if (rhs == nullptr) {
        return unmet(e.other + " does not exist; it should hold the same bytes as " + e.path);
      }

      if (lhs->sha256 != rhs->sha256) return unmet(e.path + " and " + e.other + " differ");
      return met();
    }
  }
  return undecidable("unknown expectation kind");
}

/// Whether an expectation asserts that something *changes*.
///
/// Only those can be vacuous. `Preserved(p -> p)` asserts non-change -- it is a constraint
/// meant to hold before and after -- so marking it vacuous would fire on every legitimate
/// "do not touch this" and teach a reader to skim the flag.
bool asserts_a_change(const Expectation& e) noexcept {
  return !(e.kind == ExpectationKind::Preserved && e.path == e.other);
}

}  // namespace

std::string_view to_string(Outcome o) noexcept {
  switch (o) {
    case Outcome::Met:         return "met";
    case Outcome::Unmet:       return "unmet";
    case Outcome::Undecidable: return "undecidable";
  }
  return "unknown";
}

Expectation Expectation::exists(std::string path, bool as_directory) {
  return Expectation{.kind = ExpectationKind::Exists,
                     .path = std::move(path),
                     .other = {},
                     .must_be_dir = as_directory};
}

Expectation Expectation::absent(std::string path) {
  return Expectation{
      .kind = ExpectationKind::Absent, .path = std::move(path), .other = {}, .must_be_dir = false};
}

Expectation Expectation::preserved(std::string from, std::string to) {
  return Expectation{.kind = ExpectationKind::Preserved,
                     .path = std::move(from),
                     .other = std::move(to),
                     .must_be_dir = false};
}

Expectation Expectation::identical(std::string a, std::string b) {
  return Expectation{.kind = ExpectationKind::Identical,
                     .path = std::move(a),
                     .other = std::move(b),
                     .must_be_dir = false};
}

std::string Expectation::render() const {
  switch (kind) {
    case ExpectationKind::Exists:
      return path + (must_be_dir ? " exists as a directory" : " exists");
    case ExpectationKind::Absent:
      return path + " is absent";
    case ExpectationKind::Preserved:
      // The self case reads as what it is -- a constraint, not a move.
      return path == other ? path + " is unchanged"
                           : other + " has the bytes " + path + " had";
    case ExpectationKind::Identical:
      return path + " and " + other + " are identical";
  }
  return path;
}

bool Verdict::met() const noexcept {
  return std::ranges::all_of(findings,
                             [](const Finding& f) { return f.outcome == Outcome::Met; });
}

std::size_t Verdict::count(Outcome o) const noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(findings, [o](const Finding& f) { return f.outcome == o; }));
}

std::size_t Verdict::vacuous() const noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(findings, [](const Finding& f) { return f.vacuous; }));
}

std::optional<Finding> Verdict::first_unmet() const {
  const auto it = std::ranges::find_if(
      findings, [](const Finding& f) { return f.outcome == Outcome::Unmet; });
  if (it == findings.end()) return std::nullopt;
  return *it;
}

std::string Verdict::render() const {
  std::string text;
  for (const Finding& f : findings) {
    text += to_string(f.outcome);
    text += "  ";
    // What each line is *for* differs by outcome, so what it shows differs too. A met line
    // states what holds, and the expectation is the natural phrasing of that. An unmet
    // line states what is wrong, and every `reason` is written to stand alone -- R7 hands
    // exactly that string to a model with no other context, so it must. Printing both
    // would read "unmet  missing.md exists: missing.md does not exist", which says the
    // path twice and the outcome three times. An undecidable line needs both, because the
    // reason ("a.txt could not be read") does not say which expectation it defeated.
    switch (f.outcome) {
      case Outcome::Met:
        text += f.expectation.render();
        break;
      case Outcome::Unmet:
        text += f.reason;
        break;
      case Outcome::Undecidable:
        text += f.expectation.render();
        text += ": ";
        text += f.reason;
        break;
    }
    // Said on the line it applies to, because a count at the bottom of a report is a
    // number a reader has to go looking for the cause of.
    if (f.vacuous) text += "  (already true before the run)";
    text += '\n';
  }
  return text;
}

Verdict judge(const TreeSnapshot& baseline, const TreeSnapshot& current,
              const ExpectationSet& expected) {
  Verdict out;
  out.findings.reserve(expected.size());
  for (const Expectation& e : expected) {
    Judgement judged = judge_one(e, baseline, current);
    Finding finding{.expectation = e,
                    .outcome = judged.outcome,
                    .reason = std::move(judged.reason),
                    .vacuous = false};
    // Only a *met* finding can be vacuous. The flag means "the model could have passed
    // this by doing nothing", and a finding that did not pass did not pass by any route --
    // marking a deleted README as "already true before the run" is a sentence that
    // contradicts the line it sits on. It also skips the second judgement in the common
    // failing case.
    if (finding.outcome == Outcome::Met && asserts_a_change(e)) {
      finding.vacuous = judge_one(e, baseline, baseline).outcome == Outcome::Met;
    }
    out.findings.push_back(std::move(finding));
  }
  return out;
}

}  // namespace hermit::supervisor
