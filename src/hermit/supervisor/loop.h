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
// --- A per-model hazard the loop cannot fix -----------------------------------
//
// **On the two stock Meta llama3.x instruct templates the tool definitions are not
// rendered when the last message is a tool result** -- precisely the turn on which the
// model has to decide whether to call another tool. Two of seven models measured; it is a
// property of the template, not the architecture, so hermes3-8b is llama3.1 underneath and
// unaffected. Measured table, mechanism and the three candidate responses: DECISIONS.md
// under "Still open" -- one copy, since model selection is where the decision belongs.
//
// What it costs is narrower than the first run suggested: tool calling does not collapse,
// the model works from memory of a schema it can no longer see -- measured, `llama3.2-3b`
// called `write` with `paths` (borrowed from `read`) where the declaration says `path`, and
// once degraded all the way to prose. A loop cannot detect either from a reply, and cannot
// fix it without fabricating a user turn (see below), so it is a model-selection constraint
// the loop merely suffers.
//
// --- And one that silences the system prompt entirely --------------------------
//
// `hermes3-8b` writes its own function-calling preamble *instead of* the caller's system
// prompt whenever tools are offered -- demonstrated, recorded in DECISIONS.md under "Still
// open" -- so `app`'s system prompt (which exists to counter two measured failure modes) is
// never seen by that model when tools are on, and no observed behaviour may be credited to it.
//
// **Not worked around here, deliberately.** The obvious mitigation -- append a synthetic user
// turn after the results, which restores the missing definitions above -- would put words in
// the user's mouth in history, and `pin_latest_user` would then pin that synthetic turn
// instead of the real instruction: the one message the trim must never drop becomes a
// fabricated nudge. Worse than the failure it fixes; see DECISIONS.md for the undecided
// alternatives.
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

#include <hermit/core/sandbox.h>
#include <hermit/core/tool.h>
#include <hermit/ollama/client.h>
#include <hermit/supervisor/judge.h>
#include <hermit/supervisor/session.h>
#include <hermit/supervisor/verify.h>

namespace hermit::supervisor {

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

  /// A tree snapshot could not be taken, so the run cannot be verified.
  ///
  /// Fails closed rather than continuing unverified. A caller that asked for verification
  /// and silently got none would be in the worst position of all -- believing the work was
  /// checked. The usual cause is an unreadable directory, which hides a subtree and would
  /// otherwise report every file under it as deleted.
  VerificationFailed,

  /// Expectations were stated with no verifier to judge them against, so the run never
  /// started.
  ///
  /// Kept separate from `VerificationFailed` rather than folded into it, because the two
  /// send an operator to different places: that one means the filesystem could not be
  /// read, this one means the caller asked a question and supplied no way to answer it.
  /// Reported as its own stop rather than ignored, for the reason `LoopOptions::expected`
  /// gives -- silently not judging is indistinguishable from judging and finding nothing
  /// wrong.
  Misconfigured,
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

  /// What this turn did to the tree, hash-verified and owing nothing to the model's
  /// account of it (R6). Always empty when no verifier was supplied.
  ///
  /// Per turn rather than per call on purpose: a turn is the unit at which
  /// ROUTING.md section 6 places this, and attributing a change to one call among several
  /// would need a snapshot between each, which costs a walk per call to answer a question
  /// nothing asks.
  Changeset changes{};

  /// The stated expectations as they stood at the end of this turn. No findings when none
  /// were stated, or when no verifier was supplied.
  ///
  /// Judged every turn rather than only at the end, and deliberately never used to stop
  /// the run. `Exists("report.md")` goes `Met` the instant an empty file appears, so a
  /// loop that stopped on a met verdict would cut the model off before it wrote the
  /// content -- passing the expectation and failing the task. The model decides when it
  /// is finished; this decides whether that was true.
  Verdict verdict{};
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

  /// Verify the tree after every turn (R6). Optional; null means no verification, which
  /// is the right default for a caller that only wants the loop.
  ///
  /// A pointer rather than a value because a verifier is a collaborator with a lifetime,
  /// like the registry and the sandbox, and because "absent" has to be representable. It
  /// must outlive the loop.
  ///
  /// Supplying one changes failure behaviour, deliberately: a snapshot that cannot be
  /// taken stops the run (`VerificationFailed`) rather than degrading it to an unverified
  /// one that looks identical from outside.
  TreeVerifier* verifier = nullptr;

