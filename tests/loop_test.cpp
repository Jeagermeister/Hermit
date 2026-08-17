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
