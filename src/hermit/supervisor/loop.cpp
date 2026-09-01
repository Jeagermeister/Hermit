#include <hermit/supervisor/loop.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <utility>

#include <hermit/supervisor/wire.h>

namespace hermit::supervisor {
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
    case StopReason::Misconfigured:  return "the run was configured to judge with nothing to judge against";
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
    return {.content = render_error(unknown_tool(call.name)), .refused = true, .paths = {}};
  }

  // The model's JSON becomes RawArgs without consulting the declaration -- shape only.
  const auto raw = raw_args_from(call.arguments);
  if (!raw) {
    return {.content = render_error(call.name + ": " + raw.error().message()),
            .refused = true,
            .paths = {}};
  }

  // ...and the declaration is what checks names, presence and containment. This is the
  // one place a Path argument becomes a SandboxPath, so a call cannot reach a tool with
  // a path outside the root (R1) even if the model asked for one.
  auto args = parse_args(tool->spec(), *raw, sandbox);
  if (!args) {
    return {.content = render_error(call.name + ": " + to_string(args.error())),
            .refused = true,
            .paths = {}};
  }

  // Collected before invoke, so a tool that fails still reports what it was pointed at --
  // "you already tried to read this and it did not work" is exactly as useful to a model
  // as a success, and more useful than silence.
  std::vector<std::string> touched;
  for (const ArgSpec& arg : tool->spec().args) {
    if (arg.type == ArgType::Path) {
      if (const SandboxPath* one = args->path(arg.name)) {
        touched.push_back(one->relative().string());
      }
    } else if (arg.type == ArgType::PathList) {
      for (const SandboxPath& each : args->paths(arg.name)) {
        touched.push_back(each.relative().string());
      }
    }
  }

  auto output = tool->invoke(*args);
  if (!output) {
    return {.content = render_error(call.name + ": " + output.error().reason),
            .refused = true,
            .paths = std::move(touched)};
  }

  return {.content = render_output(*output), .refused = false, .paths = std::move(touched)};
}

