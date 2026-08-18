#include <hermit/supervisor/reinvoke.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace hermit::supervisor {

namespace {

/// Stops that are not the model's to answer for. Retrying one spends a model run on a
/// dead daemon, an unreadable tree, or a caller error -- and the measured failure R7
/// exists for is model inconsistency, nothing else.
[[nodiscard]] constexpr bool infrastructure(StopReason reason) noexcept {
  switch (reason) {
    case StopReason::Transport:
    case StopReason::SessionRefused:
    case StopReason::VerificationFailed:
    case StopReason::Misconfigured:
      return true;
    case StopReason::Answered:
    case StopReason::TurnBudget:
    case StopReason::TimeBudget:
    case StopReason::Stalled:
      return false;
  }
  // A StopReason this switch has never heard of fails closed: treated as infrastructure,
  // so a new stop is never silently retried before someone has decided it should be.
  return true;
}

/// The two public overloads differ only in how the AgentLoop is built, and the loop can
/// only be built *after* the job baseline exists -- LoopOptions carries the pointer.
using LoopFactory = std::function<AgentLoop(const LoopOptions&)>;

ReinvokeOutcome drive(const LoopFactory& make_loop, LoopOptions loop_options,
                      const ReinvokeOptions& options, const SessionFactory& make_session,
                      const std::string& task) {
  ReinvokeOutcome result;

  // The job baseline, taken once and judged against by every attempt. Only when judging
  // is actually on: without expectations there is nothing to hold stable, and without a
  // verifier the loop refuses the combination itself. A baseline the caller already
  // holds is respected rather than replaced.
  std::optional<TreeSnapshot> job_baseline;
  if (loop_options.verifier != nullptr && !loop_options.expected.empty() &&
      loop_options.judge_baseline == nullptr) {
    auto taken = loop_options.verifier->snapshot();
    if (!taken) {
      result.error = "the job baseline could not be taken: " + taken.error().message();
      return result;
    }
    job_baseline = std::move(*taken);
    loop_options.judge_baseline = &*job_baseline;
  }

  AgentLoop loop = make_loop(loop_options);

  const std::size_t allowed = std::max<std::size_t>(options.attempts, 1);

  // The finding the *next* attempt is re-invoked to fix. Carried across iterations so
  // the prompt is always composed from the original task plus the latest failure --
  // never from a previous composed prompt, which is what would nest the framing.
  std::optional<Finding> retrying;

  for (std::size_t attempt = 1; attempt <= allowed; ++attempt) {
    Attempt record;
    record.instruction = retrying ? reinvocation_instruction(task, *retrying) : task;

    if (options.on_attempt) {
      options.on_attempt(attempt, allowed, retrying ? &*retrying : nullptr);
    }

    auto session = make_session();
    if (!session) {
      result.error = "a fresh session could not be opened: " + session.error().message();
      return result;
    }

    record.outcome = loop.run(*session, record.instruction);
    result.attempts.push_back(std::move(record));

    const LoopOutcome& done = result.attempts.back().outcome;
    if (infrastructure(done.reason)) break;

    retrying = done.verdict.first_unmet();
    if (!retrying) break;
  }

  return result;
}

}  // namespace

std::string reinvocation_instruction(std::string_view task, const Finding& unmet) {
  // judge() always words an unmet finding; the fallback keeps a hand-built Finding from
  // trailing off into an empty clause.
  const std::string failure =
      unmet.reason.empty() ? unmet.expectation.render() : unmet.reason;

  std::string prompt{task};
  prompt +=
      "\n\nAn earlier attempt at this task already ran. One requirement is still not "
      "met: ";
  prompt += failure;
  prompt +=
      ". Check what is already in place before redoing anything, and finish exactly "
      "that requirement.";
  return prompt;
}

ReinvokeOutcome reinvoke(const ollama::Client& client, ToolRegistry& registry,
                         const Sandbox& sandbox, LoopOptions loop_options,
                         const ReinvokeOptions& options, const SessionFactory& make_session,
                         const std::string& task) {
  return drive(
      [&](const LoopOptions& built) { return AgentLoop{client, registry, sandbox, built}; },
      std::move(loop_options), options, make_session, task);
}

ReinvokeOutcome reinvoke(ChatFn chat, ToolRegistry& registry, const Sandbox& sandbox,
                         LoopOptions loop_options, const ReinvokeOptions& options,
                         const SessionFactory& make_session, const std::string& task) {
  return drive(
      [&](const LoopOptions& built) { return AgentLoop{chat, registry, sandbox, built}; },
      std::move(loop_options), options, make_session, task);
}

}  // namespace hermit::supervisor
