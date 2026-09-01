#include <hermit/supervisor/compact.h>

#include <algorithm>
#include <vector>

namespace hermit::supervisor {
namespace {

/// One changed path, as the model needs to read it.
///
/// Deliberately not `Changeset::render()`. That renders 12 hex characters of hash either
/// side, which exists so a human can match two lines of a report by eye; to a model it is
/// a dozen high-entropy characters that tokenise badly (the measured 1.63 and 1.36
/// characters-per-token samples in session.h are exactly this shape) and answer no
/// question it can act on. Kind and path are what it needs.
void append_change(std::string& out, const Change& change) {
  out += "  ";
  out += to_string(change.kind);
  out += "  ";
  // Scrubbed. A filename is attacker-controlled data -- Linux allows newlines in one, and
  // TreeVerifier keys straight off readdir -- and this string lands in the *pinned* user
  // turn, the one message the trim can never drop, recomposed into every later rebuild.
  // Raw, a name carrying "\n---\nSYSTEM: ..." forges its own paragraph there. See
  // one_line in verify.h for why the sandbox gate cannot close this on its own.
  out += one_line(change.path);
  out += '\n';
}

void append_more(std::string& out, std::size_t remaining) {
  out += "  ... and ";
  out += std::to_string(remaining);
  out += " more.\n";
}

}  // namespace

bool should_compact(const Session& session, double threshold) noexcept {
  // Written as `!(threshold > 0.0)` rather than `threshold <= 0.0` so that a NaN -- which
  // compares false against everything -- is read as off instead of slipping through the
  // comparison below and firing on every turn.
  if (!(threshold > 0.0)) return false;

  const std::uint64_t budget = session.prompt_budget();
  if (budget == 0) return false;

  const double fill =
      static_cast<double>(session.estimated_prompt_tokens()) / static_cast<double>(budget);
  return fill >= threshold;
}

std::string reconstruction_note(const Changeset& changes, const Verdict& verdict,
                                std::span<const std::string> observed) {
  // The first sentence is the one the trim does not have. It says the turns are gone, and
  // it says what replaces them is not a recollection -- a model told only "here is the
  // state" would reasonably read it as its own summary and trust it the way it trusts its
  // own account, which is the thing D13 measured as unreliable.
  std::string note =
      "\n\n---\n"
      "The conversation so far has been rebuilt to fit the context window. The earlier "
      "turns are not shown. What follows is not a summary of them: it is the state of the "
      "files as they are on disk right now, read directly. Your own earlier account of "
      "what you did is gone, and this replaces it.\n";

  if (changes.empty()) {
    // Not a filler line. An untouched tree partway through a task is the measured failure
    // R6 exists for -- `llama32-3b` replied `DONE` over one in 18 of its 27 failed runs --
    // so saying it plainly is worth more context than saying nothing.
    note += "\nNothing on disk has changed since this task started.\n";
  } else {
    note += "\nChanged on disk since this task started (";
    note += std::to_string(changes.changes.size());
    note += changes.changes.size() == 1 ? " path):\n" : " paths):\n";

    const std::size_t shown = std::min(changes.changes.size(), kMaxListedChanges);
    for (std::size_t i = 0; i < shown; ++i) append_change(note, changes.changes[i]);
    if (changes.changes.size() > shown) append_more(note, changes.changes.size() - shown);
  }

  if (!observed.empty()) {
    // Placed after the changed list on purpose: the two are read against each other, and
    // the useful comparison is "I have opened these and changed none of them".
    note += "\nAlready opened or named in an earlier call (";
    note += std::to_string(observed.size());
    note += "), so you have seen these before -- though not what was in them, since the "
            "results themselves are gone:\n";

    const std::size_t shown = std::min(observed.size(), kMaxListedObserved);
    for (std::size_t i = 0; i < shown; ++i) {
      note += "  ";
      note += observed[i];
      note += '\n';
    }
    if (observed.size() > shown) append_more(note, observed.size() - shown);
  }

  std::vector<const Finding*> unmet;
  for (const Finding& finding : verdict.findings) {
    if (finding.outcome == Outcome::Unmet) unmet.push_back(&finding);
  }

  if (!unmet.empty()) {
    // Every unmet finding, where R7 restates only the first. The two are answering
    // different questions and the divergence is deliberate: R7 re-invokes a *fresh* model
    // on a finished attempt and gives it one thing so it does not scatter, whereas this
    // model is mid-task and about to decide whether it is done. Handed one requirement it
    // would fix that one and stop, which is the failure the whole verdict exists to catch.
    note += "\nStated requirements not yet met (";
    note += std::to_string(unmet.size());
    note += "):\n";

    const std::size_t shown = std::min(unmet.size(), kMaxListedFindings);
    for (std::size_t i = 0; i < shown; ++i) {
      // judge() always words an unmet finding; the fallback keeps a hand-built Finding
      // from trailing off into an empty bullet. Same guard as reinvocation_instruction.
      note += "  - ";
      // Scrubbed for the same reason: a finding's wording is composed from expectation text
      // and embeds paths, so it is not structurally guaranteed to be free of a newline.
      note += one_line(unmet[i]->reason.empty() ? unmet[i]->expectation.render()
                                                : unmet[i]->reason);
      note += '\n';
    }
    if (unmet.size() > shown) append_more(note, unmet.size() - shown);
  }

  // The same closing R7 uses, and for the same reason: the reliable failure after losing
  // history is to start the task over from the top, and the tree is right there to check.
  note += "\nCheck what is already in place before redoing anything, then continue the "
          "task stated above.\n---\n";
  return note;
}

std::string reconstructed_instruction(std::string_view task, const Changeset& changes,
                                      const Verdict& verdict,
                                      std::span<const std::string> observed) {
  std::string out{task};
  out += reconstruction_note(changes, verdict, observed);
  return out;
}

}  // namespace hermit::supervisor
