// The agent loop's policy, exercised without a daemon.
//
// Split the way the loop is: `dispatch_call` is pure over a registry and a sandbox, so
// the whole refusal vocabulary is reachable here; the bounds are reachable because they
// are checked before any request goes out. What is *not* covered offline is a turn that
// needs a reply -- Ollama's client is concrete by design (D7 keeps HTTP behind a pimpl in
// one target) -- so the multi-turn behaviour was verified against live models instead,
// and what those runs showed is recorded in loop.h and D12.

#include <hermes/supervisor/loop.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/app/toolset.h>

namespace fs = std::filesystem;
using hermes::Sandbox;
using hermes::supervisor::AgentLoop;
using hermes::supervisor::dispatch_call;
using hermes::supervisor::LoopOptions;
using hermes::supervisor::oversized_refusal;
using hermes::supervisor::result_is_hopeless;
using hermes::supervisor::Session;
using hermes::supervisor::SessionOptions;
using hermes::supervisor::StopReason;
using json = nlohmann::json;

namespace {

hermes::ollama::ToolCall call_of(std::string name, json arguments) {
  hermes::ollama::ToolCall call;
  call.id = "call_1";
  call.name = std::move(name);
  call.arguments = std::move(arguments);
  return call;
}

class LoopFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermes_loop_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::path{buf.data()};
    fs::create_directories(tmp_ / "root");

    std::ofstream{tmp_ / "root" / "notes.txt"} << "alpha\nbeta\n";

    auto box = Sandbox::open(tmp_ / "root");
    ASSERT_TRUE(box.has_value());
    sandbox_ = std::make_unique<Sandbox>(std::move(*box));

    auto tools = hermes::app::ToolSet::tier0(tmp_ / "backups");
    ASSERT_TRUE(tools.has_value());
    tools_ = std::make_unique<hermes::app::ToolSet>(std::move(*tools));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  hermes::supervisor::Dispatched run_call(std::string name, json arguments) {
    return dispatch_call(tools_->registry(), *sandbox_, call_of(std::move(name),
                                                                std::move(arguments)));
  }

  fs::path tmp_;
  std::unique_ptr<Sandbox> sandbox_;
  std::unique_ptr<hermes::app::ToolSet> tools_;
};

}  // namespace

// --- composition -------------------------------------------------------------

TEST_F(LoopFixture, TheTierZeroSetIsTheEightToolsInTheDecidedOrder) {
  // ROUTING.md section 4's list, observe before mutate. `shell` is deliberately absent:
  // it waits on D7's gate. The order is asserted because it fixes the prompt bytes.
  const auto tools = tools_->registry().tools();
  std::vector<std::string> names;
  for (const auto& tool : tools) names.emplace_back(tool->spec().name);

  EXPECT_EQ(names, (std::vector<std::string>{"read", "hash", "list", "find", "grep",
                                             "write", "edit", "move"}));
}

// --- dispatch: every refusal is a value the model can read -------------------

TEST_F(LoopFixture, DispatchesASuccessfulCallAsRowsRatherThanAnError) {
  // `read` declares `paths`, a PathList -- one or more. The generated schema is what
  // tells the model that, and the live runs confirm it does: the model sent `paths`.
  const auto result = run_call("read", json{{"paths", json::array({"notes.txt"})}});
  EXPECT_FALSE(result.refused) << result.content;

  const json parsed = json::parse(result.content);
  ASSERT_TRUE(parsed.is_array());
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0]["content"], "alpha\nbeta\n");
}

TEST_F(LoopFixture, RefusesAToolThisFrontendDoesNotExpose) {
  // `shell` is the case that matters: it exists in ROUTING.md section 4 and is
  // deliberately not registered, so this is the refusal a model asking for it gets.
  const auto result = run_call("shell", json{{"command", "rm -rf /"}});
  ASSERT_TRUE(result.refused);
  const json parsed = json::parse(result.content);
  EXPECT_NE(parsed["error"].get<std::string>().find("shell"), std::string::npos);
  EXPECT_NE(parsed["error"].get<std::string>().find("not in the tool list"),
            std::string::npos);
}

