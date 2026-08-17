#pragma once

// The agent loop: history, tool dispatch, bounded turns.
//
// Phase 2's last open bullet. Everything it drives already existed and had never met --
// `Session` held history, `ollama::Client` spoke the protocol, the eight Tier 0 tools
// did the filesystem work, and `wire.h` translates between them. This is the turn.
//
//     add the instruction
//     repeat:
//       prepare  ->  chat  ->  record
//       no calls?  stop.
//       for each call, in order:  find -> decode -> parse -> invoke -> render
//       append one result per call
//     until a bound is reached
//
// It opens no socket and touches no file itself: the Client does the first and the tools
// do the second, so what is testable here is the policy -- what bounds a run, what a
// refusal looks like, and what order things happen in.
//
// --- What this deliberately does NOT do: decide completion ------------------
//
// The loop stops when the model replies without asking for a tool. **That is trusting a
// completion claim, which R6 exists to forbid**, and it is worth being blunt about
// rather than letting `StopReason::Answered` read as success.
//
// R6's evidence is that the claim is often false: `llama32-3b` replied `DONE` on an
// untouched tree in 18 of its 27 failed runs. A live example turned up while building
// this, on the very first probe: handed a tool result whose `content` field was the four
// characters `aaaa`, llama3.2-3b reported *"a.txt is 1 character long"* -- reading its
// own tool output wrong, in prose that sounded certain.
//
// So `Answered` means "the model stopped asking for tools", nothing more. Deciding
// whether the *work* is done is Phase 3's job -- R4's snapshot, R3's hash-diff, and
// re-invocation with one concrete remaining failure (R7) -- and it belongs above this
// loop, not inside it. This class is the thing Phase 3 will drive, and it is honest
// about being only half the product.
//
// --- Why refusals go back to the model rather than ending the run -----------
//
// Every refusal on the dispatch path -- unknown tool, undecodable arguments, a path the
// sandbox rejected, a tool's own failure -- is rendered as that call's result and the
// loop continues. ROUTING.md section 3 asks for one loud line the model can act on, and
// R9's fail-closed rule wants the refusal to be data rather than an abort.
//
// --- A per-model hazard the loop cannot fix, measured 2026-08-17 --------------
//
// **On some chat templates the tool definitions are not rendered when the last message
// is a tool result** -- which is precisely the turn on which the model has to decide
// whether to call another tool. Ollama 0.32.9, one `read` tool offered, comparing the
// prompt with and without the `tools` array so the difference is the definitions
// themselves:
//
//   model         tools cost, user last   tools cost, tool result last
//   qwen3.5-9b            +267                    +267    kept
//   qwen3.5-4b            +267                    +267    kept
//   hermes3-8b            +208                    +208    kept
//   granite4-7b           +162                    +162    kept
//   gemma4-e4b             +55                     +55    kept
//   llama3.2-3b           +133                     +31    LOST
//   llama3.1-8b           +146                     +42    LOST
//
// It is a property of the **template, not the architecture**: hermes3-8b is llama3.1
// underneath and keeps its definitions, because Nous ships its own template. So this is
// two of seven models, and the two are the stock Meta llama3.x instruct templates.
//
// The consequence is the failure it produces, which was observed before it was
// explained. On `llama3.2-3b`, turn 1 sent a 832-token prompt and turn 2 sent 160 --
// *smaller*, while the history had grown -- and the model answered turn 2 with a tool
// call written as prose in `content`: `{"name": "read", "parameters": {"paths":
// "['colors.txt', 'count.txt']"}}`. A Python-style list inside a string, which is
// verbatim the failure R2 was written about. The 672-token drop is about eight tool
// definitions, which is exactly what was being offered.
//
// Note what this means for D5, and it is worth stating: `format` could not have fixed
// that call either. It was not a constrained generation gone wrong -- it was prose, on a
// turn where the model had not been told any tools existed.
//
// **Not worked around here, deliberately.** The obvious mitigation -- append a synthetic
// user turn after the results, which restores the definitions (measured: +133 returns) --
// would put words in the user's mouth in the history, and `pin_latest_user` would then
// pin that synthetic turn instead of the real instruction, so the one message the trim
// must never drop would become a fabricated nudge. That is a worse failure than the one
// it fixes. The honest options are to select models by this property or to detect it in
// R9's preflight, and neither is decided; see DECISIONS.md.
//
// Measured, and it is the reason Phase 3 exists rather than being an upgrade: on the
// fsops set, only qwen3.5-9b reliably *used* a refusal to correct itself. Handed an
// error, llama3.2-3b, hermes3-8b and gemma4-e4b all stopped calling tools and addressed
// the human instead -- "Please provide me with the path of the file". (Stated at its
// real width: the refusal injected in that probe was deliberately incoherent, so this
// measures reaction to a confusing error, not to a fair one. The direction is what
// matters -- these models do not reliably self-correct, so the supervisor must
// re-invoke, which is exactly R7.)

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <hermes/core/sandbox.h>
#include <hermes/core/tool.h>
#include <hermes/ollama/client.h>
#include <hermes/supervisor/session.h>

