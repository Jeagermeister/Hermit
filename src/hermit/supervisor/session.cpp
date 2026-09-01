#include <hermit/supervisor/session.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace hermit::supervisor {
namespace {

/// Room a window must leave for one exchange beyond the system prompt and the reserve.
/// Small on purpose: this only rejects windows that cannot work at all, and a session
/// whose first message does not fit is `MessageTooLarge` at `prepare()`, reported
/// against the message rather than against the configuration.
constexpr std::uint64_t kMinimumExchangeTokens = 64;

/// Below this many estimated tokens, a shortfall is not read as a discard.
///
/// The ratio between estimate and measurement is noisy on short prompts, and a discard
/// that small cannot have cost anything worth reporting. The measured collapse was
/// 9005 tokens to 64, which is nowhere near this floor.
constexpr std::uint64_t kTruncationFloor = 256;

/// How far short of the estimate a measurement must fall to mean a discard.
///
/// The estimate is deliberately pessimistic, so `measured < estimated` is ordinary and
/// cannot itself be the signal: English prose at the initial 2.0 assumption measures
/// around a third of its estimate, which is honest over-estimation and must not fire.
/// A factor of 8 sits well clear of that and far above the 0.7% the real discard
/// produced.
constexpr std::uint64_t kTruncationFactor = 8;

/// Never assume fewer characters per token than this. Guards the calibration against a
/// pathological sample driving the estimate to absurdity; 0.5 is already below any
/// tokenizer's behaviour on real bytes.
constexpr double kMinCharsPerToken = 0.5;

/// Applied to a newly measured ratio before adopting it, so the estimate lands under
/// the observation rather than exactly on it.
constexpr double kCalibrationMargin = 0.9;

}  // namespace

std::string_view to_string(SessionError e) noexcept {
  switch (e) {
    case SessionError::WindowTooSmall: return "context window too small";
    case SessionError::MessageTooLarge: return "message larger than the context window";
    case SessionError::PromptWasTruncated: return "the server discarded part of the prompt";
    case SessionError::NoRequestOutstanding: return "no prepared request to record against";
  }
  return "unknown session error";
}

std::string SessionProblem::message() const {
  std::string out{to_string(kind)};
  if (!detail.empty()) {
    out += ": ";
    out += detail;
  }
  return out;
}

bool looks_truncated(std::uint64_t estimated, std::uint64_t measured) noexcept {
  if (estimated < kTruncationFloor) return false;
  // Compare by division rather than multiplication: `measured * kTruncationFactor`
  // would be the natural phrasing and can overflow, which on an unsigned type wraps to
  // a small number and reports a truncation that did not happen.
  return measured < estimated / kTruncationFactor;
}

double calibrate(double current, std::size_t content_chars, std::size_t message_count,
                 std::uint64_t measured) noexcept {
  if (content_chars == 0) return current;

  const std::uint64_t overhead = static_cast<std::uint64_t>(message_count) * kPerMessageOverhead;
  if (measured <= overhead) return current;  // the overhead assumption already covers it

  const auto content_tokens = static_cast<double>(measured - overhead);
  const double observed = static_cast<double>(content_chars) / content_tokens;

  // Only ever tighten. An observation that the estimate was already generous is not a
  // licence to become less careful -- one prose-heavy turn would otherwise raise the
  // ratio just in time for the next turn to carry a file full of paths.
  if (observed >= current) return current;
  return std::max(observed * kCalibrationMargin, kMinCharsPerToken);
}

Session::Session(SessionOptions options, std::uint64_t window)
    : options_(std::move(options)), window_(window) {}

SessionResult<Session> Session::open(SessionOptions options, const ollama::Client& client,
                                     std::string system_prompt) {
  ollama::ClientOptions echo;
  echo.max_num_ctx = client.max_num_ctx();
  return open(std::move(options), echo, std::move(system_prompt));
}