TEST_F(LoopFixture, RefusesArgumentsThatWillNotDecode) {
  const auto result = run_call("read", json{{"paths", json::array({42})}});
  ASSERT_TRUE(result.refused);
  const auto error = json::parse(result.content)["error"].get<std::string>();
  EXPECT_NE(error.find("read"), std::string::npos);
  EXPECT_NE(error.find("paths"), std::string::npos);
}

TEST_F(LoopFixture, RefusesTheSchemaLeakageAsAnUnknownArgument) {
  // Closes the D12 loop: `tools` + `format` made llama3.2-3b send the format object as
  // the arguments, `tool` key and all. It decodes fine -- these are strings -- and is
  // refused here, by the declaration, naming the offending argument.
  const auto result = run_call(
      "write", json{{"tool", "write"}, {"path", "x.txt"}, {"content", "y"}});
  ASSERT_TRUE(result.refused);
  const auto error = json::parse(result.content)["error"].get<std::string>();
  EXPECT_NE(error.find("tool"), std::string::npos);
}

TEST_F(LoopFixture, RefusesAMissingRequiredArgument) {
  const auto result = run_call("read", json::object());
  ASSERT_TRUE(result.refused);
  EXPECT_TRUE(json::parse(result.content).contains("error"));
}

TEST_F(LoopFixture, RefusesAnEmptyPathListBecauseZeroIsNotOneOrMore) {
  const auto result = run_call("read", json{{"paths", json::array()}});
  ASSERT_TRUE(result.refused);
  EXPECT_TRUE(json::parse(result.content).contains("error"));
}

TEST_F(LoopFixture, RefusesAPathOutsideTheRootWithTheSandboxsOwnReason) {
  // R1 is structural: parse_args is the only place a Path becomes a SandboxPath, so a
  // tool cannot be reached with a path outside the root even when the model asks.
  const auto result = run_call("list", json{{"path", "../outside"}});
  ASSERT_TRUE(result.refused);
  const auto error = json::parse(result.content)["error"].get<std::string>();
  EXPECT_NE(error.find("sandbox"), std::string::npos);
}

TEST_F(LoopFixture, RefusesTheToolsOwnFailureAsOneLine) {
  const auto result = run_call("read", json{{"paths", json::array({"no-such-file.txt"})}});
  ASSERT_TRUE(result.refused);
  const auto error = json::parse(result.content)["error"].get<std::string>();
  EXPECT_NE(error.find("read"), std::string::npos);
  EXPECT_EQ(error.find('\n'), std::string::npos) << "a refusal is one line";
}

TEST_F(LoopFixture, ARefusalIsAlwaysAnObjectAndASuccessAlwaysAnArray) {
  // The loop's whole error vocabulary distinguishable by shape, not by reading keys.
  EXPECT_TRUE(json::parse(run_call("read", json{{"paths", json::array({"notes.txt"})}}).content)
                  .is_array());
  for (const auto& bad : {json{{"paths", json::array({"no-such-file.txt"})}},
                          json{{"paths", json::array({1})}}, json::object(),
                          json{{"path", "notes.txt"}}}) {
    EXPECT_TRUE(json::parse(run_call("read", bad).content).is_object());
  }
}

// --- the bounds --------------------------------------------------------------

namespace {

/// A client that will never answer: nothing is listening on this port. Enough to reach
/// the bound checks, which happen before any request goes out.
hermes::ollama::ClientOptions dead_client() {
  hermes::ollama::ClientOptions options;
  options.base_url = "http://127.0.0.1:1";
  options.max_num_ctx = 8192;
  options.connect_timeout = std::chrono::seconds{1};
  options.chat_timeout = std::chrono::seconds{1};
  return options;
}

SessionOptions session_options() {
  SessionOptions options;
  options.model = "test-model";
  options.num_ctx = 8192;
  options.reply_reserve = 1024;
  options.max_tokens = 1024;
  return options;
}

}  // namespace

