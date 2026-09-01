#pragma once

// Compaction: rebuilding the window from the tree instead of summarising it.
//
// `Session::prepare()` trims groups off the front until the turn fits. That is correct,
// cheap, and **silently lossy** -- the early turns are gone and nothing tells the model
// what it has forgotten. A supervisor whose one job is noticing when a model is
// confidently wrong should not make it forget without saying so. This is the other half:
// when the prompt approaches the budget, throw the history away *and say what the world
// looks like now*, so what replaces it is observation rather than absence.
//
// --- Why this does not ask the model to summarise ---------------------------
//
// A chat client compacts by asking the model to summarise the conversation and putting
// the summary back in place of the history. That returns the model's prose account of
// events to the critical path, which is precisely what D13 removed and for a measured
// reason: handed a tool result whose `content` was the four characters `aaaa`,
// `llama3.2-3b` reported *"a.txt is 1 character long."* A summary is that failure with no
// snapshot behind it to catch it, and the roster makes it worse rather than better --
// 3-9B models are the weakest summarisers available and they are the entire target tier.
//
// So nothing here calls a model. Everything the note carries is either the task as the
// caller wrote it or a fact read off the filesystem by `TreeVerifier`:
//
//   - **the task** -- verbatim, never compressed; it is the ground truth of intent
//   - **the world** -- re-observed, not recalled, from the changeset the loop already
//     computes every turn
//   - **the unmet requirements** -- already structured, from the `Verdict`
//   - **the model's own narration** -- dropped entirely
//
// Deterministic, costs no tokens beyond the walk the loop was doing anyway, and cannot
// hallucinate. It is D13's argument moved from the turn to the window.
//
// --- What it cannot recover, stated rather than glossed ---------------------
//
// Intent *inside* a task. An approach the model tried, abandoned mid-turn, and never
// failed against leaves no trace in the tree and no entry in the verdict, so a rebuilt
// context can send it back down a path it had already reasoned its way out of. The
// verdict covers rejected branches; it does not cover abandoned ones. Whether that costs
// more than the trim's wholesale amnesia is an empirical question and belongs in
// hermit-bench, measured against the trim as the control on the same tasks -- which is
// what `LoopOptions::compact_at = 0` exists to make possible.
//
// --- Why the note is folded into the task turn rather than appended --------
//
// The obvious shape -- append the note as its own `user` turn -- is the one loop.h
// already forbids, and for a reason that applies unchanged here: `pin_latest_user` pins
// the *latest* user message, so a synthetic turn would become the one message the trim
// must never drop, and the real instruction would become droppable. The fabrication would
// outrank the task.
//
// Folding the note beneath the task keeps exactly one user turn, still pinned, still
// opening with the caller's own words. It is also the shape R7 already uses and has run
// against live models -- `reinvocation_instruction()` composes task-then-situation -- so
// this is a second instance of a working pattern rather than a new one.
//
// It carries R7's other discipline too, and for the same reason: the note is always
// composed from the **original** task, never from an already-composed one, so a third
// compaction does not nest three framings inside each other.

#include <cstddef>
#include <string>
#include <string_view>

#include <hermit/supervisor/judge.h>
#include <hermit/supervisor/session.h>
#include <hermit/supervisor/verify.h>

namespace hermit::supervisor {

/// Fraction of the prompt budget at which the window is rebuilt.
///
/// 0.80 is deliberately the same figure as the trim's hysteresis target
/// (`budget - budget / 5`), which makes the ordering between the two policies explicit
/// rather than incidental: compaction fires where the trim would have *landed*, so on a
/// verified run reconstruction is what normally happens and the trim is a genuine
/// backstop for the cases reconstruction cannot serve -- no verifier, or a rebuild that
/// would not be smaller than the history it replaces.
inline constexpr double kDefaultCompactAt = 0.80;

/// Most changed paths the note lists before it stops and gives a count instead.
///
/// A bound is not optional. The changeset is unbounded in principle -- a model that
/// rewrites a source tree produces thousands of entries -- and a note that grew with it
/// would be a compaction step that makes the prompt bigger. The total is always stated,
/// so nothing is hidden, only elided.
inline constexpr std::size_t kMaxListedChanges = 40;

/// Most unmet findings the note lists. Same reasoning; expectation sets are small in
/// practice, so this bound is a guard rather than a policy.
inline constexpr std::size_t kMaxListedFindings = 10;

/// Whether the prompt has reached `threshold` of the session's prompt budget.
///
/// A fraction rather than a token count because the budget varies with the window, and a
/// figure that means "nearly full" at 8K would mean "barely started" at 128K.
///
/// `threshold <= 0` is off, and so is NaN -- the comparison is written so that an
/// unordered value fails it rather than passing by accident. A threshold above 1.0 is
/// meaningful rather than nonsense: it defers compaction until the prompt is already over
/// budget, which is reachable because this is checked *before* `prepare()` trims.
[[nodiscard]] bool should_compact(const Session& session, double threshold) noexcept;

/// The state paragraph: that history was rebuilt, what moved on disk, and what is still
/// unmet.
///
/// Leads with the fact that the earlier turns are gone. That sentence is the whole point
/// of preferring this to the trim -- the trim drops the same turns and says nothing --
/// and it is written to forestall the reading that this is a summary of them.
///
/// `Undecidable` findings are omitted, matching `Verdict::first_unmet()`: telling a model
/// that one side could not be read spends context on a sentence it cannot act on.
[[nodiscard]] std::string reconstruction_note(const Changeset& changes,
                                              const Verdict& verdict);

/// `task`, verbatim, followed by `reconstruction_note`.
///
/// ⚠ `task` must be the **original** instruction. Passing a previously composed one nests
/// the framing; see the header.
[[nodiscard]] std::string reconstructed_instruction(std::string_view task,
                                                    const Changeset& changes,
                                                    const Verdict& verdict);

}  // namespace hermit::supervisor