SessionResult<Session> Session::open(SessionOptions options, const ollama::ClientOptions& client,
                                     std::string system_prompt) {
  // Three ceilings, and the smallest wins. The clamp is the client's, applied here
  // rather than restated -- a session planning against a window the client will not
  // send is the failure this class exists to prevent, and it would arrive through its
  // own configuration. The architecture is the model's, and is the one limit no
  // per-request setting can raise.
  std::uint64_t window = std::min(options.num_ctx, client.max_num_ctx);
  if (options.architecture_context) window = std::min(window, *options.architecture_context);

  // Nothing downstream can make sense of a non-positive generation budget, and one that
  // arrives negative is worse than useless: Ollama reads a negative `num_predict` as
  // "generate without limit", so the field meant to bound the model would remove its
  // bound. The same shape as the `--chat-timeout` overflow that turned a timeout into
  // "wait forever", one layer up.
  if (options.max_tokens && *options.max_tokens <= 0) {
    return std::unexpected(SessionProblem{
        SessionError::WindowTooSmall,
        "a generation budget of " + std::to_string(*options.max_tokens) +
            " tokens is not a bound; Ollama reads a non-positive num_predict as unlimited"});
  }

  if (options.reply_reserve >= window) {
    return std::unexpected(SessionProblem{
        SessionError::WindowTooSmall,
        "reply reserve of " + std::to_string(options.reply_reserve) +
            " tokens leaves nothing for the prompt in a window of " + std::to_string(window)});
  }

  // A reserve smaller than the generation budget reserves nothing useful: the prompt is
  // allowed to fill the window right up to the reserve, and the model then generates
  // past it. Ollama does not refuse that -- it shifts the window, which is the silent
  // discard this class exists to prevent, arriving from the other end.
  if (options.max_tokens &&
      static_cast<std::uint64_t>(*options.max_tokens) > options.reply_reserve) {
    return std::unexpected(SessionProblem{
        SessionError::WindowTooSmall,
        "a generation budget of " + std::to_string(*options.max_tokens) +
            " tokens does not fit the " + std::to_string(options.reply_reserve) +
            " reserved for it"});
  }

  Session session{std::move(options), window};

  const std::uint64_t system_tokens = session.estimate(system_prompt);
  std::uint64_t needed = kMinimumExchangeTokens;
  if (!system_prompt.empty()) {
    needed += system_tokens + kPerMessageOverhead;
  }
  if (needed > session.prompt_budget()) {
    return std::unexpected(SessionProblem{
        SessionError::WindowTooSmall,
        "system prompt and one exchange need about " + std::to_string(needed) +
            " tokens, but only " + std::to_string(session.prompt_budget()) +
            " are left for the prompt in a window of " + std::to_string(window)});
  }

  if (!system_prompt.empty()) {
    session.turns_.push_back(Turn{.message = {.role = "system", .content = std::move(system_prompt)},
                                  .estimated_tokens = system_tokens,
                                  .measured_tokens = std::nullopt,
                                  .pinned = true});
  }
  return session;
}

std::uint64_t Session::estimate(std::string_view text) const noexcept {
  if (text.empty()) return 0;
  const double tokens = static_cast<double>(text.size()) / chars_per_token_;
  return static_cast<std::uint64_t>(std::ceil(tokens));
}

void Session::re_estimate() noexcept {
  for (Turn& turn : turns_) turn.estimated_tokens = estimate(turn.message.content);
}

void Session::pin_latest_user() noexcept {
  bool found_user = false;
  for (auto it = turns_.rbegin(); it != turns_.rend(); ++it) {
    if (it->message.role == "system") {
      it->pinned = true;
      continue;
    }
    // Only the *most recent* user message is pinned; earlier ones are droppable, which
    // is what lets a long session make room at all.
    if (!found_user && it->message.role == "user") {
      it->pinned = true;
      found_user = true;
      continue;
    }
    it->pinned = false;
  }
}

std::uint64_t Session::estimated_prompt_tokens() const noexcept {
  std::uint64_t total = 0;
  for (const Turn& turn : turns_) total += turn.cost();
  return total;
}

std::size_t Session::measured_turns() const noexcept {
  return static_cast<std::size_t>(
      std::count_if(turns_.begin(), turns_.end(),
                    [](const Turn& t) { return t.measured_tokens.has_value(); }));
}

std::vector<std::uint64_t> share_out(std::span<const std::size_t> weights,
                                     std::uint64_t remainder) {
  if (weights.empty() || remainder == 0) return {};

  std::size_t total_weight = 0;
  for (const std::size_t w : weights) total_weight += w;
  if (total_weight == 0) return {};

  std::vector<std::uint64_t> shares;
  shares.reserve(weights.size());
  for (const std::size_t w : weights) {
    // Rounded up. The alternative loses a token per message to truncation, which over a
    // long history is exactly the quiet under-count this class exists to avoid.
    const double share = static_cast<double>(remainder) * static_cast<double>(w) /
                         static_cast<double>(total_weight);
    shares.push_back(static_cast<std::uint64_t>(std::ceil(share)));
  }
  return shares;
}