TEST_F(LoopFixture, AZeroTurnBoundStopsBeforeAnyRequestGoesOut) {
  const auto client = hermes::ollama::Client::open(dead_client());
  ASSERT_TRUE(client.has_value());
  auto session = Session::open(session_options(), *client, "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{*client, tools_->registry(), *sandbox_, LoopOptions{.max_turns = 0}};
  const auto outcome = loop.run(*session, "do something");

  EXPECT_EQ(outcome.reason, StopReason::TurnBudget);
  EXPECT_EQ(outcome.turns, 0u);
  EXPECT_EQ(outcome.calls, 0u);
  EXPECT_NE(outcome.detail.find("0-turn"), std::string::npos);
}

TEST_F(LoopFixture, AZeroWallClockBudgetStopsBeforeTheTurnBoundIsConsidered) {
  // R8's point: the clock is the bound that matters, so it is checked first.
  const auto client = hermes::ollama::Client::open(dead_client());
  ASSERT_TRUE(client.has_value());
  auto session = Session::open(session_options(), *client, "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{*client, tools_->registry(), *sandbox_,
                 LoopOptions{.max_turns = 10, .budget = std::chrono::seconds{0}}};
  const auto outcome = loop.run(*session, "do something");

  EXPECT_EQ(outcome.reason, StopReason::TimeBudget);
  EXPECT_EQ(outcome.turns, 0u);
}

TEST_F(LoopFixture, AnUnreachableDaemonStopsAsTransportWithoutRetrying) {
  const auto client = hermes::ollama::Client::open(dead_client());
  ASSERT_TRUE(client.has_value());
  auto session = Session::open(session_options(), *client, "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{*client, tools_->registry(), *sandbox_, LoopOptions{}};
  const auto outcome = loop.run(*session, "do something");

  EXPECT_EQ(outcome.reason, StopReason::Transport);
  EXPECT_EQ(outcome.turns, 0u) << "a turn that never got a reply is not a completed turn";
  EXPECT_FALSE(outcome.ran_to_completion());
}

TEST_F(LoopFixture, TheInstructionReachesHistoryEvenWhenTheFirstTurnFails) {
  const auto client = hermes::ollama::Client::open(dead_client());
  ASSERT_TRUE(client.has_value());
  auto session = Session::open(session_options(), *client, "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{*client, tools_->registry(), *sandbox_, LoopOptions{}};
  (void)loop.run(*session, "the instruction");

  ASSERT_EQ(session->turns().size(), 2u);
  EXPECT_EQ(session->turns()[1].message.role, "user");
  EXPECT_EQ(session->turns()[1].message.content, "the instruction");
}

TEST_F(LoopFixture, OffersEveryRegisteredToolAsADefinition) {
  const auto client = hermes::ollama::Client::open(dead_client());
  ASSERT_TRUE(client.has_value());
  AgentLoop loop{*client, tools_->registry(), *sandbox_, LoopOptions{}};

  ASSERT_EQ(loop.definitions().size(), 8u);
  EXPECT_EQ(loop.definitions()[0]["function"]["name"], "read");
  EXPECT_EQ(loop.definitions()[7]["function"]["name"], "move");
}

// --- the oversized-result guard ----------------------------------------------

TEST_F(LoopFixture, AResultLargerThanTheWholePromptBudgetIsHopeless) {
  const auto client = hermes::ollama::Client::open(dead_client());
  ASSERT_TRUE(client.has_value());
  auto session = Session::open(session_options(), *client, "sys");
  ASSERT_TRUE(session.has_value());

  // prompt_budget is 8192 - 1024 = 7168 tokens; at the initial 1.3 chars/token that is
  // roughly 9300 characters. Well over it, and well under.
  EXPECT_TRUE(result_is_hopeless(*session, 200000));
  EXPECT_FALSE(result_is_hopeless(*session, 100));
}

TEST_F(LoopFixture, AResultMerelyLargeIsTrimmedAroundRatherThanRefused) {
  // The boundary that matters: only the case nothing can be dropped to fix is refused.
  // A large-but-fitting result is the Session's normal business.
  const auto client = hermes::ollama::Client::open(dead_client());
  ASSERT_TRUE(client.has_value());
  auto session = Session::open(session_options(), *client, "sys");
  ASSERT_TRUE(session.has_value());

  const auto budget_chars =
      static_cast<std::size_t>(static_cast<double>(session->prompt_budget()) *
                               session->chars_per_token());
  EXPECT_FALSE(result_is_hopeless(*session, budget_chars / 2));
  EXPECT_TRUE(result_is_hopeless(*session, budget_chars * 2));
}

TEST(LoopRefusal, TheOversizedRefusalNamesTheToolTheSizeAndTheBudget) {
  const auto refusal = oversized_refusal("read", 250000, 7168);
  EXPECT_NE(refusal.find("read"), std::string::npos);
  EXPECT_NE(refusal.find("250000"), std::string::npos);
  EXPECT_NE(refusal.find("7168"), std::string::npos);
  EXPECT_NE(refusal.find("ask for less"), std::string::npos);
}

TEST(LoopStopReason, EveryReasonHasItsOwnSentence) {
  const std::array reasons{StopReason::Answered,       StopReason::TurnBudget,
                           StopReason::TimeBudget,     StopReason::Stalled,
                           StopReason::SessionRefused, StopReason::Transport};
  std::vector<std::string> sentences;
  for (const auto reason : reasons) {
    const std::string text{hermes::supervisor::to_string(reason)};
    EXPECT_FALSE(text.empty());
    EXPECT_EQ(text.find("unknown"), std::string::npos);
    sentences.push_back(text);
  }
  std::sort(sentences.begin(), sentences.end());
  EXPECT_EQ(std::unique(sentences.begin(), sentences.end()), sentences.end());
}

// --- multi-turn policy, now reachable offline via the ChatFn seam -------------
//
// Everything below was verified only by hand against a live model until `ChatFn` existed.
// A mutation-testing pass found the gap: nine invariants were mutated and two survived, one
// of them the grouped-drop fix DECISIONS.md records as closed. These are the tests that
// close the reachable half of that.

namespace {

using hermes::ollama::ChatReply;
using hermes::ollama::ChatRequest;
using hermes::ollama::ToolCall;
using hermes::supervisor::ChatFn;
using hermes::supervisor::TurnEvent;

ChatReply text_reply(std::string content, std::uint64_t prompt_tokens = 200) {
  ChatReply reply;
  reply.content = std::move(content);
  reply.finish_reason = "stop";
  reply.prompt_tokens = prompt_tokens;
  reply.completion_tokens = 20;
  return reply;
}

ChatReply call_reply(const std::vector<std::pair<std::string, json>>& calls,
                     std::uint64_t prompt_tokens = 200) {
  ChatReply reply;
  reply.finish_reason = "stop";
  reply.prompt_tokens = prompt_tokens;
  reply.completion_tokens = 30;
  for (std::size_t i = 0; i < calls.size(); ++i) {
    ToolCall call;
    call.id = "call_" + std::to_string(i);
    call.name = calls[i].first;
    call.arguments = calls[i].second;
    reply.tool_calls.push_back(std::move(call));
  }
  return reply;
}

/// A scripted reply source. Held by reference in the ChatFn so the test can read back
/// what the loop sent, which is the only way to check the tool surface actually travels.
struct Script {
  std::vector<ChatReply> replies{};
  std::size_t served = 0;
  std::vector<ChatRequest> seen{};

  ChatFn fn() {
    return [this](const ChatRequest& request) -> hermes::ollama::Result<ChatReply> {
      seen.push_back(request);
      if (served >= replies.size()) {
        return std::unexpected(hermes::ollama::Failure{
            hermes::ollama::TransportError::Unreachable, "script exhausted"});
      }
      return replies[served++];
    };
  }
};

}  // namespace

TEST_F(LoopFixture, RunsSeveralTurnsAndStopsOnTheAnswer) {
  Script script{.replies = {call_reply({{"read", json{{"paths", json::array({"notes.txt"})}}}}),
                            text_reply("notes.txt has two lines.")}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, LoopOptions{}};
  const auto outcome = loop.run(*session, "read notes.txt");

  EXPECT_EQ(outcome.reason, StopReason::Answered);
  EXPECT_EQ(outcome.turns, 2u);
  EXPECT_EQ(outcome.calls, 1u);
  EXPECT_EQ(outcome.refusals, 0u);
  EXPECT_EQ(outcome.final_content, "notes.txt has two lines.");
  // system, user, assistant+call, tool, assistant
  EXPECT_EQ(session->turns().size(), 5u);
}

TEST_F(LoopFixture, OffersTheToolSurfaceOnEveryTurnAndNeverAFormatSchema) {
  // D12 on the loop's own hot path, not just in build_chat_payload's unit test.
  Script script{.replies = {call_reply({{"read", json{{"paths", json::array({"notes.txt"})}}}}),
                            text_reply("done")}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, LoopOptions{}};
  (void)loop.run(*session, "read notes.txt");

  ASSERT_EQ(script.seen.size(), 2u);
  for (const auto& request : script.seen) {
    const auto* tools = std::get_if<ChatRequest::Tools>(&request.constraint);
    ASSERT_NE(tools, nullptr) << "a turn went out without the tool surface";
    EXPECT_EQ(tools->definitions.size(), 8u);
    EXPECT_EQ(std::get_if<ChatRequest::Schema>(&request.constraint), nullptr);
  }
}

TEST_F(LoopFixture, AnEmptyReplyStallsRatherThanCountingAsAnAnswer) {
  ChatReply nothing;
  nothing.finish_reason = "stop";
  nothing.prompt_tokens = 200;
  nothing.completion_tokens = 0;
  Script script{.replies = {nothing}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, LoopOptions{}};
  const auto outcome = loop.run(*session, "do something");

  EXPECT_EQ(outcome.reason, StopReason::Stalled);
  EXPECT_FALSE(outcome.ran_to_completion());
  EXPECT_NE(outcome.detail.find("empty"), std::string::npos);
}

TEST_F(LoopFixture, AThinkingBudgetSpentToExhaustionStallsAndSaysSo) {
  // ChatReply's own warning: a num_predict too small for the reasoning returns empty
  // content with no error. done_reason "length" plus nothing to show is the tell.
  ChatReply exhausted;
  exhausted.finish_reason = "length";
  exhausted.reasoning = std::string(400, 'r');
  exhausted.prompt_tokens = 200;
  exhausted.completion_tokens = 128;
  Script script{.replies = {exhausted}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, LoopOptions{}};
  const auto outcome = loop.run(*session, "do something");

  // It generated reasoning, so `generated()` is true and this is an answer of sorts --
  // an empty one. The loop reports Answered with empty content, which is what the
  // observer's `truncated` flag and `reasoning_chars` exist to disambiguate.
  EXPECT_EQ(outcome.reason, StopReason::Answered);
  EXPECT_TRUE(outcome.final_content.empty());
}

TEST_F(LoopFixture, AnInstructionTooLargeForTheWindowIsRefusedBeforeAnyRequest) {
  Script script;  // never consulted
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, LoopOptions{}};
  const auto outcome = loop.run(*session, std::string(200000, 'x'));

  EXPECT_EQ(outcome.reason, StopReason::SessionRefused);
  EXPECT_EQ(outcome.turns, 0u);
  EXPECT_TRUE(script.seen.empty()) << "prepare() must fail before the request goes out";
}

TEST_F(LoopFixture, ADiscardedPromptStopsTheRunAndStillAnswersItsCalls) {
  // The orphan hazard by a route the trim never sees: record() appends the assistant turn
  // with its calls and THEN reports PromptWasTruncated. Stopping there would leave calls in
  // history with no results -- which a reused session shows the model, and which it
  // answers by re-issuing them.
  auto options = session_options();
  Script script{.replies = {call_reply({{"read", json{{"paths", json::array({"notes.txt"})}}}},
                                       /*prompt_tokens=*/44)}};
  auto session = Session::open(options, dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, LoopOptions{}};
  const auto outcome = loop.run(*session, std::string(4000, 'y'));

  ASSERT_EQ(outcome.reason, StopReason::SessionRefused);
  EXPECT_NE(outcome.detail.find("discarded"), std::string::npos) << outcome.detail;

  // Every call in history must have a result after it.
  const auto& turns = session->turns();
  for (std::size_t i = 0; i < turns.size(); ++i) {
    if (turns[i].message.tool_calls.empty()) continue;
    ASSERT_LT(i + 1, turns.size()) << "a call was left unanswered when the run stopped";
    EXPECT_EQ(turns[i + 1].message.role, "tool");
  }
  EXPECT_EQ(turns.back().message.role, "tool");
}

TEST_F(LoopFixture, CallsPastThePerTurnCapAreRefusedRatherThanDropped) {
  Script script{.replies = {call_reply({{"read", json{{"paths", json::array({"notes.txt"})}}},
                                        {"read", json{{"paths", json::array({"notes.txt"})}}},
                                        {"read", json{{"paths", json::array({"notes.txt"})}}}}),
                            text_reply("done")}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_,
                 LoopOptions{.max_calls_per_turn = 2}};
  const auto outcome = loop.run(*session, "read it three times");

  EXPECT_EQ(outcome.calls, 3u) << "every call the model made must be counted";
  EXPECT_GE(outcome.refusals, 1u);

  // Three calls, three results: a dropped call reads as still outstanding.
  std::size_t results = 0;
  for (const auto& turn : session->turns()) {
    if (turn.message.role == "tool") ++results;
  }
  EXPECT_EQ(results, 3u);
}

TEST_F(LoopFixture, AnOversizedResultIsReplacedByARefusalNamingTheSizeBeforeItReachesHistory) {
  // result_is_hopeless was unit-tested in isolation; this is the substitution actually
  // happening inside run(). A mutation making it always return false survived the suite.
  std::ofstream{tmp_ / "root" / "huge.txt"} << std::string(200000, 'z');
  Script script{.replies = {call_reply({{"read", json{{"paths", json::array({"huge.txt"})}}}}),
                            text_reply("done")}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, LoopOptions{}};
  const auto outcome = loop.run(*session, "read huge.txt");

  EXPECT_GE(outcome.refusals, 1u);
  bool found = false;
  for (const auto& turn : session->turns()) {
    if (turn.message.role != "tool") continue;
    const auto parsed = json::parse(turn.message.content);
    if (!parsed.is_object() || !parsed.contains("error")) continue;
    const auto error = parsed["error"].get<std::string>();
    if (error.find("cannot fit") != std::string::npos) {
      found = true;
      EXPECT_NE(error.find("read"), std::string::npos);
      EXPECT_NE(error.find("ask for less"), std::string::npos);
    }
  }
  EXPECT_TRUE(found) << "the oversized result was not replaced by a refusal";
  // And the 200 KB of content never entered history.
  for (const auto& turn : session->turns()) {
    EXPECT_LT(turn.message.content.size(), 100000u);
  }
}

TEST_F(LoopFixture, TheObserverSeesOneEventPerTurnWithItsCallsAndTokenCounts) {
  std::vector<TurnEvent> events;
  LoopOptions options;
  options.observer = [&events](const TurnEvent& event) { events.push_back(event); };

  Script script{.replies = {call_reply({{"read", json{{"paths", json::array({"notes.txt"})}}}},
                                       /*prompt_tokens=*/321),
                            text_reply("all done", /*prompt_tokens=*/456)}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, std::move(options)};
  (void)loop.run(*session, "read notes.txt");

  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].turn, 1u);
  EXPECT_EQ(events[0].prompt_tokens, 321u);
  ASSERT_EQ(events[0].calls.size(), 1u);
  EXPECT_EQ(events[0].calls[0].tool, "read");
  EXPECT_FALSE(events[0].calls[0].refused);
  EXPECT_FALSE(events[0].calls[0].result.empty());

  EXPECT_EQ(events[1].turn, 2u);
  EXPECT_EQ(events[1].prompt_tokens, 456u);
  EXPECT_TRUE(events[1].calls.empty());
  EXPECT_EQ(events[1].content, "all done");
}

