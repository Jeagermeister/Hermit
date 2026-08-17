// Session and history, exercised without a daemon.
//
// That is the point of the class taking a ChatReply rather than a Client: the policy
// that decides what to drop, and the check that catches a server-side discard, are both
// reachable from a unit test. The behaviour being defended against was measured against
// a live Ollama (see session.h) and is reproduced here as data rather than as a live
// call, for the same reason D8's clamp is tested offline -- verifying it for real means
// performing the failure.

#include <hermit/supervisor/session.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using hermit::supervisor::calibrate;
using hermit::supervisor::kInitialCharsPerToken;
using hermit::supervisor::kPerMessageOverhead;
using hermit::supervisor::looks_truncated;
using hermit::supervisor::Session;
using hermit::supervisor::SessionError;
using hermit::supervisor::SessionOptions;

hermit::ollama::ClientOptions client_with(std::uint64_t max_num_ctx) {
  hermit::ollama::ClientOptions options;
  options.max_num_ctx = max_num_ctx;
  return options;
}

SessionOptions options_with(std::uint64_t num_ctx, std::uint64_t reserve) {
  SessionOptions options;
  options.model = "test-model";
  options.num_ctx = num_ctx;
  options.reply_reserve = reserve;
  // The generation budget has to fit the room reserved for it, so these move together
  // unless a test is deliberately setting them against each other.
  options.max_tokens = static_cast<int>(reserve);
  return options;
}

std::string text_of(std::size_t chars) { return std::string(chars, 'x'); }

/// A session with a 1000-token window and 100 reserved, so the prompt budget is 900 and
/// the arithmetic in these tests can be done by hand.
Session small_session(const std::string& system = "sys") {
  auto session = Session::open(options_with(1000, 100), client_with(65536), system);
  // Not EXPECT_TRUE: that is non-fatal, so a helper using it would go on to dereference
  // an empty expected and crash the whole binary instead of failing one test.
  if (!session) throw std::runtime_error("small_session: " + session.error().message());
  return std::move(*session);
}

hermit::ollama::ChatReply reply_with(std::string content, std::uint64_t prompt_tokens,
                                     std::uint64_t completion_tokens = 10) {
  hermit::ollama::ChatReply reply;
  reply.content = std::move(content);
  reply.prompt_tokens = prompt_tokens;
  reply.completion_tokens = completion_tokens;
  reply.finish_reason = "stop";
  return reply;
}

// --- the window, and the clamp that decides it ------------------------------

