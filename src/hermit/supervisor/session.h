#pragma once

// A bounded conversation: the messages, what they cost, and who decides what to drop.
//
// This is the **supervisor** layer of D7's table -- the layer that drives Ollama and
// owns bounded sessions. It sits above the transport in `hermit::ollama` and below the
// agent loop that Phase 2 adds; it holds history and enforces the context budget, and
// deliberately does no I/O of its own. `prepare()` hands back a ChatRequest and
// `record()` consumes a ChatReply, so every policy decision here is testable without a
// daemon -- the same reason `build_chat_options` and the parse functions are exposed in
// client.h.
//
// --- Why this exists at all: Ollama discards history silently ----------------
//
// Measured on `kitchen-desktop`, Ollama 0.32.9, 2026-08-13. Six messages, five of them
// carrying a distinct codeword, sent whole at three different `num_ctx` values. Columns
// are [system, user1, assistant1, user2(40 KB of filler), assistant2]; the sixth message
// is the question asking which codewords are still visible:
//
//   model              num_ctx   prompt_eval_count   which codewords came back
//   gemma31-agent      32768     9005                sys Y  u1 Y  a1 Y  u2 Y  a2 Y
//   gemma31-agent       2048       64                sys Y  u1 -  a1 -  u2 -  a2 Y
//   gemma31-agent       1024       64                sys Y  u1 -  a1 -  u2 -  a2 Y
//   qwen35-agent       32768     9018                sys Y  u1 Y  a1 Y  u2 Y  a2 Y
//   qwen35-agent        2048       70                sys Y  u1 -  a1 -  u2 -  a2 Y
//
// Three things follow, and the whole design is built on them.
//
//  1. **It is silent.** The response carries no error, no warning, no flag. A prompt of
//     9005 tokens became 64 and the reply looked entirely healthy. This is the exact
//     shape R6 was written about -- the model states its situation confidently and the
//     statement is worthless -- except that here the *client* is the one being misled.
//
//  2. **It keeps the system prompt and a tail, and discards the middle.** `u1` and `a1`
//     are a few tokens each and would have fitted with room to spare; they went anyway.
//     So this is not "drop oldest until it fits" -- a contiguous run after the system
//     message is dropped wholesale. The surviving system prompt is what makes it so
//     dangerous: the model still *sounds* correctly configured, having forgotten
//     everything it did.
//
//  3. **It does not use the window it has.** With 2048 tokens available it kept 64 --
//     3%. Nothing is packed back in after the discard. So the cost of letting the
//     server handle overflow is not "lose the oldest turn", it is "lose almost
//     everything".
//
//  4. **It is a cliff, not a slope.** Measured separately, sweeping a prompt across a
//     4096-token window: 4052 tokens evaluated whole, and 4496 -- ten percent over --
//     evaluated as 44. There is no regime where a small overshoot is trimmed
//     proportionally. Going one token past the window costs the same as going 100x past
//     it, which is what makes *prevention* the only useful strategy and makes a
//     collapse-shaped detector the right backstop.
//
//         pad chars   true tokens   evaluated   kept
//              8000          1829        1829   100%     fits
//             18000          4052        4052   100%     fits
//             20000          4496          44     1%     1.10x over
//             24000          5385          44     0.8%   1.31x over
//             40000          8940          44     0.5%   2.18x over
//
// The conclusion is not that truncation is avoidable -- history outgrows any window --
// but that **this client must decide what to drop, and say so.** That is the difference
// between a supervisor and a chat box.
//
// --- Detecting it anyway, because the estimate can be wrong ------------------
//
// `prepare()` is supposed to guarantee the prompt fits. `record()` checks that it did,
// by comparing `prompt_tokens` from the reply against what was sent. This is R5's
// discipline -- read back after every write -- applied to the prompt: the guarantee is
// verified rather than assumed, because it rests on a token *estimate* and this code
// has no tokenizer.
//
// That check is only possible because `prompt_eval_count` reports the whole prompt
// rather than the newly-evaluated part. Worth stating, since the opposite would have
// made a cache hit indistinguishable from a discard. Measured: the same prompt sent
// twice reported 2127 then 2127, and a two-message continuation 2143 then 2143.
//
// --- Estimating tokens without a tokenizer ----------------------------------
//
// Ollama exposes no tokenize endpoint, and linking a tokenizer per model family is not
// something this project is going to do for an accounting problem. So: estimate
// conservatively, then correct from measurement.
//
// The folklore figure of four characters per token is not safe here, because it is an
// average over English prose and this is an agent that sends paths, code and JSON.
// Measured over the same five samples on both models:
//
//   content            gemma31-agent   qwen35-agent
//   english prose          5.88            5.96
//   source code            2.85            3.11
//   JSON                   2.52            3.11
//   filesystem paths       2.40            2.83
//   base64-ish             1.63            1.36
//
// A 4.0 assumption under-counts real traffic by 40% and a base64 blob by nearly 3x, and
// under-counting is the direction that ends in a silent discard. `kInitialCharsPerToken`
// is therefore pessimistic, and `record()` only ever revises it *downwards* -- never up
// -- so a session cannot talk itself into optimism.
//
// --- Guessing only about what has never been sent ---------------------------
//
// Pessimism has a price, and running the harness against a real model is what showed how
// steep it is. `hermit session --model gemma31-agent --max-num-ctx 2048` estimated
// 935 tokens for a prompt Ollama evaluated at 268, and the session dropped **five turns
// it did not need to drop**. A supervisor that discards history it could have kept is
// failing at its job in the same direction as the server, just more politely.
//
// The way out is that pessimism is only *necessary* for content the model has not seen
// yet. Every prompt already sent has an exact token count sitting in `prompt_tokens`, so
// after each reply the measurement is attributed back to the messages it covered, and
// those messages stop being guesses. The estimate then applies to one new message rather
// than to the whole conversation, and the error stops compounding with every turn.
//
// The ratio still matters, because that one new message might be the 40 KB one. It is
// still only ever revised downwards -- a prose-heavy turn must not buy permission to
// under-count the next turn's file full of paths. Sessions are bounded by design
// (Phase 3: a fresh session per unit of work), so a single awkward blob permanently
// tightening the ratio is a cost worth paying for a bound that only gets safer.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <hermit/ollama/client.h>