TEST_F(LoopFixture, TheOutcomeReportsHistoryGivenUpDuringTheRun) {
  // LoopOutcome::dropped was set on every exit path and asserted on none.
  // The instruction stays small -- an oversized one is refused before a turn runs, and
  // then nothing is ever trimmed. It is the accumulating *results* that must overflow.
  std::ofstream{tmp_ / "root" / "mid.txt"} << std::string(1000, 'm');
  const auto read_mid = json{{"paths", json::array({"mid.txt"})}};

  // prompt_tokens has to GROW across the script, and that is not incidental. Session
  // attributes each measurement back over the turns it covered, so a constant figure
  // re-anchors the whole history to that constant every turn and the budget is never
  // reached -- and a figure large against a tiny prompt drives chars_per_token to its 0.5
  // floor, which makes each result look hopeless and be replaced by a small refusal
  // instead of accumulating. Both of those made an earlier version of this test measure
  // nothing. These numbers are what a growing conversation actually reports.
  Script script{.replies = {call_reply({{"read", read_mid}}, 300),
                            call_reply({{"read", read_mid}}, 1500),
                            call_reply({{"read", read_mid}}, 2800),
                            call_reply({{"read", read_mid}}, 3700),
                            text_reply("done", 3800)}};
  auto options = session_options();
  options.num_ctx = 4096;  // budget 3840: a few ~1000-byte results overflow it
  options.reply_reserve = 256;
  options.max_tokens = 256;
  auto session = Session::open(options, dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, LoopOptions{}};
  const auto outcome = loop.run(*session, "read mid.txt");

  EXPECT_EQ(outcome.dropped, session->dropped());
  EXPECT_GT(outcome.dropped, 0u) << "this scenario is meant to force a trim";
}