LoopOutcome AgentLoop::run(Session& session, std::string instruction) {
  const auto started = Clock::now();
  LoopOutcome outcome;

  // Every stated expectation as a question that was not answered, for the stops where
  // there is no snapshot to answer it from.
  //
  // A default-constructed Verdict will not do, and the reason is a trap rather than a
  // detail: it has zero findings, and `met()` is true of zero findings. A run that stopped
  // because it could not read the filesystem would then carry a verdict saying everything
  // passed. `Undecidable` is the state that exists for exactly this -- `met()` goes false
  // because the judge never agreed, and `first_unmet()` skips it, so R7 is never handed
  // "we could not look" as a concrete failure to go and fix.
  const auto unanswered = [&](std::string_view why) {
    Verdict verdict;
    verdict.unjudged = options_.unjudged_requirements;
    for (const auto& expectation : options_.expected) {
      verdict.findings.push_back(Finding{.expectation = expectation,
                                         .outcome = Outcome::Undecidable,
                                         .reason = std::string{why}});
    }
    return verdict;
  };

  // Refused before anything is sent, because the alternative is a run that looks judged
  // and is not. There is no snapshot without a verifier and therefore nothing to judge
  // against, so this is the caller contradicting itself rather than a condition to
  // recover from.
  if (!options_.expected.empty() && options_.verifier == nullptr) {
    outcome.reason = StopReason::Misconfigured;
    outcome.detail = std::to_string(options_.expected.size()) +
                     " expectations were stated but no verifier was supplied";
    outcome.verdict = unanswered("no verifier was supplied to judge against");
    outcome.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    return outcome;
  }

  // The companion refusal. A caller-held baseline exists to be judged against -- attempt
  // two of an R7 job judged against the tree attempt one started from -- and there is no
  // judging without a verifier. Ignoring it would silently re-baseline the caller's job,
  // which is the same shape of failure as silently not judging.
  if (options_.judge_baseline != nullptr && options_.verifier == nullptr) {
    outcome.reason = StopReason::Misconfigured;
    outcome.detail = "a judge baseline was supplied but no verifier";
    outcome.verdict = unanswered("no verifier was supplied to judge against");
    outcome.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    return outcome;
  }

  // The baseline, taken before the model is told anything. Everything the run is later
  // judged to have done is measured against this, so it must precede the first request.
  TreeSnapshot baseline;
  TreeSnapshot previous;
  if (options_.verifier != nullptr) {
    // A caller-held job baseline seeds the walk so unchanged files carry their hashes
    // forward instead of being re-read; it does not replace the opening snapshot, for the
    // attribution reason `LoopOptions::judge_baseline` states.
    auto taken = options_.verifier->snapshot(options_.judge_baseline);
    if (!taken) {
      outcome.reason = StopReason::VerificationFailed;
      outcome.detail = taken.error().message();
      outcome.verdict = unanswered(outcome.detail);
      outcome.elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
      return outcome;
    }
    baseline = std::move(*taken);
    previous = baseline;
  }

  // The only place `judge` is called. A second call site is how `unjudged_requirements`
  // would end up on the outcome and missing from the turns, and a verdict that quietly
  // drops it reports "all met" for a task with a requirement nobody examined.
  const auto judged = [&](const TreeSnapshot& current) {
    // The caller's job baseline when one was supplied, this run's own otherwise.
    const TreeSnapshot& against =
        options_.judge_baseline != nullptr ? *options_.judge_baseline : baseline;
    Verdict verdict = judge(against, current, options_.expected);
    verdict.unjudged = options_.unjudged_requirements;
    return verdict;
  };

  const auto finish = [&](StopReason reason, std::string detail) {
    outcome.reason = reason;
    outcome.detail = std::move(detail);
    outcome.dropped = session.dropped();
    outcome.compactions = session.compactions();
    // The net effect of the run: last against first, so a file created and deleted again
    // does not appear and one written three times appears once. Taken on every exit path,
    // including the bounds -- a run that ran out of turns still changed whatever it
    // changed, and that is exactly what a caller deciding whether to re-invoke needs.
    if (options_.verifier != nullptr) outcome.net_changes = diff(baseline, previous);
    // Judged on every exit path, bounds included: a run that ran out of turns still
    // either met the stated expectations or did not, and which one it was is exactly what
    // a caller deciding whether to re-invoke needs. Computed even with no verifier, where
    // it is an empty set of findings carrying `unjudged` -- the honest reading of "nobody
    // asked, and here is what could not be asked".
    //
    // Except when the tree could not be read. `previous` is then the last snapshot that
    // succeeded, and judging against it would report a verdict about a tree that is no
    // longer the one on disk -- with a hash behind it, so it would read as verified.
    outcome.verdict = reason == StopReason::VerificationFailed ? unanswered(outcome.detail)
                                                               : judged(previous);
    outcome.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    return outcome;
  };

  // Copied before `add_user` takes it. Every reconstruction is composed from this, never
  // from an already-composed instruction, so a third compaction does not nest three
  // framings inside each other -- the discipline reinvocation_instruction states for the
  // third retry, reached here by a different route.
  const std::string task = instruction;
  session.add_user(std::move(instruction));

  // Every path the run has named in a call, first-touch order, no repeats. A vector with a
  // linear scan rather than a set, because the order is the useful part -- it reads as the
  // trail the model actually walked.
  //
  // The scan is not bounded by the turn and call caps, which an earlier version of this
  // comment claimed: `read`, `grep` and `hash` all take a `PathList` and `parse_args` puts
  // no cap on its length, so one call can contribute any number of entries. Resolving those
  // paths costs a component-by-component walk each and dominates this scan by a wide
  // margin, so the shape is still right -- but the bound is the model's appetite, not a
  // constant in this file.
  std::vector<std::string> observed;
  const auto observe = [&observed](const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
      if (std::find(observed.begin(), observed.end(), path) == observed.end()) {
        observed.push_back(path);
      }
    }
  };

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

    // Rebuild the window before asking prepare() to trim it. `previous` is the snapshot
    // taken after the last turn's calls, so the changeset and the verdict describe the
    // tree as it stands right now; with no verifier there is neither, and the trim below
    // stays the policy. reconstruct() declines when there is nothing to fold or the
    // rebuild would not be smaller, and declining is not an error -- the trim handles
    // both cases and the counters show which path a run took.
    if (options_.verifier != nullptr && should_compact(session, options_.compact_at)) {
      // The record is the one part of a rebuilt window that is remembered rather than
      // re-observed, which is why it is switchable independently of the rebuild itself.
      const std::span<const std::string> carried =
          options_.carry_observed_paths ? std::span<const std::string>{observed}
                                        : std::span<const std::string>{};
      static_cast<void>(session.reconstruct(reconstructed_instruction(
          task, diff(baseline, previous), judged(previous), carried)));
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

    // Built before the truncation check, not after it. Everything here comes from the
    // reply and the session, both of which are already final; and the refusal path below
    // needs somewhere to record the calls it answers, which is exactly why that path used
    // to report nothing at all.
    TurnEvent event;
    event.turn = outcome.turns;
    event.prompt_tokens = reply->prompt_tokens;
    event.generated_tokens = reply->completion_tokens;
    event.truncated = reply->truncated();
    event.reasoning_chars = reply->reasoning.size();
    event.content = reply->content;
    event.dropped = session.dropped();
    event.compactions = session.compactions();

    if (!recorded) {
      // record() appends the assistant turn -- tool calls included -- and *then* reports
      // failure on the truncation path. Returning here would leave those calls in history
      // with no results: the orphaned-call shape the grouped trim exists to prevent,
      // arriving by a route the trim never sees. Session reuse across instructions is
      // explicitly anticipated above, and a model shown its own unanswered call re-issues
      // it. So answer them before stopping.
      //
      // Counted and reported like any other refusal, which they were not until this was
      // written. These results reach the model, so leaving them out of `calls` and
      // `refusals` under-reports the run, and building no CallEvent for them loses the
      // whole turn from the trace -- a strictly larger version of the disagreement this
      // commit set out to fix, in the same function.
      for (const auto& call : reply->tool_calls) {
        std::string refusal =
            render_error(call.name + ": the run stopped before this call ran: " +
                         recorded.error().message());
        ++outcome.calls;
        ++outcome.refusals;
        event.calls.push_back(
            CallEvent{.tool = call.name, .refused = true, .result = refusal});
        session.add_tool_result(call.name, std::move(refusal));
      }
      // The turn happened and its calls were answered, so an observer that is building a
      // trace has to see it. Without this the run's last turn is simply missing.
      if (options_.observer) options_.observer(event);
      return finish(StopReason::SessionRefused, recorded.error().message());
    }

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
      // Against the baseline, not against the previous turn. `Preserved` asks what the
      // bytes were before the run, and a turn-to-turn comparison cannot answer that --
      // by the second turn the original is already gone from the near side.
      event.verdict = judged(previous);
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
      outcome.final_reasoning = reply->reasoning;
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
        // The flag moves with the content. Counting this in `refusals` while leaving the
        // event saying otherwise made the summary and the trace disagree about the same
        // call -- measured on a live run, where four lines printed `ok` above a footer
        // reading `10 calls (4 refused)`, and the four were these.
        dispatched.refused = true;
      }

      observe(dispatched.paths);
      event.calls.push_back(CallEvent{.tool = call.name,
                                      .refused = dispatched.refused,
                                      .result = dispatched.content});
      session.add_tool_result(call.name, std::move(dispatched.content));
    }

    if (auto failed = verify()) return finish(StopReason::VerificationFailed, *failed);
    emit();
  }
}

}  // namespace hermit::supervisor