namespace hermit::supervisor {

enum class SessionError {
  /// The window cannot hold the system prompt, the reply reserve and one exchange.
  /// Nothing can be dropped to fix this, so it is refused at construction.
  WindowTooSmall,

  /// One message alone does not fit, with everything droppable already dropped.
  /// Fail closed rather than hand the server a prompt it will quietly mangle: a tool
  /// returning more than the window holds is the tool's bug, and this is where it
  /// gets reported instead of becoming a confused model three turns later.
  MessageTooLarge,

  /// A reply arrived whose `prompt_tokens` is far below what was sent -- the server
  /// discarded history despite `prepare()` having judged it to fit. Means the estimate
  /// was wrong, and is reported rather than absorbed.
  PromptWasTruncated,

  /// `record()` called without a matching `prepare()`, or twice for one reply.
  NoRequestOutstanding,
};

std::string_view to_string(SessionError e) noexcept;

struct SessionProblem {
  SessionError kind;
  std::string detail;

  [[nodiscard]] std::string message() const;
};

template <typename T>
using SessionResult = std::expected<T, SessionProblem>;

/// Characters per token assumed before any measurement has been taken.
///
/// Below **every** figure in the table above, base64 at 1.36 included -- a "conservative"
/// figure not checked against the worst measured case is worse than no comment at all:
/// unverified, it can still under-count by up to 47%.
///
/// The reason this is affordable now is attribution. When every turn was estimated, a
/// figure this pessimistic would have inflated the whole conversation and dropped turns
/// needlessly; since the estimate applies only to content that has never been sent, the
/// cost is confined to one message. An over-estimated message may be refused as
/// `MessageTooLarge` when it would in fact have fitted -- a loud, recoverable failure,
/// against a silent collapse to 44 tokens if the guess goes the other way.
inline constexpr double kInitialCharsPerToken = 1.3;

/// Tokens attributed to each message on top of its content, for the chat template's
/// role markers and turn delimiters.
///
/// Not free and not uniform: two messages totalling 141 characters evaluated as 50
/// tokens on gemma31-agent, where the content alone accounts for roughly 24. Gemma has
/// no system role and folds it into the first user turn, so the per-message cost varies
/// by family; 16 is above every value observed.
inline constexpr std::uint64_t kPerMessageOverhead = 16;

/// Weight added to each message's character count when a measurement is shared out
/// across the messages it covered, standing in for the chat template's own tokens.
///
/// Without it an empty assistant reply would be attributed zero tokens despite costing
/// its role markers. Expressed in characters because that is the unit being weighted:
/// roughly `kPerMessageOverhead` tokens at the initial ratio.
inline constexpr std::size_t kTemplateWeightChars = 32;

/// A message, with what the session believes it costs.
struct Turn {
  ollama::ChatMessage message;