TEST_F(LoopFixture, TheWallClockIsConsultedBeforeTheTurnCount) {
  // R8's claim, and the ordering was unobservable: no test exceeded both bounds at once, so
  // swapping the two checks survived a mutation pass.
  Script script;
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_,
                 LoopOptions{.max_turns = 0, .budget = std::chrono::seconds{0}}};
  const auto outcome = loop.run(*session, "do something");

  EXPECT_EQ(outcome.reason, StopReason::TimeBudget)
      << "with both bounds spent, the clock is the one that must be reported";
}

// --- R6: per-turn state verification ------------------------------------------
//
// The loop's stop condition still reads the model's behaviour (it stopped asking for
// tools). What these cover is the half that does not: what the filesystem shows, measured
// without reference to anything the model said.

namespace {

using hermes::supervisor::ChangeKind;
using hermes::supervisor::TreeVerifier;

const hermes::supervisor::Change* change_for(const hermes::supervisor::Changeset& set,
                                             std::string_view path) {
  for (const auto& change : set.changes) {
    if (change.path == path) return &change;
  }
  return nullptr;
}

}  // namespace

TEST_F(LoopFixture, ATurnsChangesAreReportedWithoutReadingTheReply) {
  TreeVerifier verifier{*sandbox_};
  std::vector<TurnEvent> events;
  LoopOptions options;
  options.verifier = &verifier;
  options.observer = [&events](const TurnEvent& e) { events.push_back(e); };

  Script script{.replies = {call_reply({{"write", json{{"path", "made.txt"},
                                                       {"content", "hello"}}}}),
                            text_reply("done")}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, std::move(options)};
  const auto outcome = loop.run(*session, "make a file");

  ASSERT_EQ(events.size(), 2u);
  ASSERT_NE(change_for(events[0].changes, "made.txt"), nullptr);
  EXPECT_EQ(change_for(events[0].changes, "made.txt")->kind, ChangeKind::Created);
  EXPECT_TRUE(events[1].changes.empty()) << "the answering turn touched nothing";
  EXPECT_EQ(outcome.reason, StopReason::Answered);
}

