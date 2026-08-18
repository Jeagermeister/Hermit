// R7's second half: re-invoke with one concrete remaining failure.
//
// The tournament recommendation this project is built on reads "use an external
// supervisor that checks repository state and reinvokes". judge.cpp produces the failure
// -- `Verdict::first_unmet()`, one sentence, first in declaration order -- and this file
// drives it back in. The arithmetic is the product: a task succeeding ~67% per attempt,
// retried with a check in between, approaches ~96% by the third try. If these models were
// consistent a retry driver would be pointless; bench/fsops measured that they are not
// (39% of model x task pairs were unstable across three identical runs).
//
// Three policies live here rather than in the loop, stated because each is a decision and
// not a default:
//
//   - **Every attempt is a fresh session.** "Break larger work into fresh sessions" is
//     the tournament's own conclusion, and the nearest measurement is in ROADMAP.md
//     (2026-08-17): handed a mid-session refusal, three of four models stopped calling
//     tools and addressed the human instead -- a probe of reaction to a confusing error,
//     stated at that width, but the direction is the point: these models do not reliably
//     self-correct in place, so the correction has to arrive fresh. The retry prompt is
//     the original task plus the one failure -- never the wreckage of the previous try.
//
//   - **One baseline for the whole job.** Attempt two is judged against the tree as it
//     stood before attempt one, not against its own opening snapshot. `Preserved` reads
//     its source bytes from the baseline, so re-baselining between attempts would let a
//     first attempt that wrongly moved the source change what the operator's expectation
//     *means* mid-job. The verdict always answers the question that was asked, once, at
//     the start. `LoopOptions::judge_baseline` is the seam this rides on.
//
//   - **Infrastructure is never retried.** Transport, SessionRefused, VerificationFailed
//     and Misconfigured stop the job: R7 exists for model inconsistency, and a model run
//     spent against a dead daemon or an unreadable tree retries a problem that is not the
//     model's. Undecidable-only verdicts stop for the same reason -- "one side could not
//     be read" is never sent to the model (judge.h), so there is nothing to re-invoke
//     with. A run cut off by a bound *is* retried when its verdict has an unmet finding:
//     the judge's evidence is equally valid either way, and a cutoff with work remaining
//     is exactly what a fresh bounded session is for.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <hermit/supervisor/loop.h>
#include <hermit/supervisor/session.h>

namespace hermit::supervisor {

/// The instruction a retry attempt is given: the task, then the one concrete failure.
///
/// The task travels too, because the failure alone is not actionable -- "falcon-index.md
/// does not exist" does not say what the file should contain; the task does. Always
/// composed from the *original* task, never from a previous attempt's composed prompt,
/// so the framing does not nest on the third try.
[[nodiscard]] std::string reinvocation_instruction(std::string_view task,
                                                   const Finding& unmet);

/// How a fresh session is made, once per attempt. A callable for the same reason the
/// loop's ChatFn is one: policy must be testable without a daemon.
using SessionFactory = std::function<SessionResult<Session>()>;

struct ReinvokeOptions {
  /// Total attempts, the first run included. 1 turns R7 off. The default is the
  /// arithmetic the product is named for: three attempts with a check in between.
  /// 0 is read as 1 -- "never run" is not a request anyone makes on purpose.
  std::size_t attempts = 3;

  /// Called before each attempt begins. `retrying` is the unmet finding this attempt was
  /// re-invoked to fix, null on the first attempt; it points at driver-held state and
  /// must not be kept past the call. Empty by default; must not throw (the same contract
  /// as `LoopOptions::observer`).
  std::function<void(std::size_t attempt, std::size_t of, const Finding* retrying)>
      on_attempt{};
};

/// One attempt: exactly what the model was asked, and what came of it.
struct Attempt {
  std::string instruction;
  LoopOutcome outcome;
};

struct ReinvokeOutcome {
  /// In order. Never empty unless `error` is set.
  std::vector<Attempt> attempts;

  /// Set when the job could not run at all: the baseline could not be taken, or a fresh
  /// session could not be opened. Attempts already made are kept. Distinct from a failed
  /// *run* -- those are in `attempts` with their own stop reasons -- because a frontend
  /// sends the two to different places: this is the operator's problem, never the
  /// model's.
  std::string error;

  /// The attempt whose verdict and stop reason speak for the job: the last one made.
  /// Precondition: `attempts` is non-empty.
  [[nodiscard]] const LoopOutcome& last() const noexcept { return attempts.back().outcome; }
};

/// Run `task` up to `options.attempts` times, each in a fresh session, judging every
/// attempt against one job baseline, until nothing stated remains unmet.
///
/// Stops early the moment an attempt's verdict has no unmet finding -- met,
/// undecidable-only, and nothing-stated all qualify, for the header's reasons -- or the
/// stop reason is one no retry can answer. With no expectations stated this is exactly
/// one `AgentLoop::run`, because there is no verdict to retry against.
[[nodiscard]] ReinvokeOutcome reinvoke(const ollama::Client& client, ToolRegistry& registry,
                                       const Sandbox& sandbox, LoopOptions loop_options,
                                       const ReinvokeOptions& options,
                                       const SessionFactory& make_session,
                                       const std::string& task);

/// Testing seam: the same driver against any reply source, mirroring `AgentLoop`'s pair
/// for the same reason -- the production overload binds the loop to the very Client the
/// sessions were planned against, and this one cannot check that.
[[nodiscard]] ReinvokeOutcome reinvoke(ChatFn chat, ToolRegistry& registry,
                                       const Sandbox& sandbox, LoopOptions loop_options,
                                       const ReinvokeOptions& options,
                                       const SessionFactory& make_session,
                                       const std::string& task);

}  // namespace hermit::supervisor