  /// The pessimistic estimate from character count. Recomputed when the ratio tightens,
  /// so this is a cache rather than a record.
  std::uint64_t estimated_tokens = 0;

  /// What this message actually cost, once a reply has priced the prompt containing it.
  ///
  /// `prompt_tokens` covers a whole prompt rather than one message, so this is the
  /// message's share of a real measurement rather than a direct reading -- but it is
  /// grounded in one, and a turn that has it stops being guessed about. Includes its
  /// own template overhead, unlike `estimated_tokens`.
  ///
  /// ⚠ **The split is by character count, and cost per character is not uniform** --
  /// the table above spans 1.36 to 5.96. A prompt mixing a base64 blob with English
  /// prose has its true cost apportioned evenly between them when it is not evenly
  /// owed. The *total* stays right, and that is what `prepare()` reads, so this is
  /// harmless while every priced turn is present. It stops being harmless when one is
  /// dropped: the survivors then carry a total that no longer matches what they cost,
  /// and a turn already priced is never re-priced upward. The next reply re-anchors the
  /// total against a real measurement, so the error is bounded to the turns between one
  /// drop and the following exchange, and the cliff detector remains behind it.
  /// Recorded rather than fixed because fixing it needs a per-message token count that
  /// no Ollama endpoint exposes.
  std::optional<std::uint64_t> measured_tokens;

  /// What this turn contributes to the prompt: the measurement if there is one, and the
  /// pessimistic estimate plus template overhead if there is not.
  [[nodiscard]] std::uint64_t cost() const noexcept {
    return measured_tokens.value_or(estimated_tokens + kPerMessageOverhead);
  }

  /// Never dropped to make room. True for the system prompt and for the most recent
  /// user message -- dropping the instruction currently being answered is never the
  /// right trade, and dropping the system prompt would silently change the agent's
  /// rules mid-session.
  bool pinned = false;
};

struct SessionOptions {
  std::string model;

  /// The context window to plan against.
  ///
  /// ⚠ This is a *request*, not the window. What gets planned against is the smallest
  /// of this, the client's `max_num_ctx` clamp (D8) and `architecture_context` below.
  /// A session planning against 131072 while the client sends 65536 would fill the
  /// window to twice its size and be discarded in silence -- the precise failure this
  /// class exists to prevent, arriving through its own configuration -- so `open()`
  /// applies the clamp itself rather than trusting the two to agree.
  std::uint64_t num_ctx = 65536;

  /// The context the model was *built* for, from `ModelCard::architecture_context`.
  ///
  /// The one limit no per-request setting can exceed (see preflight.h), and therefore a
  /// real ceiling on the planning window rather than a preference. Left absent it is
  /// simply not applied, which is the status quo for a caller that has not looked the
  /// model up.
  ///
  /// It matters because the two 65536 constants that make the defaults cohere -- R9's
  /// `minimum_context` floor and D8's `max_num_ctx` clamp -- are independent, and D8
  /// explicitly invites raising the clamp for a bigger card. Raise it past a model's
  /// architecture with nothing else changed and the session would plan against tokens
  /// the model cannot hold, discovering it only from the collapse afterwards.
  std::optional<std::uint64_t> architecture_context;

  /// Tokens held back from the prompt for the model's own output.
  ///
  /// Distinct from `max_tokens`, which bounds generation but reserves nothing: Ollama
  /// will fill the window with prompt and leave the model no space to answer in. The
  /// two have to agree, and `Session::open` checks that they do -- a reserve smaller
  /// than the budget it is reserving for is not a conservative setting, it is a reserve
  /// that does not work.
  std::uint64_t reply_reserve = 4096;

  /// Sent as `num_predict`. Must fit inside `reply_reserve`.
  ///
  /// ⚠ Spent on reasoning first on a thinking model -- too small a value returns empty
  /// content with no error (see ChatReply::completion_tokens).
  std::optional<int> max_tokens = 4096;