TEST_F(LoopFixture, TheNetChangesetIsTheWholeRunsEffectNotTheSumOfItsTurns) {
  // Created then deleted must not appear: a caller deciding whether to re-invoke needs
  // what the tree looks like now, not a transaction log.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;

  Script script{.replies = {call_reply({{"write", json{{"path", "temp.txt"},
                                                       {"content", "scratch"}}}}),
                            call_reply({{"move", json{{"from", "temp.txt"},
                                                      {"to", "final.txt"}}}}),
                            text_reply("done")}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, std::move(options)};
  const auto outcome = loop.run(*session, "make a file then rename it");

  EXPECT_EQ(change_for(outcome.net_changes, "temp.txt"), nullptr)
      << "a file created and then moved away should not appear in the net effect";
  ASSERT_NE(change_for(outcome.net_changes, "final.txt"), nullptr);
  EXPECT_EQ(change_for(outcome.net_changes, "final.txt")->kind, ChangeKind::Created);
}

TEST_F(LoopFixture, AConfidentAnswerOverAnUntouchedTreeProducesAnEmptyChangeset) {
  // R6's own evidence, as an assertion: llama32-3b replied DONE on an untouched tree in 18
  // of its 27 failed runs. The loop still reports Answered -- it has no basis to do
  // otherwise -- and the changeset is what contradicts it.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;

  Script script{.replies = {text_reply("I have created the file and verified it. DONE.")}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, std::move(options)};
  const auto outcome = loop.run(*session, "create report.txt");

  EXPECT_EQ(outcome.reason, StopReason::Answered);
  EXPECT_FALSE(outcome.final_content.empty()) << "the model sounded certain";
  EXPECT_TRUE(outcome.net_changes.empty()) << "and changed nothing";
  EXPECT_EQ(outcome.net_changes.substantive(), 0u);
}