void Session::attribute(std::uint64_t measured) noexcept {
  // What the prompt cost is known exactly; which message cost what is not. Turns already
  // priced by an earlier reply keep their figure -- their content has not changed -- and
  // whatever is left over is shared across the ones still being guessed about.
  std::uint64_t known = 0;
  std::vector<std::size_t> weights;
  std::vector<Turn*> unpriced;
  for (Turn& turn : turns_) {
    if (turn.measured_tokens) {
      known += *turn.measured_tokens;
    } else {
      weights.push_back(turn.message.content.size() + kTemplateWeightChars);
      unpriced.push_back(&turn);
    }
  }
  if (unpriced.empty() || measured <= known) {
    // Nothing to price, or the earlier attributions already account for the whole
    // prompt. Leaving the rest on their estimates is the conservative reading, and the
    // estimates are the pessimistic ones.
    return;
  }

  const std::vector<std::uint64_t> shares = share_out(weights, measured - known);
  if (shares.size() != unpriced.size()) return;
  for (std::size_t i = 0; i < unpriced.size(); ++i) unpriced[i]->measured_tokens = shares[i];
}

void Session::add_user(std::string content) {
  const std::uint64_t tokens = estimate(content);
  turns_.push_back(Turn{.message = {.role = "user", .content = std::move(content)},
                        .estimated_tokens = tokens,
                        .measured_tokens = std::nullopt,
                        .pinned = false});
  // A message added between prepare() and record() would land the assistant reply after
  // it, silently reordering the conversation. Dropping the outstanding request turns
  // that into `NoRequestOutstanding` at record() rather than a scrambled history.
  outstanding_.reset();
}

void Session::add_tool_result(std::string tool_name, std::string content) {
  const std::uint64_t tokens = estimate(content);
  turns_.push_back(Turn{.message = {.role = "tool",
                                    .content = std::move(content),
                                    .tool_name = std::move(tool_name)},
                        .estimated_tokens = tokens,
                        .measured_tokens = std::nullopt,
                        .pinned = false});
  // Same reasoning as add_user: anything appended between prepare() and record() would
  // reorder the conversation, so the outstanding request is given up rather than
  // checked against a history it no longer describes.
  outstanding_.reset();
}

namespace {

/// How many turns belong to the group starting at `first`.
///
/// A group is an assistant turn carrying tool calls together with every `tool` turn
/// immediately following it; anything else is a group of one. See prepare()'s header
/// note for why the two halves cannot be separated.
///
/// A stray `tool` turn whose assistant is already gone is a group of one, which drops
/// it. That should not arise -- the loop records the assistant turn before appending
/// any result -- and if it ever does, an orphan is better removed than preserved.
std::size_t group_size(const std::vector<Turn>& turns, std::size_t first) {
  if (turns[first].message.tool_calls.empty()) return 1;
  std::size_t last = first + 1;
  while (last < turns.size() && turns[last].message.role == "tool") ++last;
  return last - first;
}

}  // namespace

SessionResult<ollama::ChatRequest> Session::prepare() {
  pin_latest_user();

  const std::uint64_t budget = prompt_budget();
  if (estimated_prompt_tokens() > budget) {
    // Trimmed with hysteresis: once over, drop down to a margin below the budget, not
    // to an exact fit. An exact fit re-trims on every subsequent prepare() -- each
    // exchange adds a group, so the rendered prompt would diverge from the previous
    // request right after the system message, and the server's prefix cache would
    // re-evaluate the whole prompt on every turn for the rest of the session. The
    // margin gives up a little history a few turns early to keep the prompt
    // byte-stable between trims.
    const std::uint64_t target = budget - budget / 5;
    while (estimated_prompt_tokens() > target) {
      const auto victim = std::find_if(turns_.begin(), turns_.end(),
                                       [](const Turn& t) { return !t.pinned; });
      if (victim == turns_.end()) {
        // Everything left is pinned: the system prompt plus the message being answered.
        // Inside the budget that is simply a prompt that fits -- the margin is a
        // preference, never a reason to refuse. Over the budget, nothing can be given
        // up, so this is refused rather than handed to a server that would discard the
        // middle of it and say nothing.
        if (estimated_prompt_tokens() <= budget) break;
        return std::unexpected(SessionProblem{
            SessionError::MessageTooLarge,
            "the system prompt and the current message need about " +
                std::to_string(estimated_prompt_tokens()) + " tokens, but only " +
                std::to_string(budget) + " are available in a window of " +
                std::to_string(window_)});
      }
      // The whole group goes, or none of it. A pinned turn cannot appear inside a group --
      // pin_latest_user pins only `system` and the latest `user`, and a group is an
      // assistant turn followed by `tool` turns -- so this never erases something pinned.
      const auto first = static_cast<std::size_t>(std::distance(turns_.begin(), victim));
      const std::size_t count = group_size(turns_, first);
      dropped_ += count;
      turns_.erase(victim, victim + static_cast<std::ptrdiff_t>(count));
    }
  }

  ollama::ChatRequest request;
  request.model = options_.model;
  request.temperature = options_.temperature;
  request.max_tokens = options_.max_tokens;
  request.num_ctx = window_;
  request.messages.reserve(turns_.size());

  std::size_t content_chars = 0;
  for (const Turn& turn : turns_) {
    content_chars += turn.message.content.size();
    request.messages.push_back(turn.message);
  }

  outstanding_ = Outstanding{.estimated_tokens = estimated_prompt_tokens(),
                             .message_count = turns_.size(),
                             .content_chars = content_chars};
  return request;
}