TEST(SessionWindow, PlansAgainstTheClampedWindowRatherThanTheRequestedOne) {
  // The trap this guards: asking for 131072 while the client will send 65536 fills the
  // window to twice its size, and the server discards the overflow without a word.
  const auto session = Session::open(options_with(131072, 4096), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  EXPECT_EQ(session->window(), 65536u);
  EXPECT_EQ(session->prompt_budget(), 65536u - 4096u);
}

TEST(SessionWindow, LeavesTheRequestedWindowAloneWhenItIsUnderTheClamp) {
  const auto session = Session::open(options_with(8192, 1024), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  EXPECT_EQ(session->window(), 8192u);
}

TEST(SessionWindow, RefusesAReserveThatLeavesNoRoomForAPrompt) {
  const auto session = Session::open(options_with(4096, 4096), client_with(65536), "sys");
  ASSERT_FALSE(session.has_value());
  EXPECT_EQ(session.error().kind, SessionError::WindowTooSmall);
}

TEST(SessionWindow, RefusesAWindowThatCannotHoldTheSystemPromptAndAnExchange) {
  const auto session =
      Session::open(options_with(600, 100), client_with(65536), text_of(4000));
  ASSERT_FALSE(session.has_value());
  EXPECT_EQ(session.error().kind, SessionError::WindowTooSmall);
  // The message has to name both numbers, or the operator cannot tell which to change.
  EXPECT_NE(session.error().message().find("600"), std::string::npos);
}

TEST(SessionWindow, RefusesAGenerationBudgetLargerThanTheReserveHoldingItsRoom) {
  // The reserve is what keeps the prompt from filling the window; a larger generation
  // budget means the model runs past it and the server shifts the window, which is the
  // same silent discard arriving from the other end.
  SessionOptions options = options_with(65536, 1024);
  options.max_tokens = 4096;
  const auto session = Session::open(options, client_with(65536), "sys");
  ASSERT_FALSE(session.has_value());
  EXPECT_EQ(session.error().kind, SessionError::WindowTooSmall);
}

TEST(SessionWindow, AcceptsAGenerationBudgetThatFitsItsReserve) {
  SessionOptions options = options_with(65536, 4096);
  options.max_tokens = 4096;
  EXPECT_TRUE(Session::open(options, client_with(65536), "sys").has_value());
}

TEST(SessionWindow, RefusesANonPositiveGenerationBudget) {
  // Ollama reads a non-positive num_predict as "generate without limit", so the field
  // meant to bound the model would remove its bound instead -- the same shape as the
  // oversized --chat-timeout that wrapped to -1 and meant "wait forever".
  SessionOptions options = options_with(65536, 4096);
  options.max_tokens = -1794967296;  // what max_num_ctx/4 casts to at 1e10
  const auto session = Session::open(options, client_with(65536), "sys");
  ASSERT_FALSE(session.has_value());
  EXPECT_EQ(session.error().kind, SessionError::WindowTooSmall);

  options.max_tokens = 0;
  EXPECT_FALSE(Session::open(options, client_with(65536), "sys").has_value());
}

TEST(SessionWindow, TheArchitectureCeilingBoundsThePlanningWindow) {
  // R9 gates on the architecture because it is the one limit no per-request setting can
  // raise. D8 invites raising max_num_ctx for a bigger card, and the two constants that
  // make the defaults cohere are independent -- so the clamp alone is not enough.
  SessionOptions options = options_with(65536, 4096);
  options.architecture_context = 8192;
  const auto session = Session::open(options, client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  EXPECT_EQ(session->window(), 8192u);
}

TEST(SessionWindow, AGenerousArchitectureDoesNotRaiseTheClamp) {
  SessionOptions options = options_with(65536, 4096);
  options.architecture_context = 1048576;
  const auto session = Session::open(options, client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  EXPECT_EQ(session->window(), 65536u);
}

TEST(SessionWindow, AnAbsentArchitectureSimplyIsNotApplied) {
  SessionOptions options = options_with(32768, 4096);
  options.architecture_context = std::nullopt;
  const auto session = Session::open(options, client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  EXPECT_EQ(session->window(), 32768u);
}

TEST(SessionWindow, AnEmptySystemPromptAddsNoTurn) {
  const auto session = Session::open(options_with(4096, 512), client_with(65536), "");
  ASSERT_TRUE(session.has_value());
  EXPECT_TRUE(session->turns().empty());
}

// --- estimation --------------------------------------------------------------

TEST(SessionEstimate, StartsBelowEveryMeasuredRatioIncludingTheWorstOne) {
  // Not merely below realistic traffic: below the worst figure in the table, which is
  // base64 at 1.36 on qwen35-agent. An earlier 2.0 sat above that and under-counted a
  // base64-dense blob by up to 47%, while the header called under-counting the direction
  // that ends in a discard. Attribution is what makes this affordable -- the guess now
  // covers one unsent message rather than the whole conversation.
  EXPECT_LE(kInitialCharsPerToken, 1.36);
}

TEST(SessionEstimate, CountsPerMessageTemplateOverhead) {
  auto session = small_session("");
  session.add_user(text_of(100));
  const auto content = static_cast<std::uint64_t>(std::ceil(100.0 / kInitialCharsPerToken));
  EXPECT_EQ(session.estimated_prompt_tokens(), content + kPerMessageOverhead);
}

TEST(SessionEstimate, AnEmptyMessageStillCostsItsTemplateOverhead) {
  auto session = small_session("");
  session.add_user("");
  EXPECT_EQ(session.estimated_prompt_tokens(), kPerMessageOverhead);
}

// --- preparing a request -----------------------------------------------------

TEST(SessionPrepare, SendsTheClampedWindowAsNumCtx) {
  auto session = Session::open(options_with(131072, 4096), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user("hello");
  const auto request = session->prepare();
  ASSERT_TRUE(request.has_value());
  ASSERT_TRUE(request->num_ctx.has_value());
  EXPECT_EQ(*request->num_ctx, 65536u);
  EXPECT_EQ(request->model, "test-model");
}

TEST(SessionPrepare, KeepsEverythingWhenItFits) {
  auto session = small_session();
  session.add_user("first");
  const auto request = session.prepare();
  ASSERT_TRUE(request.has_value());
  ASSERT_EQ(request->messages.size(), 2u);
  EXPECT_EQ(request->messages[0].role, "system");
  EXPECT_EQ(request->messages[1].content, "first");
  EXPECT_EQ(session.dropped(), 0u);
}

TEST(SessionPrepare, DropsTheOldestUnpinnedTurnToMakeRoom) {
  // Derived from the ratio rather than hand-written, so tightening the constant tunes
  // the estimate instead of breaking the test that checks the drop policy.
  auto session = small_session();
  const auto per_turn =
      static_cast<std::uint64_t>(std::ceil(200.0 / kInitialCharsPerToken)) + kPerMessageOverhead;
  const auto system_cost =
      static_cast<std::uint64_t>(std::ceil(3.0 / kInitialCharsPerToken)) + kPerMessageOverhead;
  const auto fits = (session.prompt_budget() - system_cost) / per_turn;

  for (std::uint64_t i = 0; i < fits + 1; ++i) session.add_user(text_of(200));

  const auto request = session.prepare();
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(session.dropped(), 1u);
  EXPECT_EQ(request->messages.size(), fits + 1);  // system plus the survivors
  EXPECT_EQ(request->messages[0].role, "system");
  EXPECT_LE(session.estimated_prompt_tokens(), session.prompt_budget());
}

TEST(SessionPrepare, NeverDropsTheSystemPrompt) {
  auto session = small_session();
  for (int i = 0; i < 40; ++i) session.add_user(text_of(200));
  const auto request = session.prepare();
  ASSERT_TRUE(request.has_value());
  ASSERT_FALSE(request->messages.empty());
  EXPECT_EQ(request->messages.front().role, "system");
  EXPECT_EQ(request->messages.front().content, "sys");
  EXPECT_GT(session.dropped(), 1u);
}

TEST(SessionPrepare, NeverDropsTheMessageCurrentlyBeingAnswered) {
  auto session = small_session();
  for (int i = 0; i < 20; ++i) session.add_user(text_of(200));
  session.add_user("the question that matters");

  const auto request = session.prepare();
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->messages.back().content, "the question that matters");
}

TEST(SessionPrepare, RefusesAMessageLargerThanTheWindowInsteadOfLettingTheServerManageIt) {
  // A tool returning more than the window holds is the tool's bug. Reporting it here
  // beats handing it over to be silently gutted -- which is what the server does, and
  // it keeps the system prompt while doing it, so the model still looks configured.
  auto session = small_session();
  session.add_user(text_of(10000));  // 5000 tokens against a 900-token budget

  const auto request = session.prepare();
  ASSERT_FALSE(request.has_value());
  EXPECT_EQ(request.error().kind, SessionError::MessageTooLarge);
}

TEST(SessionPrepare, CountsEveryDropAcrossTheWholeSession) {
  auto session = small_session();
  for (int i = 0; i < 8; ++i) session.add_user(text_of(200));
  ASSERT_TRUE(session.prepare().has_value());
  const std::size_t after_first = session.dropped();
  ASSERT_GT(after_first, 0u);

  for (int i = 0; i < 4; ++i) session.add_user(text_of(200));
  ASSERT_TRUE(session.prepare().has_value());
  EXPECT_GT(session.dropped(), after_first);
}

// --- recording a reply -------------------------------------------------------

TEST(SessionRecord, AppendsTheReplyAndAccumulatesTheGenerationCount) {
  auto session = small_session();
  session.add_user("question");
  ASSERT_TRUE(session.prepare().has_value());

  ASSERT_TRUE(session.record(reply_with("answer", 30, 42)).has_value());
  EXPECT_EQ(session.turns().back().message.role, "assistant");
  EXPECT_EQ(session.turns().back().message.content, "answer");
  EXPECT_EQ(session.completed_turns(), 1u);
  EXPECT_EQ(session.generated_tokens(), 42u);
}

TEST(SessionRecord, RefusesAReplyWithNoPreparedRequestBehindIt) {
  auto session = small_session();
  const auto recorded = session.record(reply_with("answer", 30));
  ASSERT_FALSE(recorded.has_value());
  EXPECT_EQ(recorded.error().kind, SessionError::NoRequestOutstanding);
}

TEST(SessionRecord, RefusesToRecordTheSameReplyTwice) {
  auto session = small_session();
  session.add_user("question");
  ASSERT_TRUE(session.prepare().has_value());
  ASSERT_TRUE(session.record(reply_with("answer", 30)).has_value());

  const auto again = session.record(reply_with("answer", 30));
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().kind, SessionError::NoRequestOutstanding);
}

TEST(SessionRecord, AMessageAddedAfterPrepareInvalidatesTheOutstandingRequest) {
  // Otherwise the assistant reply lands after the new user message and the history is
  // silently out of order -- which is worse than an error, because it still works.
  auto session = small_session();
  session.add_user("question");
  ASSERT_TRUE(session.prepare().has_value());
  session.add_user("second thought");

  const auto recorded = session.record(reply_with("answer", 30));
  ASSERT_FALSE(recorded.has_value());
  EXPECT_EQ(recorded.error().kind, SessionError::NoRequestOutstanding);
}

TEST(SessionRecord, ReportsAServerSideDiscardRatherThanAbsorbingIt) {
  // The measured case: gemma31-agent evaluated 64 tokens of a 9005-token prompt and the
  // reply came back looking entirely healthy.
  auto session = Session::open(options_with(65536, 4096), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user(text_of(20000));  // ~10000 estimated tokens, comfortably inside
  ASSERT_TRUE(session->prepare().has_value());

  const auto recorded = session->record(reply_with("looks fine", 64));
  ASSERT_FALSE(recorded.has_value());
  EXPECT_EQ(recorded.error().kind, SessionError::PromptWasTruncated);
  EXPECT_NE(recorded.error().message().find("64"), std::string::npos);
}

TEST(SessionRecord, KeepsTheReplyInHistoryEvenWhenItReportsADiscard) {
  // The turn happened. Dropping it would leave the caller unable to see what the model
  // was actually answering when it went wrong.
  auto session = Session::open(options_with(65536, 4096), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user(text_of(20000));
  ASSERT_TRUE(session->prepare().has_value());

  ASSERT_FALSE(session->record(reply_with("looks fine", 64)).has_value());
  EXPECT_EQ(session->turns().back().message.content, "looks fine");
  EXPECT_EQ(session->completed_turns(), 1u);
}

TEST(SessionRecord, DoesNotMistakeOrdinaryOverEstimationForADiscard) {
  // English prose measures near 5.9 chars/token against the 2.0 assumed, so a healthy
  // reply routinely evaluates about a third of the estimate. That must not fire.
  auto session = Session::open(options_with(65536, 4096), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user(text_of(20000));
  const auto request = session->prepare();
  ASSERT_TRUE(request.has_value());

  const std::uint64_t estimated = session->estimated_prompt_tokens();
  EXPECT_TRUE(session->record(reply_with("fine", estimated / 3)).has_value());
}

TEST(SessionRecord, DoesNotCalibrateOnATruncatedPrompt) {
  // A truncated prompt_tokens describes a prompt this session did not send. Tightening
  // the ratio on it would be calibrating against a measurement of something else.
  auto session = Session::open(options_with(65536, 4096), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user(text_of(20000));
  ASSERT_TRUE(session->prepare().has_value());
  const double before = session->chars_per_token();

  ASSERT_FALSE(session->record(reply_with("looks fine", 64)).has_value());
  EXPECT_DOUBLE_EQ(session->chars_per_token(), before);
}

// --- calibration -------------------------------------------------------------

TEST(SessionCalibrate, TightensWhenTheEstimateWasTooGenerous) {
  // 1000 chars measured at 1000 tokens is about 1.03 chars/token, well under the 2.0
  // assumed, so the assumption was optimistic and must come down.
  const double revised = calibrate(2.0, 1000, 2, 1000);
  EXPECT_LT(revised, 2.0);
  EXPECT_GT(revised, 0.5);
}

TEST(SessionCalibrate, NeverLoosensOnAGenerousObservation) {
  // One prose-heavy turn must not raise the ratio just in time for the next turn to
  // carry a file full of paths.
  EXPECT_DOUBLE_EQ(calibrate(2.0, 1000, 2, 100), 2.0);
}

TEST(SessionCalibrate, HasAFloorSoOneOddSampleCannotDriveItToAbsurdity) {
  EXPECT_GE(calibrate(2.0, 100, 0, 100000), 0.5);
}

TEST(SessionCalibrate, IgnoresAMeasurementSwallowedEntirelyByTheOverheadAssumption) {
  EXPECT_DOUBLE_EQ(calibrate(2.0, 1000, 10, kPerMessageOverhead * 10), 2.0);
  EXPECT_DOUBLE_EQ(calibrate(2.0, 0, 2, 500), 2.0);
}

TEST(SessionCalibrate, ATightenedRatioRaisesTheEstimateForUnseenContent) {
  auto session = Session::open(options_with(65536, 4096), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user(text_of(2000));
  ASSERT_TRUE(session->prepare().has_value());

  // 2000 chars that actually cost 1800 tokens: about 1.1 chars/token, well under the
  // 2.0 assumed, so the assumption must come down.
  ASSERT_TRUE(session->record(reply_with("", 1800)).has_value());
  const double tightened = session->chars_per_token();
  ASSERT_LT(tightened, 2.0);

  // The tightened ratio governs the *next* message, which is the one nothing has
  // measured yet.
  const std::uint64_t before = session->estimated_prompt_tokens();
  session->add_user(text_of(1000));
  const std::uint64_t added = session->estimated_prompt_tokens() - before;
  const auto at_initial_ratio =
      static_cast<std::uint64_t>(std::ceil(1000.0 / kInitialCharsPerToken)) + kPerMessageOverhead;
  EXPECT_GT(added, at_initial_ratio);
}

// --- attribution: guessing only about what has never been sent ---------------

TEST(SessionAttribution, PricesTheSentTurnsFromTheReplyRatherThanLeavingThemGuessed) {
  auto session = small_session();
  session.add_user(text_of(200));
  ASSERT_TRUE(session.prepare().has_value());
  EXPECT_EQ(session.measured_turns(), 0u);

  ASSERT_TRUE(session.record(reply_with("ok", 90)).has_value());
  // System and user were both in the prompt the reply priced; the assistant reply was
  // not, so it stays on its estimate.
  EXPECT_EQ(session.measured_turns(), 2u);
  EXPECT_TRUE(session.turns()[0].measured_tokens.has_value());
  EXPECT_TRUE(session.turns()[1].measured_tokens.has_value());
  EXPECT_FALSE(session.turns()[2].measured_tokens.has_value());
}

TEST(SessionAttribution, NeverCreditsTheReplyItselfWithPromptTokens) {
  // The measurement covers the prompt. Sharing it across the answer the model produced
  // afterwards would price a message the server never saw.
  auto session = small_session();
  session.add_user("short");
  ASSERT_TRUE(session.prepare().has_value());
  ASSERT_TRUE(session.record(reply_with(text_of(400), 40)).has_value());

  const auto& assistant = session.turns().back();
  ASSERT_EQ(assistant.message.role, "assistant");
  EXPECT_FALSE(assistant.measured_tokens.has_value());
}

TEST(SessionAttribution, TheAccountingConvergesOnWhatTheServerActuallyCharged) {
  // The behaviour the live harness was built to show: before attribution a session
  // over-estimated 935 against a measured 268 and dropped five turns it could have kept.
  auto session = Session::open(options_with(65536, 4096), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user(text_of(4000));
  ASSERT_TRUE(session->prepare().has_value());
  const std::uint64_t guessed = session->estimated_prompt_tokens();

  ASSERT_TRUE(session->record(reply_with("", 700)).has_value());
  // Only the assistant reply is still a guess, and it is empty.
  EXPECT_LT(session->estimated_prompt_tokens(), guessed / 2);
  EXPECT_GE(session->estimated_prompt_tokens(), 700u);
}

TEST(SessionAttribution, AnAlreadyPricedTurnKeepsItsFigureAcrossLaterTurns) {
  auto session = small_session();
  session.add_user("first");
  ASSERT_TRUE(session.prepare().has_value());
  ASSERT_TRUE(session.record(reply_with("one", 60)).has_value());
  const std::uint64_t system_cost = *session.turns()[0].measured_tokens;

  session.add_user("second");
  ASSERT_TRUE(session.prepare().has_value());
  ASSERT_TRUE(session.record(reply_with("two", 140)).has_value());
  // Its content has not changed, so neither has what it costs.
  EXPECT_EQ(*session.turns()[0].measured_tokens, system_cost);
}

TEST(SessionAttribution, LeavesTurnsGuessedWhenTheMeasurementIsAlreadySpent) {
  // A measurement no larger than what earlier turns were already priced at leaves
  // nothing to share out. Falling back to the pessimistic estimate is the safe reading.
  auto session = small_session();
  session.add_user("first");
  ASSERT_TRUE(session.prepare().has_value());
  ASSERT_TRUE(session.record(reply_with("one", 300)).has_value());

  session.add_user(text_of(100));
  ASSERT_TRUE(session.prepare().has_value());
  ASSERT_TRUE(session.record(reply_with("two", 300)).has_value());
  EXPECT_FALSE(session.turns()[2].measured_tokens.has_value());
}

TEST(SessionAttribution, DoesNotPriceTurnsFromATruncatedPrompt) {
  auto session = Session::open(options_with(65536, 4096), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user(text_of(20000));
  ASSERT_TRUE(session->prepare().has_value());

  ASSERT_FALSE(session->record(reply_with("looks fine", 64)).has_value());
  EXPECT_EQ(session->measured_turns(), 0u);
}

TEST(SessionAttribution, PricesTheReplyWithTheRatioAsCalibrationLeavesIt) {
  // The reply is appended after calibration, so it must be priced at the tightened
  // ratio. Pricing it first left a stale, too-low figure that re_estimate() could not
  // reach -- it walks the history, which the reply had not joined yet -- and the next
  // prepare() then admitted it against the budget. Under-count, unsafe direction, and
  // it landed on the very first turn, which is when the big calibration jump happens.
  auto session = Session::open(options_with(65536, 4096), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user(text_of(2000));
  ASSERT_TRUE(session->prepare().has_value());

  const double before = session->chars_per_token();
  ASSERT_TRUE(session->record(reply_with(text_of(2000), 1800)).has_value());
  const double after = session->chars_per_token();
  ASSERT_LT(after, before) << "this test is meaningless unless the ratio actually moved";

  const auto stale = static_cast<std::uint64_t>(std::ceil(2000.0 / before));
  const auto correct = static_cast<std::uint64_t>(std::ceil(2000.0 / after));
  EXPECT_EQ(session->turns().back().estimated_tokens, correct);
  EXPECT_GT(correct, stale);
}

TEST(SessionAttribution, IgnoresAPromptCountLargerThanTheWindowItWasSentIn) {
  // num_ctx is exactly what bounds prompt_eval_count, so a bigger number is a daemon
  // this code cannot reason about. Left unbounded it drives calibration to its floor
  // and reaches share_out, where a value near the limit of the type stops being safe.
  auto session = Session::open(options_with(8192, 1024), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user(text_of(2000));
  ASSERT_TRUE(session->prepare().has_value());

  ASSERT_TRUE(session->record(reply_with("ok", std::numeric_limits<std::uint64_t>::max()))
                  .has_value());
  EXPECT_GE(session->chars_per_token(), 0.5);
  for (const auto& turn : session->turns()) {
    EXPECT_LE(turn.cost(), session->window() + kPerMessageOverhead);
  }
}

TEST(SessionShareOut, DividesInProportionToWeight) {
  const std::vector<std::size_t> weights = {100, 300};
  const auto shares = hermit::supervisor::share_out(weights, 400);
  ASSERT_EQ(shares.size(), 2u);
  EXPECT_EQ(shares[0], 100u);
  EXPECT_EQ(shares[1], 300u);
}

TEST(SessionShareOut, RoundsUpSoALongHistoryDoesNotBleedTokens) {
  const std::vector<std::size_t> weights = {1, 1, 1};
  const auto shares = hermit::supervisor::share_out(weights, 10);
  ASSERT_EQ(shares.size(), 3u);
  std::uint64_t total = 0;
  for (const std::uint64_t s : shares) total += s;
  EXPECT_GE(total, 10u);
}

TEST(SessionShareOut, GivesUpRatherThanDividingByNothing) {
  const std::vector<std::size_t> none;
  EXPECT_TRUE(hermit::supervisor::share_out(none, 100).empty());

  const std::vector<std::size_t> weightless = {0, 0};
  EXPECT_TRUE(hermit::supervisor::share_out(weightless, 100).empty());

  const std::vector<std::size_t> weights = {5, 5};
  EXPECT_TRUE(hermit::supervisor::share_out(weights, 0).empty());
}

// --- the truncation predicate ------------------------------------------------

TEST(SessionTruncationPredicate, FiresOnTheCollapseThatWasActuallyMeasured) {
  EXPECT_TRUE(looks_truncated(9005, 64));
  EXPECT_TRUE(looks_truncated(9018, 70));
}

TEST(SessionTruncationPredicate, StaysQuietOnShortPromptsWhereTheRatioIsNoise) {
  EXPECT_FALSE(looks_truncated(100, 1));
}

TEST(SessionTruncationPredicate, StaysQuietWithinTheRangeOverEstimationCanProduce) {
  EXPECT_FALSE(looks_truncated(1000, 500));
  EXPECT_FALSE(looks_truncated(1000, 334));  // the prose case, about a third
}

TEST(SessionTruncationPredicate, DoesNotWrapOnALargeMeasurement) {
  // Phrased as multiplication this would overflow and report a truncation that did not
  // happen, on an unsigned type, silently.
  EXPECT_FALSE(looks_truncated(1000, std::numeric_limits<std::uint64_t>::max()));
}

}  // namespace

// --- tool results in history (Phase 2) ---------------------------------------
//
// DECISIONS.md recorded this hazard under "Still open" before there were tool messages
// to trigger it: the trim loop erased one turn at a time, so it could drop a result and
// keep its call, or the reverse. The second is the damaging one -- a call that looks
// outstanding gets re-issued, which is the repeat-call loop the supervisor exists to
// break. These tests are what closes that entry.

namespace {

hermit::ollama::ChatReply reply_with_call(std::string name, std::uint64_t prompt_tokens) {
  hermit::ollama::ChatReply reply;
  reply.finish_reason = "stop";
  reply.prompt_tokens = prompt_tokens;
  reply.completion_tokens = 20;
  hermit::ollama::ToolCall call;
  call.id = "call_1";
  call.name = std::move(name);
  call.arguments = nlohmann::json{{"path", "a.txt"}};
  reply.tool_calls.push_back(std::move(call));
  return reply;
}

/// Roles in history, in order -- the shape assertions below are all about order.
std::vector<std::string> roles_of(const Session& session) {
  std::vector<std::string> roles;
  for (const auto& turn : session.turns()) roles.push_back(turn.message.role);
  return roles;
}

}  // namespace

TEST(SessionToolHistory, AnAssistantTurnKeepsTheCallsItMade) {
  // Without this the result that follows has no visible request, and the model is
  // answering a question it cannot see.
  auto session = Session::open(options_with(8192, 1024), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user("read a.txt");
  ASSERT_TRUE(session->prepare().has_value());
  ASSERT_TRUE(session->record(reply_with_call("read", 100)).has_value());

  const auto& turns = session->turns();
  ASSERT_FALSE(turns.empty());
  const auto& assistant = turns.back();
  EXPECT_EQ(assistant.message.role, "assistant");
  ASSERT_EQ(assistant.message.tool_calls.size(), 1u);
  EXPECT_EQ(assistant.message.tool_calls[0].name, "read");
  EXPECT_EQ(assistant.message.tool_calls[0].arguments["path"], "a.txt");
}

TEST(SessionToolHistory, AToolResultLandsAsAToolRoleCarryingItsName) {
  auto session = Session::open(options_with(8192, 1024), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user("read a.txt");
  ASSERT_TRUE(session->prepare().has_value());
  ASSERT_TRUE(session->record(reply_with_call("read", 100)).has_value());
  session->add_tool_result("read", R"([{"path":"a.txt","content":"aaaa"}])");

  EXPECT_EQ(roles_of(*session),
            (std::vector<std::string>{"system", "user", "assistant", "tool"}));
  EXPECT_EQ(session->turns().back().message.tool_name, "read");
}

namespace {

/// How many turns carry each shape, so a test can refuse to pass vacuously.
struct Shape {
  std::size_t tool_results = 0;
  std::size_t calls_bearing = 0;
};

Shape shape_of(const Session& session) {
  Shape shape;
  for (const auto& turn : session.turns()) {
    if (turn.message.role == "tool") ++shape.tool_results;
    if (!turn.message.tool_calls.empty()) ++shape.calls_bearing;
  }
  return shape;
}

/// Two complete exchanges plus a fresh instruction, sized so the trim gives up the FIRST
/// call-and-results group while the second survives.
///
/// The surviving group is the entire point, and the reason this helper exists. An earlier
/// version of both tests below built a single exchange and let the trim take all of it,
/// leaving history as [system, user] -- so their invariant loops, each gated on finding a
/// `tool` role, ran zero iterations and asserted nothing. Both passed while the grouping
/// they were written to protect was disabled. The guards in the tests are what make that
/// unrepeatable; this shape is what gives them something to check.
///
/// The growing `prompt_tokens` are deliberate: a constant figure re-anchors the whole
/// history to it every turn, and one large against a tiny prompt drives chars_per_token to
/// its floor. Either makes the trim never fire.
void build_two_exchanges(Session& session) {
  session.add_user("first instruction");
  ASSERT_TRUE(session.prepare().has_value());
  ASSERT_TRUE(session.record(reply_with_call("read", 300)).has_value());
  session.add_tool_result("read", std::string(1200, 'x'));

  session.add_user("second instruction");
  ASSERT_TRUE(session.prepare().has_value());
  ASSERT_TRUE(session.record(reply_with_call("read", 1800)).has_value());
  session.add_tool_result("read", std::string(1200, 'y'));

  session.add_user("third instruction");
}

}  // namespace

TEST(SessionToolHistory, ATrimDropsACallAndItsResultsTogether) {
  // The invariant: history never contains a `tool` turn whose call is absent, nor an
  // assistant call with its results stripped.
  auto session = Session::open(options_with(4096, 256), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  build_two_exchanges(*session);

  const auto request = session->prepare();
  ASSERT_TRUE(request.has_value()) << request.error().message();
  ASSERT_GT(session->dropped(), 0u) << "nothing was trimmed, so nothing here is tested";

  // The guard that makes this test mean something. Without it the assertions below sit
  // behind a `continue` that a fully-trimmed history skips entirely, and the test passes
  // while the grouping is broken -- which is exactly what it did before.
  const auto shape = shape_of(*session);
  ASSERT_GT(shape.tool_results, 0u)
      << "no tool result survived the trim: the loop below would check nothing";

  const auto roles = roles_of(*session);
  for (std::size_t i = 0; i < roles.size(); ++i) {
    if (roles[i] != "tool") continue;
    // Walk back over any sibling results to the turn that must be the call.
    std::size_t owner = i;
    while (owner > 0 && roles[owner - 1] == "tool") --owner;
    ASSERT_GT(owner, 0u) << "a tool result begins the history: its call was dropped";
    EXPECT_EQ(roles[owner - 1], "assistant")
        << "a tool result is not preceded by the assistant turn that called it";
    EXPECT_FALSE(session->turns()[owner - 1].message.tool_calls.empty())
        << "the preceding assistant turn carries no calls";
  }
}

TEST(SessionToolHistory, TheGroupedDropNeverLeavesAnAssistantCallWithoutItsResults) {
  auto session = Session::open(options_with(4096, 256), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  build_two_exchanges(*session);
  ASSERT_TRUE(session->prepare().has_value());
  ASSERT_GT(session->dropped(), 0u) << "nothing was trimmed, so nothing here is tested";

  const auto shape = shape_of(*session);
  ASSERT_GT(shape.calls_bearing, 0u)
      << "no call-bearing turn survived the trim: the loop below would check nothing";

  // Whatever survived, no assistant turn carrying calls may be the final turn with its
  // results gone -- that is the half the model responds to by re-issuing the call.
  const auto roles = roles_of(*session);
  const auto& turns = session->turns();
  for (std::size_t i = 0; i < turns.size(); ++i) {
    if (turns[i].message.tool_calls.empty()) continue;
    ASSERT_LT(i + 1, roles.size())
        << "an assistant turn with calls is last: its results were dropped";
    EXPECT_EQ(roles[i + 1], "tool")
        << "an assistant turn with calls is not followed by its results";
  }
}

TEST(SessionToolHistory, TheTrimCannotStopBetweenACallAndItsResult) {
  // The test that actually distinguishes grouping from not, and the earlier attempts did
  // not. Dropping from the front means erasing an assistant turn and then its result
  // one-at-a-time usually lands on the same history as erasing them together -- so a
  // mutation disabling grouping survived a suite that already had six tests around it.
  //
  // The distinguishing case is narrow: the trim must have room to stop *between* the call
  // and its result, which needs the assistant turn to be the expensive one and the result
  // to be cheap. Then:
  //   grouped   -> both go, history is [system, user]
  //   ungrouped -> only the assistant goes, leaving [system, tool, user] -- a result whose
  //                request is gone, which is the orphan half of the hazard.
  // Asserting the exact surviving sequence is what makes that observable; a loop gated on
  // finding a `tool` role cannot see it, because in the correct case there is none.
  auto session = Session::open(options_with(2048, 256), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());

  session->add_user("go");
  ASSERT_TRUE(session->prepare().has_value());

  // A large assistant turn that also made a call, and a cheap result after it.
  auto reply = reply_with_call("read", 200);
  reply.content = std::string(2200, 'a');
  ASSERT_TRUE(session->record(reply).has_value());
  session->add_tool_result("read", std::string(100, 'r'));

  session->add_user("next");
  const auto request = session->prepare();
  ASSERT_TRUE(request.has_value()) << request.error().message();
  ASSERT_GT(session->dropped(), 0u) << "nothing was trimmed, so nothing here is tested";

  EXPECT_EQ(roles_of(*session), (std::vector<std::string>{"system", "user"}))
      << "a result outlived the call that produced it";
  for (const auto& turn : session->turns()) {
    EXPECT_NE(turn.message.role, "tool") << "orphaned tool result left in history";
  }
}

TEST(SessionToolHistory, DroppedCountsTurnsNotGroups) {
  // Documented in prepare()'s header: a session that gave up one exchange of three
  // messages reports three, so the number stays comparable with a tool-free session.
  auto session = Session::open(options_with(700, 100), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());

  session->add_user("first instruction");
  ASSERT_TRUE(session->prepare().has_value());
  ASSERT_TRUE(session->record(reply_with_call("read", 60)).has_value());
  session->add_tool_result("read", std::string(400, 'x'));
  session->add_tool_result("read", std::string(400, 'y'));
  session->add_user("second instruction");
  ASSERT_TRUE(session->prepare().has_value());

  // user + assistant + two results = at least the three of the tool group.
  EXPECT_GE(session->dropped(), 3u);
}

TEST(SessionToolHistory, AnOversizedToolResultIsDroppedWithItsCallRatherThanRefused) {
  // Worth pinning precisely, because the intuitive answer is wrong. `MessageTooLarge`
  // cannot fire for a tool result: pin_latest_user pins only `system` and the latest
  // `user`, so a result is *always* droppable, and the trim therefore gives up the whole
  // group instead of refusing the prompt.
  //
  // That is coherent history -- no orphan either way -- but it is not a good outcome,
  // and the reason is worth stating where it is visible: the model's call and its answer
  // both vanish, so the model re-issues the same read, gets the same oversized result,
  // and the session makes no progress until its turn bound stops it. Two facts explain
  // how a result gets this big at all: `read`'s cap is a filesystem-safety limit (16 MB)
  // with no relation to a context window, and nothing in `core` knows what window it is
  // being read into.
  //
  // The loop is where that is handled -- it substitutes a refusal naming the size before
  // a result this large ever reaches history (see loop.h). This test fixes the Session
  // half of the contract so that change cannot silently regress it.
  auto session = Session::open(options_with(700, 100), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user("read the big one");
  ASSERT_TRUE(session->prepare().has_value());
  ASSERT_TRUE(session->record(reply_with_call("read", 60)).has_value());
  session->add_tool_result("read", std::string(100000, 'x'));

  const auto request = session->prepare();
  ASSERT_TRUE(request.has_value()) << request.error().message();
  EXPECT_GE(session->dropped(), 2u);  // the assistant call and its result, together

  const auto roles = roles_of(*session);
  EXPECT_EQ(std::count(roles.begin(), roles.end(), "tool"), 0);
  for (const auto& turn : session->turns()) EXPECT_TRUE(turn.message.tool_calls.empty());
}

TEST(SessionToolHistory, AddingAToolResultGivesUpAnOutstandingRequest) {
  // Same guarantee add_user carries: anything appended between prepare() and record()
  // would reorder the conversation, so record() refuses rather than checking against a
  // history it no longer describes.
  auto session = Session::open(options_with(8192, 1024), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user("read a.txt");
  ASSERT_TRUE(session->prepare().has_value());
  session->add_tool_result("read", "[]");

  const auto recorded = session->record(reply_with_call("read", 100));
  ASSERT_FALSE(recorded.has_value());
  EXPECT_EQ(recorded.error().kind, hermit::supervisor::SessionError::NoRequestOutstanding);
}

TEST(SessionToolHistory, ThePreparedRequestCarriesToolMessagesThroughToTheWire) {
  auto session = Session::open(options_with(8192, 1024), client_with(65536), "sys");
  ASSERT_TRUE(session.has_value());
  session->add_user("read a.txt");
  ASSERT_TRUE(session->prepare().has_value());
  ASSERT_TRUE(session->record(reply_with_call("read", 100)).has_value());
  session->add_tool_result("read", R"([{"path":"a.txt"}])");

  const auto request = session->prepare();
  ASSERT_TRUE(request.has_value());
  ASSERT_EQ(request->messages.size(), 4u);
  EXPECT_EQ(request->messages[2].role, "assistant");
  EXPECT_EQ(request->messages[2].tool_calls.size(), 1u);
  EXPECT_EQ(request->messages[3].role, "tool");
  EXPECT_EQ(request->messages[3].tool_name, "read");
}