TEST_F(LoopFixture, SeesAChangeNoToolMadeAndNoReplyMentioned) {
  // Verification must not depend on tool output either. Here the tree moves underneath the
  // loop between turns, exactly as `shell` or an outside process would move it.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;

  const auto root = tmp_ / "root";
  Script script;
  script.replies = {call_reply({{"read", json{{"paths", json::array({"notes.txt"})}}}}),
                    text_reply("done")};
  // Mutate the tree as a side effect of serving the first reply.
  auto raw = script.fn();
  hermes::supervisor::ChatFn meddling =
      [&raw, &root](const ChatRequest& request) {
        auto reply = raw(request);
        std::ofstream{root / "appeared.txt"} << "not via any tool\n";
        return reply;
      };

  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());
  AgentLoop loop{meddling, tools_->registry(), *sandbox_, std::move(options)};
  const auto outcome = loop.run(*session, "read notes.txt");

  ASSERT_NE(change_for(outcome.net_changes, "appeared.txt"), nullptr)
      << "a change nothing reported must still be seen";
  EXPECT_EQ(change_for(outcome.net_changes, "appeared.txt")->kind, ChangeKind::Created);
}

TEST_F(LoopFixture, WithoutAVerifierNothingIsReportedAndTheRunIsUnaffected) {
  Script script{.replies = {call_reply({{"write", json{{"path", "made.txt"},
                                                       {"content", "hello"}}}}),
                            text_reply("done")}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, LoopOptions{}};
  const auto outcome = loop.run(*session, "make a file");

  EXPECT_EQ(outcome.reason, StopReason::Answered);
  EXPECT_TRUE(outcome.net_changes.empty());
}

TEST_F(LoopFixture, AnUnverifiableTreeStopsTheRunRatherThanRunningUnverified) {
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores the permission bits this test sets";
  std::filesystem::create_directories(tmp_ / "root" / "locked");
  ::chmod((tmp_ / "root" / "locked").c_str(), 0000);

  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  Script script{.replies = {text_reply("done")}};
  auto session = Session::open(session_options(), dead_client(), "sys");
  ASSERT_TRUE(session.has_value());

  AgentLoop loop{script.fn(), tools_->registry(), *sandbox_, std::move(options)};
  const auto outcome = loop.run(*session, "do something");
  ::chmod((tmp_ / "root" / "locked").c_str(), 0755);

  EXPECT_EQ(outcome.reason, StopReason::VerificationFailed);
  EXPECT_EQ(outcome.turns, 0u) << "the baseline is taken before the first request";
  EXPECT_TRUE(script.seen.empty()) << "and no request went out unverifiable";
}