  /// Unset means the model's own default sampling -- see ChatRequest::temperature for
  /// why unpinned is the default and what the 0.0 default silently did to R7.
  std::optional<double> temperature{};
};

/// One bounded conversation against one model.
///
/// Blocking and single-threaded like everything else under D1, and pure: it never
/// opens a socket. The caller drives `prepare()` -> `Client::chat()` -> `record()`.
class Session {
 public:
  /// Preferred form: the window is read from the very Client the requests will go
  /// through, so the window planned against and the window sent cannot disagree.
  ///
  /// The overload below takes bare options instead, and that difference is the whole
  /// point of having two. Nothing connects a loose `ClientOptions` to the `Client` a
  /// caller actually dials; hand `open()` one object and `chat()` another with a lower
  /// clamp and the session plans against a window that is never sent. `looks_truncated`
  /// would not save it either -- the discard is a cliff, so the first over-full turn is
  /// already lost. Taking the Client removes the opportunity rather than documenting it.
  [[nodiscard]] static SessionResult<Session> open(SessionOptions options,
                                                   const ollama::Client& client,
                                                   std::string system_prompt);

  /// Testing seam: the same thing against bare options, so the suite does not have to
  /// stand up a Client to exercise policy. Production callers should prefer the
  /// overload above; this one cannot check that the options describe the client that
  /// will be used, because it has not been given it.
  [[nodiscard]] static SessionResult<Session> open(SessionOptions options,
                                                   const ollama::ClientOptions& client,
                                                   std::string system_prompt);

  void add_user(std::string content);

  /// Append one tool result, in the order the calls were made.
  ///
  /// Must follow the `record()` of the assistant turn that asked for it -- a result
  /// whose call is not in history leaves the model answering a question it cannot see.
  /// The loop calls this once per entry in `ChatReply::tool_calls`, including repeats:
  /// measured, llama3.2-3b asked to `read a.txt` twice in one turn, and answering only
  /// the first would leave a call unanswered.
  ///
  /// `content` is already-rendered JSON from `wire.h` -- either the rows or an
  /// `{"error": ...}` object. This class does not know or care which.
  void add_tool_result(std::string tool_name, std::string content);

  /// The messages to send, compacted to fit if they did not.
  ///
  /// Drops the oldest unpinned turns when the prompt outgrows the budget, recording how
  /// many went. Trimmed with hysteresis, to a margin below the budget rather than to an
  /// exact fit: an exact fit would re-trim on every subsequent call, and a prompt whose
  /// head changes every turn defeats the server's prefix cache for the rest of the
  /// session. Deliberately *not* summarisation: what to summarise and when is an open
  /// question in ROADMAP.md, and quietly answering it here would be the wrong place.
  /// Dropping is the honest interim policy, and the count is exposed so it cannot
  /// happen unnoticed.
  ///
  /// **A call and its results are dropped together, never separately.** This was
  /// recorded as an open hazard in DECISIONS.md before tool messages existed, and it is
  /// the reason the unit of dropping is a group rather than a turn. Both halves of the
  /// split are harmful, and the second is the one that matters:
  ///
  ///   - a result whose call is gone is a claim about nothing, and
  ///   - a call whose result is gone reads as still outstanding, and the reliable
  ///     response to an outstanding call is to re-issue it -- which is the repeat-call
  ///     loop this whole project exists to break.
  ///
  /// So the erase unit is: an assistant turn carrying `tool_calls`, plus every `tool`
  /// turn immediately following it. `dropped()` counts turns, not groups, so a session
  /// that gave up one exchange of three messages reports three.
  [[nodiscard]] SessionResult<ollama::ChatRequest> prepare();

  /// Append the reply, verify the prompt was not discarded, and calibrate the estimate.
  ///
  /// Returns `PromptWasTruncated` when `prompt_tokens` falls far short of what was
  /// sent. The history is still updated in that case -- the turn happened, and hiding
  /// it would leave the caller unable to see what the model was actually answering.
  [[nodiscard]] SessionResult<void> record(const ollama::ChatReply& reply);

