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
// **Known limits, stated rather than discovered later.** The judge sees one file's
// content plus the tree's paths, so a criterion needing another file's *content* ("a
// summary of notes.txt" judged for faithfulness) is checkable only as far as form and
// plausibility -- the judge cannot compare against bytes it was not given. And the
// content being judged is model-written: a hostile or degenerate file could try to
// address the judge directly. The prompt states that content is data, never
// instructions, which is a mitigation and not a guarantee -- the same standing caveat
// as every LLM-judge design.

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

  /// Files past this are Undecidable, never truncated: a judgment of half a file
  /// silently reads as a judgment of the file. The same rule is enforced against the
  /// WINDOW in judge_one, twice -- a cheap size gate before any tokens are spent, and
  /// the authoritative check after the reply: Ollama truncates an over-long prompt to
  /// num_ctx minus num_predict (measured), so prompt_tokens reaching that boundary is
  /// the truncation signature itself, independent of how densely the content
  /// tokenises. A chars-based estimate alone was measured failing open on
  /// digit-dense content. All of it lands Undecidable, never a verdict.
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