SessionResult<void> Session::record(const ollama::ChatReply& reply) {
  if (!outstanding_) {
    return std::unexpected(SessionProblem{SessionError::NoRequestOutstanding,
                                          "record() needs a prepare() it can check against"});
  }
  const Outstanding sent = *outstanding_;
  outstanding_.reset();

  // Everything below runs against `turns_` as it was *sent*; the reply is appended last.
  // Attributing a measurement of the prompt across a message that was not in it would
  // credit the assistant's own answer with tokens the server never saw. Its estimate is
  // likewise computed at the end rather than here, because the calibration below may
  // tighten the ratio -- pricing it first would leave a stale, too-low figure that
  // `re_estimate()` cannot reach (it walks `turns_`, which the reply has not joined yet)
  // and that the next `prepare()` would then admit against the budget.
  ++completed_turns_;
  generated_tokens_ += reply.completion_tokens;

  // A prompt cannot evaluate more tokens than the window it was sent in; `num_ctx` is
  // what that number means. Anything above it is a daemon this code cannot reason
  // about, and letting it through would drive the calibration to its floor and reach
  // `share_out`, where a value near the limit of the type stops being safe arithmetic.
  const std::uint64_t measured = std::min(reply.prompt_tokens, window_);

  if (looks_truncated(sent.estimated_tokens, measured)) {
    // The reply still joins the history. The turn happened, and hiding it would leave
    // the caller unable to see what the model was answering when it went wrong.
    turns_.push_back(Turn{.message = {.role = "assistant",
                                    .content = reply.content,
                                    .tool_calls = reply.tool_calls},
                          .estimated_tokens = estimate(reply.content),
                          .measured_tokens = std::nullopt,
                          .pinned = false});
    return std::unexpected(SessionProblem{
        SessionError::PromptWasTruncated,
        "sent about " + std::to_string(sent.estimated_tokens) + " tokens across " +
            std::to_string(sent.message_count) + " messages, but the server evaluated only " +
            std::to_string(reply.prompt_tokens) +
            " -- history was discarded and the reply answers a conversation with holes in it"});
  }

  // Only learn from a prompt that arrived whole. A truncated `prompt_tokens` describes a
  // prompt this session did not send, and both the ratio and the per-turn attribution
  // would be recording a measurement of something else.
  const double revised =
      calibrate(chars_per_token_, sent.content_chars, sent.message_count, measured);
  if (revised < chars_per_token_) {
    chars_per_token_ = revised;
    re_estimate();
  }

  // Defensive: `add_user` drops the outstanding request precisely so this cannot happen,
  // and if it somehow does, guessing is safer than attributing across the wrong messages.
  if (sent.message_count == turns_.size()) attribute(measured);

  // Priced last, against the ratio as calibration has just left it -- see above.
  // `reasoning` is deliberately not carried into the history: it is the model's working
  // rather than its answer, Ollama returns it in its own field rather than in content,
  // and feeding it back would spend context on tokens the model did not ask to see again.
  turns_.push_back(Turn{.message = {.role = "assistant",
                                    .content = reply.content,
                                    .tool_calls = reply.tool_calls},
                        .estimated_tokens = estimate(reply.content),
                        .measured_tokens = std::nullopt,
                        .pinned = false});
  return {};
}