  /// Replace the droppable history with `instruction`, keeping only what is pinned.
  ///
  /// The reconstruction half of compaction ([compact.h](./compact.h)). Where the trim in
  /// `prepare()` erases the oldest groups and says nothing, this erases **all** of them
  /// and rewrites the pinned task turn to carry what the caller has re-observed in their
  /// place. This class does not compose that text and could not: the world it describes
  /// is read from the filesystem, and nothing here does I/O.
  ///
  /// `instruction` becomes the content of the pinned `user` turn, so it must be the
  /// original task with the note beneath it rather than the note alone -- dropping the
  /// task would erase the one thing compaction is supposed to keep verbatim. A session
  /// with no user turn at all gets one appended, which is the only way this adds a turn.
  ///
  /// **The trailing group is kept**, where the trim would have been free to drop it. A
  /// caller compacts at the top of a turn, so history ends with the results of the calls
  /// the *previous* turn asked for -- appended after that turn's `prepare()`, and so never
  /// seen by the model, which is still waiting on them. Erasing them guarantees it
  /// re-issues the same calls, and re-observation cannot stand in: five of the eight Tier 0
  /// tools only look, so nothing a `read` returned is recoverable from a snapshot. Dropping
  /// the model's *account* of an answer is the design; dropping the answer is not.
  ///
  /// **Returns false, having changed nothing, in the two cases where rebuilding does not
  /// help:**
  ///
  ///   - nothing outside the pinned turns and that trailing group is droppable, so there
  ///     is no history to fold and the rewrite would only restate the task, and
  ///   - the rebuilt prompt would not land under `trim_target()`. Merely *smaller* is not
  ///     enough, and review found two ways it fails: a rebuild that is smaller but still
  ///     over the threshold recompacts next turn and every turn after, and one that is
  ///     smaller but still over budget is followed straight into the trim, which drops the
  ///     unanswered tail this function just went out of its way to keep. Clearing the
  ///     target avoids both, because below it the trim does not run.
  ///
  /// The kept tail therefore survives into the request whenever this returns true -- which
  /// is the guarantee, and it is worth stating that it is conditional. When the rebuild
  /// cannot clear the target this declines, the trim runs instead, and the trim makes no
  /// promise about the tail: if the outstanding results alone exceed the budget it will
  /// drop them like any other group. That case is a window too small for the work, and the
  /// honest answer to it is a bigger window, not a cleverer policy.
  ///
  /// Both are the caller's cue to fall through to the trim, which is why this is a bool
  /// rather than a refusal: neither is an error, and whether the *result* fits the window
  /// remains `prepare()`'s question, asked the same way it always was.
  ///
  /// Two consequences worth stating rather than discovering. The prompt bytes change
  /// right after the system message, so the server's prefix cache misses the whole prompt
  /// on the next turn -- the same cost the trim's hysteresis note describes, paid rarely
  /// and on purpose. And the surviving turns keep measurements that were apportioned
  /// across a prompt they are no longer alone in; that is the bounded, self-correcting
  /// error `Turn::measured_tokens` already documents for the trim, re-anchored by the next
  /// reply. The rewritten task turn is exempt -- its content changed, so its measurement
  /// is discarded rather than carried.
  bool reconstruct(std::string instruction);

  [[nodiscard]] const std::vector<Turn>& turns() const noexcept { return turns_; }

  /// The window actually being planned against, after D8's clamp.
  [[nodiscard]] std::uint64_t window() const noexcept { return window_; }

  /// Prompt tokens available once the reply reserve is held back.
  [[nodiscard]] std::uint64_t prompt_budget() const noexcept { return window_ - options_.reply_reserve; }

  /// What the trim aims for once it fires, and the level a rebuild has to reach to be
  /// worth doing. One function because three things must agree on it.
  ///
  /// `prepare()` trims with hysteresis -- once over budget it drops to a margin below,
  /// never to an exact fit, or it would re-trim every turn and defeat the server's prefix
  /// cache. `reconstruct()` then declines any rebuild that would not land under this,
  /// which is what keeps the two policies from fighting: a rebuild that clears this level
  /// is a rebuild the trim will not touch afterwards.
  ///
  /// It is also the figure `kDefaultCompactAt` is defined to equal. That coupling used to
  /// live in a comment and a test that re-derived the arithmetic independently -- which
  /// meant the trim's divisor could be changed with the whole suite still green. Now there
  /// is one expression and both readers call it.
  [[nodiscard]] std::uint64_t trim_target() const noexcept {
    const std::uint64_t budget = prompt_budget();
    return budget - budget / 5;
  }

  /// How many turns have been dropped to make room, over the session's whole life.
  /// Non-zero means the model has been answering with holes in its history, which is
  /// a fact about the run and belongs in whatever reports it.
  [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }

  /// How many times the window has been rebuilt from re-observed state.
  ///
  /// Deliberately *not* folded into `dropped()`. That counter means "the model has been
  /// answering with holes in its history it was never told about", which is the condition
  /// compaction exists to remove; a reconstructed session has lost the same turns and has
  /// been told so. Reporting them as one number would erase the distinction that is the
  /// entire point of the feature. A run carrying `dropped > 0` with `compactions == 0` is
  /// one where reconstruction never ran -- no verifier, or it would not have helped.
  [[nodiscard]] std::size_t compactions() const noexcept { return compactions_; }

