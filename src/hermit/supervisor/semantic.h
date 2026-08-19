#pragma once

// The meaning half of judgment -- deciding `satisfies:` expectations, which judge.h
// deliberately does not: "is this a *correct* one-line summary of notes.txt" is an open
// vocabulary, and no pair of snapshots decides it. A model does, and that shapes every
// rule here (decided 2026-08-18, D15):
//
//   * **It reads the tree, never the transcript.** D13's discipline, applied to meaning:
//     the judge is handed the target file's bytes as they stand on disk, the stated
//     criterion, and the tree's relative path listing -- nothing the working model said,
//     nothing it thought, nothing it claimed. A fresh session with no history is what
//     "fresh pair of eyes" means mechanically; it is the same isolation argument as R7's
//     fresh attempts.
//
//   * **A judgment is labelled as one.** A hash comparison is a measurement; this is a
//     model's opinion, and the two must not read alike in a verdict. Every finding this
//     module produces has kind `Satisfies`, and `Verdict::render` marks those lines.
//     `met()` still counts them -- an operator who stated a criterion wants it enforced,
//     and R7 retries on an unmet one exactly as on a structural one -- but the report
//     always says which claims were measured and which were judged.
//
//   * **It fails closed, into Undecidable, never into Met.** A judge that could not run
//     -- transport down, reply unparseable, content binary, file unreadable -- has not
//     agreed. Undecidable is reported to the operator and never retried (judge.h's
//     rule: a sentence the model cannot act on is not worth a model run). One deliberate
//     asymmetry: a *missing* file is `Unmet`, not `Undecidable` -- absence is an
//     established fact, and "create it" is exactly the kind of failure R7 exists to
//     hand back.
//
//   * **The reply is constrained, not parsed hopefully.** The request carries a JSON
//     schema (`ChatRequest::Schema`) and no tools -- D12 measured that combination
//     working on seven of seven models, and it is the shape Tier 1's `triage` was
//     already promised. A reply that still fails to parse is Undecidable.
//
// Known limits and what would overturn this design: DECISIONS.md D15.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <hermit/core/fsio.h>
#include <hermit/core/sandbox.h>
#include <hermit/supervisor/judge.h>
#include <hermit/supervisor/loop.h>

namespace hermit::supervisor {

/// How the judge is reached and sized. `chat` is the transport seam (a scripted one in
/// tests, `Client::chat` in production); the rest is what every request must carry.
struct SemanticJudge {
  ChatFn chat;
  std::string model;

  /// The window every request names. Not optional: an unset num_ctx leaves the
  /// server default in force, which is "exactly the undetermined state R9 refuses"
  /// (client.h). The zero a default-constructed judge carries fails closed -- every
  /// criterion lands Undecidable against a 0-token window -- rather than silently
  /// inheriting the server's idea of a window.
  std::uint64_t num_ctx = 0;

  /// Generation cap, sent as num_predict. Bounded because the judge's whole answer is
  /// one boolean and one sentence: an unbounded judge -- a thinking model most of all
  /// -- can spend the entire window generating, per criterion, per attempt, limited
  /// only by the transport timeout. A thinking judge that exhausts this returns empty
  /// content, which parses to Undecidable -- fail closed, and visible.
  std::uint64_t max_tokens = 2048;

  /// Files past this are Undecidable, never truncated -- a judgment of half a file
  /// would silently read as a judgment of the whole one. See judge_one's truncation
  /// checks, which enforce the same rule against the window.
  std::uint64_t max_read_bytes = kDefaultMaxReadBytes;
};

/// Judge every criterion (all must be `Satisfies`) against the tree as it stands now,
/// one model call each, in declaration order. Bytes are read through `box` (R1) at call
/// time; the listing handed to the judge is walked at call time too, so the two are one
/// coherent observation.
[[nodiscard]] std::vector<Finding> judge_semantics(const SemanticJudge& judge,
                                                   const Sandbox& box,
                                                   const ExpectationSet& criteria);

}  // namespace hermit::supervisor
