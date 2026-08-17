#include <hermes/supervisor/loop.h>

#include <cmath>
#include <optional>
#include <utility>

#include <hermes/supervisor/wire.h>

namespace hermes::supervisor {
namespace {

using Clock = std::chrono::steady_clock;

/// The refusal text for a call naming a tool this frontend does not expose.
///
/// ROUTING.md section 8 makes exposure a per-frontend policy, so "unknown" here means
/// "not offered to you", which is what the model needs to hear -- naming the tools it
/// does have would be guidance this layer has no business inventing, and the definitions
/// it was sent already carry that list.
std::string unknown_tool(std::string_view name) {
  std::string reason = "unknown tool '";
  reason += name;
  reason += "': it is not in the tool list you were given";
  return reason;
}

}  // namespace

std::string_view to_string(StopReason r) noexcept {
  switch (r) {
    case StopReason::VerificationFailed: return "the tree could not be verified";
    case StopReason::Answered:       return "the model answered without asking for a tool";
    case StopReason::TurnBudget:     return "the turn budget ran out";
    case StopReason::TimeBudget:     return "the wall-clock budget ran out";
    case StopReason::Stalled:        return "the model returned neither an answer nor a call";
    case StopReason::SessionRefused: return "the session refused the prompt";
    case StopReason::Transport:      return "the request to Ollama failed";
  }
  return "unknown stop reason";
}

bool result_is_hopeless(const Session& session, std::size_t content_chars) {
  // Same arithmetic Session::estimate uses, deliberately: a divergence between the two
  // would mean refusing results that would have fitted, or admitting ones that cannot.
  const double tokens = static_cast<double>(content_chars) / session.chars_per_token();
  const auto estimated = static_cast<std::uint64_t>(std::ceil(tokens)) + kPerMessageOverhead;
  return estimated > session.prompt_budget();
}

std::string oversized_refusal(std::string_view tool, std::size_t content_chars,
                              std::uint64_t budget_tokens) {
  std::string reason{tool};
  reason += " produced ";
  reason += std::to_string(content_chars);
  reason += " characters, which cannot fit a prompt budget of ";
  reason += std::to_string(budget_tokens);
  reason += " tokens; ask for less at a time";
  return reason;
}

AgentLoop::AgentLoop(const ollama::Client& client, ToolRegistry& registry,
                     const Sandbox& sandbox, LoopOptions options)
    // Captures by reference: the Client must outlive the loop, which is the contract stated
    // on the class and the same one Session::open already has.
    : AgentLoop([&client](const ollama::ChatRequest& request) { return client.chat(request); },
                registry, sandbox, std::move(options)) {}

AgentLoop::AgentLoop(ChatFn chat, ToolRegistry& registry, const Sandbox& sandbox,
                     LoopOptions options)
    : chat_(std::move(chat)),
      registry_(&registry),
      sandbox_(&sandbox),
      options_(std::move(options)),
      definitions_(tool_definitions(registry)) {}

Dispatched dispatch_call(ToolRegistry& registry, const Sandbox& sandbox,
                         const ollama::ToolCall& call) {
  Tool* tool = registry.find(call.name);
  if (tool == nullptr) {
    return {.content = render_error(unknown_tool(call.name)), .refused = true};
  }

  // The model's JSON becomes RawArgs without consulting the declaration -- shape only.
  const auto raw = raw_args_from(call.arguments);
  if (!raw) {
    return {.content = render_error(call.name + ": " + raw.error().message()), .refused = true};
  }

  // ...and the declaration is what checks names, presence and containment. This is the
  // one place a Path argument becomes a SandboxPath, so a call cannot reach a tool with
  // a path outside the root (R1) even if the model asked for one.
  auto args = parse_args(tool->spec(), *raw, sandbox);
  if (!args) {
    return {.content = render_error(call.name + ": " + to_string(args.error())),
            .refused = true};
  }

  auto output = tool->invoke(*args);
  if (!output) {
    return {.content = render_error(call.name + ": " + output.error().reason), .refused = true};
  }

  return {.content = render_output(*output), .refused = false};
}

LoopOutcome AgentLoop::run(Session& session, std::string instruction) {
  const auto started = Clock::now();
  LoopOutcome outcome;

  // The baseline, taken before the model is told anything. Everything the run is later
  // judged to have done is measured against this, so it must precede the first request.
  TreeSnapshot baseline;
  TreeSnapshot previous;
  if (options_.verifier != nullptr) {
    auto taken = options_.verifier->snapshot();
    if (!taken) {
      outcome.reason = StopReason::VerificationFailed;
      outcome.detail = taken.error().message();
      outcome.elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
      return outcome;
    }
    baseline = std::move(*taken);
    previous = baseline;
  }

  const auto finish = [&](StopReason reason, std::string detail) {
    outcome.reason = reason;
    outcome.detail = std::move(detail);
    outcome.dropped = session.dropped();
    // The net effect of the run: last against first, so a file created and deleted again
    // does not appear and one written three times appears once. Taken on every exit path,
    // including the bounds -- a run that ran out of turns still changed whatever it
    // changed, and that is exactly what a caller deciding whether to re-invoke needs.
    if (options_.verifier != nullptr) outcome.net_changes = diff(baseline, previous);
    outcome.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    return outcome;
  };

  session.add_user(std::move(instruction));

  while (true) {
    // R8, checked here because it cannot be checked anywhere else: a bound enforced by
    // the thread doing the work cannot fire mid-request. Checked *before* the turn so a
    // budget already spent does not buy one more generation.
    if (Clock::now() - started >= options_.budget) {
      return finish(StopReason::TimeBudget,
                    "spent the " + std::to_string(options_.budget.count()) +
                        "s budget after " + std::to_string(outcome.turns) + " turns");
    }
    if (outcome.turns >= options_.max_turns) {
      return finish(StopReason::TurnBudget,
                    "reached the " + std::to_string(options_.max_turns) + "-turn bound");
    }

    auto request = session.prepare();
    if (!request) return finish(StopReason::SessionRefused, request.error().message());

    // D12: the tool surface, never a `format` schema alongside it.
    request->constraint = ollama::ChatRequest::Tools{.definitions = definitions_};

    const auto reply = chat_(*request);
    if (!reply) return finish(StopReason::Transport, reply.error().message());

    // record() appends the assistant turn -- tool calls included, so the results
    // appended below are never orphaned -- and verifies the prompt was not discarded.
    const auto recorded = session.record(*reply);
    ++outcome.turns;
    if (!recorded) {
      // record() appends the assistant turn -- tool calls included -- and *then* reports
      // failure on the truncation path. Returning here would leave those calls in history
      // with no results: the orphaned-call shape the grouped trim exists to prevent,
      // arriving by a route the trim never sees. Session reuse across instructions is
      // explicitly anticipated above, and a model shown its own unanswered call re-issues
      // it. So answer them before stopping.
      for (const auto& call : reply->tool_calls) {
        session.add_tool_result(
            call.name, render_error(call.name + ": the run stopped before this call ran: " +
                                    recorded.error().message()));
      }
      return finish(StopReason::SessionRefused, recorded.error().message());
    }

    TurnEvent event;
    event.turn = outcome.turns;
    event.prompt_tokens = reply->prompt_tokens;
    event.generated_tokens = reply->completion_tokens;
    event.truncated = reply->truncated();
    event.reasoning_chars = reply->reasoning.size();
    event.content = reply->content;
    event.dropped = session.dropped();

    // R6, and the ordering is the point: the tree is read *after* the turn's calls have
    // run, and nothing about it comes from the reply. A model that answered in prose still
    // gets its turn verified, because shell and tools we did not write are exactly what
    // ROUTING.md section 6 says this layer exists to cover.
    const auto verify = [&]() -> std::optional<std::string> {
      if (options_.verifier == nullptr) return std::nullopt;
      auto taken = options_.verifier->snapshot(&previous);
      if (!taken) return taken.error().message();
      event.changes = diff(previous, *taken);
      previous = std::move(*taken);
      return std::nullopt;
    };

    const auto emit = [&] {
      if (options_.observer) options_.observer(event);
    };

    if (reply->tool_calls.empty()) {
      if (auto failed = verify()) return finish(StopReason::VerificationFailed, *failed);
      emit();
      // Either an answer, or nothing at all. `generated()` is the broader question,
      // because a thinking model that spent its whole budget reasoning returns empty
      // content with no error -- a re-run at temperature 0 would do it again.
      if (!reply->generated()) {
        return finish(StopReason::Stalled,
                      reply->truncated()
                          ? "the reply hit the generation budget with nothing to show for it"
                          : "the reply was empty");
      }
      outcome.final_content = reply->content;
      return finish(StopReason::Answered, {});
    }

    // One result per call, in the order asked. Repeats included: the model asked twice
    // and is waiting on two answers, and llama3.2-3b does exactly that.
    for (std::size_t i = 0; i < reply->tool_calls.size(); ++i) {
      const auto& call = reply->tool_calls[i];
      ++outcome.calls;

      if (i >= options_.max_calls_per_turn) {
        // Refused rather than dropped: a call with no answer reads as outstanding, and
        // the reliable response to that is to re-issue it.
        ++outcome.refusals;
        std::string refusal = render_error("refused: this turn already made " +
                                           std::to_string(options_.max_calls_per_turn) +
                                           " calls, which is the per-turn limit");
        event.calls.push_back(
            CallEvent{.tool = call.name, .refused = true, .result = refusal});
        session.add_tool_result(call.name, std::move(refusal));
        continue;
      }

      auto dispatched = dispatch_call(*registry_, *sandbox_, call);
      if (dispatched.refused) ++outcome.refusals;

      // A result nothing can be trimmed around never enters history; the size goes back
      // instead, which the model can act on. See result_is_hopeless.
      if (!dispatched.refused && result_is_hopeless(session, dispatched.content.size())) {
        ++outcome.refusals;
        dispatched.content =
            render_error(oversized_refusal(call.name, dispatched.content.size(),
                                           session.prompt_budget()));
      }

      event.calls.push_back(CallEvent{.tool = call.name,
                                      .refused = dispatched.refused,
                                      .result = dispatched.content});
      session.add_tool_result(call.name, std::move(dispatched.content));
    }

    if (auto failed = verify()) return finish(StopReason::VerificationFailed, *failed);
    emit();
  }
}

}  // namespace hermes::supervisor