bool Session::reconstruct(std::string instruction) {
  // Establish the pins first: everything else here is expressed in terms of them, and
  // `prepare()` would recompute them on the next call anyway. After this the pinned set
  // is exactly the system prompt and the most recent user turn.
  pin_latest_user();

  // The trailing group is kept, and this is not a refinement -- without it compaction is
  // broken in the ordinary case rather than an unusual one.
  //
  // A caller compacts at the top of a turn, and history then ends with the results of the
  // calls the *previous* turn asked for. Those were appended after that turn's prepare(),
  // so the model has never seen them: it is still waiting on them. Erasing them sends it a
  // window in which it asked nothing and learned nothing, and the reliable response to a
  // call with no answer is to re-issue it -- the repeat-call loop this project exists to
  // break, arriving from the policy meant to help.
  //
  // Re-observation cannot stand in for them either, which is what separates this from the
  // history compaction *does* drop. Five of the eight Tier 0 tools only look; a `read` or a
  // `grep` moves nothing, so no snapshot recovers what it returned. Dropping the model's
  // account of an answer is the whole design. Dropping the answer is not.
  //
  // The group is identified the way prepare()'s trim identifies one -- a run of `tool`
  // turns and the assistant turn whose calls they answer -- so the two policies cannot
  // disagree about where a group starts.
  std::size_t keep_from = turns_.size();
  if (!turns_.empty() && turns_.back().message.role == "tool") {
    keep_from = turns_.size() - 1;
    while (keep_from > 0 && turns_[keep_from - 1].message.role == "tool") --keep_from;
    if (keep_from > 0 && !turns_[keep_from - 1].message.tool_calls.empty()) --keep_from;
  }

  const auto erasable = [&](std::size_t i) { return !turns_[i].pinned && i < keep_from; };

  std::size_t droppable = 0;
  for (std::size_t i = 0; i < turns_.size(); ++i) {
    if (erasable(i)) ++droppable;
  }
  if (droppable == 0) return false;  // nothing to fold; the rewrite would only restate

  // What survives, and whether the rebuild is actually an improvement. The task turn is
  // excluded from `kept` because it is about to be replaced rather than carried.
  std::uint64_t kept = 0;
  bool has_task = false;
  for (std::size_t i = 0; i < turns_.size(); ++i) {
    if (erasable(i)) continue;
    if (turns_[i].pinned && turns_[i].message.role == "user") {
      has_task = true;
      continue;
    }
    kept += turns_[i].cost();
  }

  // The note is composed from re-observed state and has no fixed size, so a rebuild can
  // cost more than the history it replaces -- a handful of turns against a changeset of
  // hundreds of paths, or a kept trailing group that is most of the budget on its own.
  // Compacting anyway would grow the prompt, and a caller firing on a threshold would then
  // compact every turn without ever getting under it. Refusing here makes compaction
  // non-regressive by construction and hands the case back to the trim, which drops from
  // the front and can always make progress.
  const std::uint64_t rebuilt = kept + estimate(instruction) + kPerMessageOverhead;
  if (rebuilt >= estimated_prompt_tokens()) return false;

  std::vector<Turn> rebuilt_turns;
  rebuilt_turns.reserve(turns_.size() - droppable);
  for (std::size_t i = 0; i < turns_.size(); ++i) {
    if (erasable(i)) continue;
    rebuilt_turns.push_back(std::move(turns_[i]));
  }
  turns_ = std::move(rebuilt_turns);

  if (has_task) {
    // Exactly one user turn survives the erase, so the first one found is the task. It
    // keeps its position, which is what preserves the ordering the chat template needs:
    // the task still precedes the assistant turn whose results follow it.
    for (Turn& turn : turns_) {
      if (turn.message.role != "user") continue;
      turn.message.content = std::move(instruction);
      turn.estimated_tokens = estimate(turn.message.content);
      // The content is not what was priced. Carrying the old figure would let a much
      // larger message be admitted against the budget on a measurement of a smaller one,
      // which is the under-count direction that ends in a silent discard.
      turn.measured_tokens.reset();
      break;
    }
  } else {
    // Only reachable through the bare Session API -- AgentLoop::run always adds the
    // instruction before any turn is taken. Appending is still the right answer: the
    // caller asked for a window rebuilt around this text, and refusing would leave a
    // session with no task in it at all.
    const std::uint64_t tokens = estimate(instruction);
    turns_.push_back(Turn{.message = {.role = "user", .content = std::move(instruction)},
                          .estimated_tokens = tokens,
                          .measured_tokens = std::nullopt,
                          .pinned = true});
  }

  reconstructed_ += droppable;
  ++compactions_;

  // Same reasoning as add_user and add_tool_result, and more sharply: history did not
  // merely gain a message, it was rewritten. A reply recorded against the prepare() that
  // preceded this would be checked against a prompt that no longer exists.
  outstanding_.reset();
  return true;
}

}  // namespace hermit::supervisor