namespace hermes::supervisor {

enum class StopReason {
  /// The model replied without asking for a tool.
  ///
  /// ⚠ Not "the work is done" -- see the header. R6 forbids reading it that way, and
  /// Phase 3's state verification is what replaces it.
  Answered,

  /// `max_turns` reached. The run was cut off, not finished.
  TurnBudget,

  /// The wall-clock budget ran out between turns (R8).
  TimeBudget,

  /// The model returned neither an answer nor a call, so another identical turn would
  /// do the same thing. Usually the thinking budget: a `num_predict` too small for the
  /// reasoning returns empty content with no error at all (see ChatReply).
  Stalled,

  /// The Session refused -- the prompt could not be made to fit, or a reply showed the
  /// server had discarded history despite the estimate saying it would fit.
  SessionRefused,

  /// `Client::chat` failed. Ollama stopped answering; no retry is attempted (D7).
  Transport,
};

std::string_view to_string(StopReason r) noexcept;

/// What one dispatched call did, for an observer. `result` is the JSON that went back
/// to the model, verbatim -- truncated for display by whoever is printing, not here.
struct CallEvent {
  std::string tool;
  bool refused = false;
  std::string result;
};

/// One completed round trip, handed to `LoopOptions::observer` after its calls have run.
///
/// Exists because a run that only reports its total is not debuggable: the first live
/// run of this loop stopped with `Answered` and an empty reply, and nothing in the
/// outcome could distinguish "the model answered in an empty string" from "the model
/// spent its whole budget thinking" -- which ChatReply warns is silent and which
/// `reasoning_chars` and `truncated` together settle.
struct TurnEvent {
  std::size_t turn = 0;

  /// From the reply, so this is what the server actually charged rather than an
  /// estimate -- the number `Session` calibrates against.
  std::uint64_t prompt_tokens = 0;
  std::uint64_t generated_tokens = 0;

  /// Generation stopped at the token budget rather than because the model finished.
  bool truncated = false;

  /// Thinking-model output is measured, not carried: it can be large, it is not part of
  /// the conversation, and a size is enough to tell an empty answer from an exhausted
  /// budget.
  std::size_t reasoning_chars = 0;

  /// The prose half of the reply. Empty is the normal case on a turn that called tools.
  std::string content;

  std::vector<CallEvent> calls;

  /// History given up before this turn was sent, cumulative.
  std::size_t dropped = 0;
};

struct LoopOptions {
  /// Turns, where one turn is one `/api/chat` round trip plus any tool calls it asked
  /// for. A bound on count alone is not enough (R8) but it is the cheaper of the two
  /// and catches a model that calls tools forever without converging.
  std::size_t max_turns = 12;

  /// Wall clock for the whole run (R8), checked between turns.
  ///
  /// ⚠ **It cannot interrupt a request already in flight.** D1 makes this blocking and
  /// single-threaded, and a timeout enforced by the same thread doing the work cannot
  /// fire -- which is upstream's own conclusion, recorded under "A bounded-execution
  /// primitive" in DECISIONS.md. So a single wedged generation is bounded by
  /// `ClientOptions::chat_timeout` (600s by default) and not by this, and the real
  /// worst case for a run is this budget plus one chat timeout. Stated rather than
  /// papered over; a primitive that fixes it properly is an open decision, not a flag.
  std::chrono::seconds budget{300};

  /// Calls executed from one reply. Measured: llama3.2-3b asked for three in a single
  /// turn on a two-file prompt, one of them a repeat, so the real figure is small and
  /// this is a runaway bound rather than a tuning knob.
  ///
  /// Calls past the cap are **refused, not dropped**. Silently discarding a call the
  /// model made would leave it waiting on an answer that never comes, which is the
  /// orphaned-call failure the Session's grouped trim was written to avoid.
  std::size_t max_calls_per_turn = 8;

  /// Called once per completed turn, after its calls have run. Empty by default.
  ///
  /// A plain callback rather than a virtual interface or an event queue: D1 makes this
  /// blocking and single-threaded, so there is no ordering question to solve and
  /// nothing to synchronise. It must not throw -- the loop makes no attempt to
  /// continue if it does.
  ///
  /// Carries an explicit `{}` so a designated initialiser naming only the bounds --
  /// which is every call site that does not want a trace -- neither has to name this
  /// nor warns for omitting it.
  std::function<void(const TurnEvent&)> observer{};
};

/// What one run did, in numbers that can go straight into a report.
struct LoopOutcome {
  StopReason reason = StopReason::Answered;

  /// Round trips completed.
  std::size_t turns = 0;

  /// Calls dispatched, refusals included.
  std::size_t calls = 0;

  /// Calls that came back a refusal rather than rows. Non-zero is not failure by
  /// itself -- a model probing for a file that is not there produces one -- but a run
  /// that is mostly refusals is a run that was arguing with its tools.
  std::size_t refusals = 0;