  /// The post-conditions this run is judged against ([judge.h](./judge.h)). Empty is the
  /// default and means report-only: the changeset still says what moved, and nothing
  /// claims whether it was the right thing.
  ///
  /// Declaration order is carried through to the verdict, because R7 restates the *first*
  /// unmet finding -- so a set written in dependency order yields the next actionable step
  /// rather than an arbitrary one out of a set.
  ///
  /// **Requires `verifier`.** Judging reads two snapshots and there are none without one,
  /// so a non-empty set with no verifier is a configuration error the loop refuses at the
  /// start rather than a request it can half-honour -- silently not judging would look
  /// exactly like judging and finding nothing wrong.
  ExpectationSet expected{};

  /// Requirements the caller knew about and could not state as expectations, carried so
  /// that "all expectations met" cannot be read as "the work is done". Reaches the verdict
  /// unchanged; `Verdict::unjudged` says why it has to exist at all.
  std::size_t unjudged_requirements = 0;

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

  /// What the whole run did to the tree: the last snapshot against the first, so a file
  /// created and then deleted again does not appear, and one written three times appears
  /// once. Empty when no verifier was supplied, and *legitimately* empty when a run
  /// changed nothing -- which is itself the finding R6 is about, since a model that
  /// reports success over an untouched tree is the measured failure (`llama32-3b` replied
  /// `DONE` on an untouched tree in 18 of 27 failed runs).
  ///
  /// This is evidence, not a verdict. Whether the changes are the *right* ones needs a
  /// post-condition the supervisor has not been given; see verify.h's closing note.
  Changeset net_changes{};

  /// The stated expectations against the whole run -- the last snapshot judged against the
  /// first, the same pair `net_changes` is derived from.
  ///
  /// With no expectations stated there are no findings, and `met()` is then trivially true
  /// because nothing was asked. That is exactly why this class does not turn the verdict
  /// into a success flag: "nothing was asked" and "everything asked for holds" are the
  /// same value here and must not be the same *report*. The frontend has the context to
  /// tell them apart, so the frontend decides what an exit code says.
  Verdict verdict{};

  /// The model's last prose reply. Empty for every reason except `Answered`, and
  /// legitimately empty even then if the model answered with tool calls to the end.
  std::string final_content;

  /// Why it stopped, for anything other than a clean `Answered`. One line, already
  /// carrying whatever the Session or the transport said.
  std::string detail;

  std::chrono::milliseconds elapsed{0};

  /// Whether the run ended by the model's own choice rather than by hitting a bound.
  /// Says nothing about whether the work is correct -- see StopReason::Answered, and
  /// `verdict`, which does say so for the part that was actually stated.
  [[nodiscard]] bool ran_to_completion() const noexcept {
    return reason == StopReason::Answered;
  }
};

/// How the loop reaches a model: one request in, one reply or failure out.
///
/// A callable rather than the `Client` itself, and the reason is this project's own stated
/// value -- policy must be testable without a daemon, which is why `Session` takes a
/// ChatReply and why the client's parsers are exposed. `run()` calls exactly one method on
/// a Client, so everything interesting about a *multi-turn* run -- a stalled reply, a
/// discarded prompt, the per-turn call cap, the oversized-result substitution, the observer,
/// any sequence longer than one turn -- was reachable only through a live model until this
/// existed. That is a large amount of policy verified by hand and by nothing else.
///
/// Costs nothing against the layering: D7 keeps HTTP behind the pimpl in `client.cpp` and
/// this never touches it, and D1's blocking single thread means a `std::function` call is
/// indistinguishable from the direct one it replaces. Same idiom as `LoopOptions::observer`,
/// for the same reason.
using ChatFn = std::function<ollama::Result<ollama::ChatReply>(const ollama::ChatRequest&)>;

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

  /// Testing seam: the same loop against any reply source.
  ///
  /// Production callers should prefer the overload above -- it binds the loop to the very
  /// Client whose `max_num_ctx` the Session planned against, which is the pairing
  /// `Session::open(options, client, ...)` exists to enforce. This one cannot check that,
  /// because it has not been given a Client; it takes the honest name instead.
  AgentLoop(ChatFn chat, ToolRegistry& registry, const Sandbox& sandbox,
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
  ChatFn chat_;
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

}  // namespace hermit::supervisor