  /// Turns folded into a reconstruction, over the session's whole life. Counts turns
  /// rather than rebuilds, so it reads against `dropped()` on the same scale.
  [[nodiscard]] std::size_t reconstructed() const noexcept { return reconstructed_; }

  /// Current characters-per-token assumption. Starts at `kInitialCharsPerToken` and
  /// only ever decreases.
  [[nodiscard]] double chars_per_token() const noexcept { return chars_per_token_; }

  /// What the current history is reckoned to cost, template overhead included.
  ///
  /// A blend rather than a pure estimate: turns already priced by a real reply
  /// contribute their measurement, and only turns the model has not seen yet are
  /// guessed about. That is what keeps a long session from compounding its own
  /// pessimism into dropping turns it could have kept.
  [[nodiscard]] std::uint64_t estimated_prompt_tokens() const noexcept;

  /// How many turns in the current history have been priced by a real measurement.
  [[nodiscard]] std::size_t measured_turns() const noexcept;

  /// Tokens the model has generated across the session, from `eval_count`. Includes
  /// thinking tokens, which is what makes it the right number to bound.
  [[nodiscard]] std::uint64_t generated_tokens() const noexcept { return generated_tokens_; }

  /// Tokens the daemon reports it actually processed as input across the session, from
  /// `prompt_eval_count`. D18: the raw wire value, not `estimated_prompt_tokens()`'s
  /// window-clamped calibration -- usage tracking needs what was billed, not what the
  /// current window is reckoned to cost.
  [[nodiscard]] std::uint64_t prompt_tokens_seen() const noexcept { return prompt_tokens_seen_; }

  [[nodiscard]] std::size_t completed_turns() const noexcept { return completed_turns_; }

 private:
  Session(SessionOptions options, std::uint64_t window);

  [[nodiscard]] std::uint64_t estimate(std::string_view text) const noexcept;
  void re_estimate() noexcept;
  void pin_latest_user() noexcept;
  void attribute(std::uint64_t measured) noexcept;

  SessionOptions options_;
  std::uint64_t window_ = 0;
  std::vector<Turn> turns_;

  double chars_per_token_ = kInitialCharsPerToken;
  std::size_t dropped_ = 0;
  std::size_t compactions_ = 0;
  std::size_t reconstructed_ = 0;
  std::size_t completed_turns_ = 0;
  std::uint64_t generated_tokens_ = 0;
  std::uint64_t prompt_tokens_seen_ = 0;

  /// Set by `prepare()`, cleared by `record()`. Carries what was sent so the reply can
  /// be checked against it.
  struct Outstanding {
    std::uint64_t estimated_tokens = 0;
    std::size_t message_count = 0;
    std::size_t content_chars = 0;
  };
  std::optional<Outstanding> outstanding_;
};

// --- exposed for testing ----------------------------------------------------

/// Whether a measured `prompt_tokens` is short enough of the estimate to mean the
/// server discarded history rather than the estimate merely being pessimistic.
///
/// The estimate is conservative by construction, so `measured < estimated` is the
/// normal case and cannot itself be the signal. A collapse is: the discard is a cliff
/// (see the table above), so anything the server truncates lands at a few dozen tokens
/// regardless of how far over it went. There is no proportional-trim regime to catch,
/// which is why one threshold suffices and why it can sit far below anything
/// over-estimation produces.
[[nodiscard]] bool looks_truncated(std::uint64_t estimated, std::uint64_t measured) noexcept;

/// Share `remainder` tokens across messages of the given weights.
///
/// Weights are character counts already raised by `kTemplateWeightChars`, so an empty
/// message still receives something. Rounds each share up: over-attributing a token is
/// the harmless direction, and a floor would let a long history quietly under-count by
/// one token per message.
///
/// Returns an empty vector when there is nothing to share out or no weight to share it
/// by, which the caller reads as "leave these turns on their estimates".
[[nodiscard]] std::vector<std::uint64_t> share_out(std::span<const std::size_t> weights,
                                                   std::uint64_t remainder);

/// The tightened characters-per-token figure implied by one measured prompt.
///
/// Returns the current value unchanged when the estimate already covered the
/// measurement; only an under-estimate moves it, and only downwards.
[[nodiscard]] double calibrate(double current, std::size_t content_chars,
                               std::size_t message_count, std::uint64_t measured) noexcept;

}  // namespace hermit::supervisor