  /// Turns the Session gave up to make room, over the whole run. Non-zero means the
  /// model was answering with holes in its history, which belongs in any report.
  std::size_t dropped = 0;

  /// The model's last prose reply. Empty for every reason except `Answered`, and
  /// legitimately empty even then if the model answered with tool calls to the end.
  std::string final_content;

  /// Why it stopped, for anything other than a clean `Answered`. One line, already
  /// carrying whatever the Session or the transport said.
  std::string detail;

  std::chrono::milliseconds elapsed{0};

  /// Whether the run ended by the model's own choice rather than by hitting a bound.
  /// Says nothing about whether the work is correct -- see StopReason::Answered.
  [[nodiscard]] bool ran_to_completion() const noexcept {
    return reason == StopReason::Answered;
  }
};

/// One instruction, driven to a bound.
///
/// Holds references, and outlives none of them: the Client, the registry and the
/// Sandbox must all stay alive for the loop's lifetime. That is the same contract
/// `Session::open(options, client, ...)` already has, and under D1 there is no thread
/// for them to move between.
class AgentLoop {
 public:
  /// The tool definitions are rendered once here rather than per turn. Not only to save
  /// the work: an unstable tool order changes the prompt bytes and therefore the
  /// server's prompt-cache hit, which turns a reproducible run into a noisy one.
  ///
  /// The consequence, stated because it is a real constraint and not an implementation
  /// note: **a tool added to the registry after this constructor runs is never offered.**
  /// Composition finishes before a loop is built (`app::ToolSet`), so nothing does that
  /// today; a frontend that wanted to vary its surface mid-run would need a new loop, and
  /// per ROUTING.md section 8 varying it *between* frontends is the supported shape.
  AgentLoop(const ollama::Client& client, ToolRegistry& registry, const Sandbox& sandbox,
            LoopOptions options = {});

  /// Add `instruction` as the user turn and run until a bound is reached.
  ///
  /// `session` is driven, not owned -- the caller keeps it, so its history and token
  /// accounting are readable afterwards, and Phase 3 can run several instructions
  /// through one session or a fresh session per unit of work as it decides.
  [[nodiscard]] LoopOutcome run(Session& session, std::string instruction);

  /// The definitions being offered, in registration order. Exposed so a caller can log
  /// or assert the surface a run was given.
  [[nodiscard]] const std::vector<nlohmann::json>& definitions() const noexcept {
    return definitions_;
  }

 private:
  const ollama::Client* client_;
  ToolRegistry* registry_;
  const Sandbox* sandbox_;
  LoopOptions options_;
  std::vector<nlohmann::json> definitions_;
};

/// What one dispatched call produced: the JSON that goes back as its result, and
/// whether it was a refusal.
struct Dispatched {
  std::string content;
  bool refused = false;
};

/// Run one tool call: find, decode, parse, invoke, render.
///
/// Free rather than a member, and public, because it needs no Client -- it is the half
/// of a turn that is pure over the registry and the sandbox, so the whole refusal
/// vocabulary is reachable from a test without a daemon. That is the same reason
/// `Session` takes a ChatReply and the client's parsers are exposed.
///
/// **Never fails.** Every refusal is a value, because the model is the one that needs to
/// see it: an unknown tool, arguments that would not decode, a path the sandbox rejected,
/// or the tool's own failure all come back as `{"error": ...}` with `refused` set. R9's
/// fail-closed rule wants the refusal to be data rather than an abort, and ROUTING.md
/// section 3 wants it to be one line the model can act on.
[[nodiscard]] Dispatched dispatch_call(ToolRegistry& registry, const Sandbox& sandbox,
                                       const ollama::ToolCall& call);

// --- exposed for testing ----------------------------------------------------

/// Whether one rendered result is too large for `session` to ever plan around.
///
/// The hopeless case only: a result whose own estimated cost exceeds the entire prompt
/// budget cannot be made to fit by dropping anything else, so the trim gives up its
/// whole group -- call included -- and the model, seeing neither, re-issues the same
/// call and gets the same result. That is a run with a bound as its only exit and no
/// progress along the way.
///
/// A result merely *large* is not this: it is trimmed against normally, which is the
/// Session's job and not a refusal.
///
/// The gap it stands in for is worth naming. `read`'s cap is 16 MB, a filesystem-safety
/// limit with no relation to any context window, and nothing in `core` knows what window
/// it is being read into -- ROUTING.md section 9 makes the cap configuration, so wiring
/// the two together is a composition decision that has not been made. Until it is, this
/// converts a silent no-progress loop into one loud line naming the size.
[[nodiscard]] bool result_is_hopeless(const Session& session, std::size_t content_chars);

/// The refusal sent in place of a result too large to plan around.
[[nodiscard]] std::string oversized_refusal(std::string_view tool, std::size_t content_chars,
                                            std::uint64_t budget_tokens);

}  // namespace hermes::supervisor
