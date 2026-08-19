// R7's driver, exercised without a daemon.
//
// Everything here rides the same two seams the loop's own tests do -- a scripted ChatFn
// and Session's bare-options overload -- plus the driver's own SessionFactory. The
// scripted replies are consumed across attempts in order, which is exactly how the tests
// tell the attempts apart: what the second session was *asked* is readable from
// `Attempt::instruction`, and what it was allowed to see is a fresh history by
// construction.

#include <hermit/supervisor/reinvoke.h>

#include <gtest/gtest.h>

#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermit/app/toolset.h>
#include <hermit/supervisor/loop.h>

namespace fs = std::filesystem;
using hermit::Sandbox;
using hermit::ollama::ChatReply;
using hermit::ollama::ChatRequest;
using hermit::ollama::ToolCall;
using hermit::supervisor::Attempt;
using hermit::supervisor::ChatFn;
using hermit::supervisor::Expectation;
using hermit::supervisor::Finding;
using hermit::supervisor::LoopOptions;
using hermit::supervisor::Outcome;
using hermit::supervisor::reinvocation_instruction;
using hermit::supervisor::reinvoke;
using hermit::supervisor::ReinvokeOptions;
using hermit::supervisor::Session;
using hermit::supervisor::SessionOptions;
using hermit::supervisor::StopReason;
using hermit::supervisor::TreeVerifier;
using json = nlohmann::json;

namespace {

ChatReply text_reply(std::string content) {
  ChatReply reply;
  reply.content = std::move(content);
  reply.finish_reason = "stop";
  reply.prompt_tokens = 200;
  reply.completion_tokens = 20;
  return reply;
}

ChatReply call_reply(const std::vector<std::pair<std::string, json>>& calls) {
  ChatReply reply;
  reply.finish_reason = "stop";
  reply.prompt_tokens = 200;
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

struct Script {
  std::vector<ChatReply> replies{};
  std::size_t served = 0;
  std::vector<ChatRequest> seen{};

  ChatFn fn() {
    return [this](const ChatRequest& request) -> hermit::ollama::Result<ChatReply> {
      seen.push_back(request);
      if (served >= replies.size()) {
        return std::unexpected(hermit::ollama::Failure{
            hermit::ollama::TransportError::Unreachable, "script exhausted"});
      }
      return replies[served++];
    };
  }
};

hermit::ollama::ClientOptions dead_client() {
  hermit::ollama::ClientOptions options;
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

class ReinvokeFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermit_reinvoke_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::path{buf.data()};
    fs::create_directories(tmp_ / "root");

    std::ofstream{tmp_ / "root" / "notes.txt"} << "alpha\nbeta\n";

    auto box = Sandbox::open(tmp_ / "root");
    ASSERT_TRUE(box.has_value());
    sandbox_ = std::make_unique<Sandbox>(std::move(*box));

    auto tools = hermit::app::ToolSet::tier0(tmp_ / "backups");
    ASSERT_TRUE(tools.has_value());
    tools_ = std::make_unique<hermit::app::ToolSet>(std::move(*tools));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  /// One fresh session per call, counted, so a test can assert "per attempt" directly.
  hermit::supervisor::SessionFactory counted_factory() {
    return [this] {
      ++sessions_opened_;
      return Session::open(session_options(), dead_client(), "sys");
    };
  }

  fs::path tmp_;
  std::unique_ptr<Sandbox> sandbox_;
  std::unique_ptr<hermit::app::ToolSet> tools_;
  std::size_t sessions_opened_ = 0;
};

}  // namespace

// --- the composed prompt ------------------------------------------------------

TEST(ReinvocationInstruction, CarriesTheTaskAndTheOneConcreteFailure) {
  // The task travels too, because the failure alone is not actionable: "does not exist"
  // says nothing about what the file should contain.
  Finding unmet;
  unmet.expectation = Expectation::exists("falcon-index.md");
  unmet.outcome = Outcome::Unmet;
  unmet.reason = "falcon-index.md does not exist";

  const std::string prompt = reinvocation_instruction("index the notes", unmet);
  EXPECT_NE(prompt.find("index the notes"), std::string::npos);
  EXPECT_NE(prompt.find("falcon-index.md does not exist"), std::string::npos);
}

TEST(ReinvocationInstruction, AWordlessFindingFallsBackToTheExpectationItself) {
  // judge() always words an unmet finding; a hand-built one must not trail off into an
  // empty clause.
  Finding unmet;
  unmet.expectation = Expectation::exists("falcon-index.md");
  unmet.outcome = Outcome::Unmet;

  const std::string prompt = reinvocation_instruction("index the notes", unmet);
  EXPECT_NE(prompt.find(unmet.expectation.render()), std::string::npos);
}

// --- when a retry happens, and what it is given -------------------------------

TEST_F(ReinvokeFixture, AMetFirstAttemptIsNotRetried) {
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::exists("made.txt")};

  Script script{.replies = {call_reply({{"write", json{{"path", "made.txt"},
                                                       {"content", "hello"}}}}),
                            text_reply("done")}};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            {.attempts = 3}, counted_factory(), "make a file");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 1u);
  ASSERT_EQ(job.last().verdict.findings.size(), 1u);
  EXPECT_EQ(job.last().verdict.findings[0].outcome, Outcome::Met);
  EXPECT_EQ(sessions_opened_, 1u);
}

TEST_F(ReinvokeFixture, ASecondAttemptIsInvokedWithTheTaskAndTheFailure) {
  // The measured R6 failure, then the R7 answer to it: attempt one announces success
  // over an untouched tree; attempt two -- a fresh session, told the task and the one
  // thing still undone -- does the work.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::exists("falcon-index.md")};

  Script script{.replies = {text_reply("DONE"),
                            call_reply({{"write", json{{"path", "falcon-index.md"},
                                                       {"content", "# index\n"}}}}),
                            text_reply("done")}};

  std::vector<bool> retry_flags;
  ReinvokeOptions retries;
  retries.attempts = 3;
  retries.on_attempt = [&](std::size_t, std::size_t, const Finding* retrying) {
    retry_flags.push_back(retrying != nullptr);
  };

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            retries, counted_factory(), "index the notes");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 2u);

  // What the second session was actually asked.
  const std::string& second = job.attempts[1].instruction;
  EXPECT_NE(second.find("index the notes"), std::string::npos);
  EXPECT_NE(second.find("falcon-index.md does not exist"), std::string::npos);

  ASSERT_EQ(job.last().verdict.findings.size(), 1u);
  EXPECT_EQ(job.last().verdict.findings[0].outcome, Outcome::Met);

  EXPECT_EQ(sessions_opened_, 2u) << "one fresh session per attempt";
  EXPECT_EQ(retry_flags, (std::vector<bool>{false, true}));
}

TEST_F(ReinvokeFixture, EveryAttemptFailingSpendsExactlyTheAllowedAttempts) {
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::exists("never.md")};

  Script script{.replies = {text_reply("DONE"), text_reply("DONE"), text_reply("DONE")}};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            {.attempts = 3}, counted_factory(), "make never.md");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 3u);
  ASSERT_EQ(job.last().verdict.findings.size(), 1u);
  EXPECT_EQ(job.last().verdict.findings[0].outcome, Outcome::Unmet);

  // The third prompt is composed from the *original* task, never from the second's
  // composed prompt -- the framing must not nest.
  const std::string& third = job.attempts[2].instruction;
  const std::string marker = "An earlier attempt";
  std::size_t occurrences = 0;
  for (std::size_t at = third.find(marker); at != std::string::npos;
       at = third.find(marker, at + 1)) {
    ++occurrences;
  }
  EXPECT_EQ(occurrences, 1u);
}

TEST_F(ReinvokeFixture, ABoundCutoffWithAnUnmetVerdictIsRetried) {
  // A run cut off with work remaining is exactly what a fresh bounded session is for;
  // the judge's evidence is no weaker for the model having been stopped mid-stride.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.max_turns = 1;
  options.expected = {Expectation::exists("never.md")};

  // Every reply asks for another tool, so every attempt exhausts its single turn.
  const auto busy = call_reply({{"read", json{{"paths", json::array({"notes.txt"})}}}});
  Script script{.replies = {busy, busy}};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            {.attempts = 2}, counted_factory(), "make never.md");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 2u);
  EXPECT_EQ(job.attempts[0].outcome.reason, StopReason::TurnBudget);
  EXPECT_EQ(job.attempts[1].outcome.reason, StopReason::TurnBudget);
}

// --- when a retry must NOT happen ---------------------------------------------

TEST_F(ReinvokeFixture, AnUndecidableOnlyVerdictIsNotRetried) {
  // "One side could not be read" is never sent to the model (judge.h), so there is
  // nothing to re-invoke with: `ghost.txt` was never in the baseline, and no number of
  // model runs changes that.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::preserved("ghost.txt", "copy.txt")};

  Script script{.replies = {text_reply("DONE"), text_reply("DONE")}};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            {.attempts = 3}, counted_factory(), "copy the ghost");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 1u);
  ASSERT_EQ(job.last().verdict.findings.size(), 1u);
  EXPECT_EQ(job.last().verdict.findings[0].outcome, Outcome::Undecidable);
}

TEST_F(ReinvokeFixture, ATransportFailureIsNotRetried) {
  // R7 exists for model inconsistency. A dead daemon is not the model's failure, and a
  // retry would spend attempts asking a socket that already refused to answer.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::exists("never.md")};

  Script script{};  // exhausted immediately: every chat fails

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            {.attempts = 3}, counted_factory(), "make never.md");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 1u);
  EXPECT_EQ(job.last().reason, StopReason::Transport);
}

TEST_F(ReinvokeFixture, NoExpectationsMeansExactlyOneAttempt) {
  // With nothing stated there is no verdict to retry against, so `--attempts 3` on a
  // report-only run must not triple its cost.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;

  Script script{.replies = {text_reply("done")}};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            {.attempts = 3}, counted_factory(), "look around");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 1u);
  EXPECT_EQ(sessions_opened_, 1u);
}

TEST_F(ReinvokeFixture, ASessionThatCannotOpenStopsTheJobWithTheErrorRecorded) {
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::exists("never.md")};

  Script script{.replies = {text_reply("DONE"), text_reply("DONE")}};

  // The second open is refused: a non-positive generation budget is one of the
  // combinations `Session::open` fails closed on.
  std::size_t opened = 0;
  const hermit::supervisor::SessionFactory flaky = [&] {
    SessionOptions options_for_this = session_options();
    if (++opened > 1) options_for_this.max_tokens = 0;
    return Session::open(options_for_this, dead_client(), "sys");
  };

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            {.attempts = 3}, flaky, "make never.md");

  EXPECT_FALSE(job.error.empty());
  EXPECT_EQ(job.attempts.size(), 1u) << "the first attempt's record is kept";
}

TEST_F(ReinvokeFixture, AnUnreadableTreeFailsTheJobBeforeAnyModelIsCalled) {
  fs::create_directories(tmp_ / "root" / "locked");
  ::chmod((tmp_ / "root" / "locked").c_str(), 0000);

  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::exists("never.md")};

  Script script{.replies = {text_reply("DONE")}};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            {.attempts = 3}, counted_factory(), "make never.md");

  ::chmod((tmp_ / "root" / "locked").c_str(), 0755);

  EXPECT_FALSE(job.error.empty());
  EXPECT_NE(job.error.find("baseline"), std::string::npos) << job.error;
  EXPECT_TRUE(job.attempts.empty());
  EXPECT_TRUE(script.seen.empty()) << "a model turn was spent on an unjudgeable job";
}

// --- the one-baseline rule ----------------------------------------------------

TEST_F(ReinvokeFixture, TheJobBaselineHoldsAcrossAttempts) {
  // The design decision this file exists to protect, as a run. `preserved:notes.txt=
  // copy.txt` reads its source bytes from the baseline. Attempt one *moves* the source
  // to the wrong place -- so if attempt two were judged against its own opening
  // snapshot, `notes.txt` would no longer be in it and the expectation would collapse
  // to Undecidable, unrecoverable by any model. Judged against the job baseline,
  // attempt two can finish the move and be found Met.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::preserved("notes.txt", "copy.txt")};

  Script script{.replies = {call_reply({{"move", json{{"from", "notes.txt"},
                                                      {"to", "wrong.txt"}}}}),
                            text_reply("DONE"),
                            call_reply({{"move", json{{"from", "wrong.txt"},
                                                      {"to", "copy.txt"}}}}),
                            text_reply("done")}};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            {.attempts = 3}, counted_factory(), "copy notes.txt");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 2u);

  // Attempt one: unmet, with the destination named -- the sentence attempt two was fed.
  const auto first = job.attempts[0].outcome.verdict.first_unmet();
  ASSERT_TRUE(first.has_value());
  EXPECT_NE(first->reason.find("copy.txt"), std::string::npos);

  // Attempt two: met on merit against the tree the *job* started from.
  ASSERT_EQ(job.last().verdict.findings.size(), 1u);
  EXPECT_EQ(job.last().verdict.findings[0].outcome, Outcome::Met);
}

// --- meaning after structure (semantic.h, wired through the driver) -----------

TEST_F(ReinvokeFixture, MeaningIsJudgedOnlyAfterStructurePasses) {
  // Attempt one leaves the structural expectation unmet; the semantic judge must not
  // be consulted, and the criterion must still appear in the verdict -- as Undecidable,
  // "not judged" -- so nothing the operator stated silently drops out of the report.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::exists("made.txt")};

  Script script{.replies = {text_reply("DONE")}};
  Script judge_script{};  // must never be consulted

  ReinvokeOptions retries;
  retries.attempts = 1;
  retries.semantic = {Expectation::satisfies("made.txt", "a greeting")};
  retries.judge = hermit::supervisor::SemanticJudge{
      .chat = judge_script.fn(), .model = "judge", .num_ctx = 4096};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            retries, counted_factory(), "make a file");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 1u);
  EXPECT_TRUE(judge_script.seen.empty()) << "the judge ran on a structurally failed attempt";
  ASSERT_EQ(job.last().verdict.findings.size(), 2u);
  EXPECT_EQ(job.last().verdict.findings[0].outcome, Outcome::Unmet);
  EXPECT_EQ(job.last().verdict.findings[1].outcome, Outcome::Undecidable);
  EXPECT_NE(job.last().verdict.findings[1].reason.find("not judged"), std::string::npos);
}

TEST_F(ReinvokeFixture, AnUnmetJudgmentDrivesTheRetryAndAMetOneStopsIt) {
  // The E1 residue, closed end to end: structure holds on the first attempt, the judge
  // reads the bytes and says no, and its sentence -- not a hash's -- is what the fresh
  // session is re-invoked with.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::exists("summary.md")};

  Script script{.replies = {call_reply({{"write", json{{"path", "summary.md"},
                                                       {"content", "grep -c . notes.txt"}}}}),
                            text_reply("done"),
                            text_reply("rewrote it")}};
  Script judge_script{
      .replies = {[] {
                    ChatReply r;
                    r.content = json{{"satisfied", false},
                                     {"reason", "summary.md holds a shell command, not "
                                                "a summary"}}.dump();
                    r.finish_reason = "stop";
                    return r;
                  }(),
                  [] {
                    ChatReply r;
                    r.content = json{{"satisfied", true}, {"reason", ""}}.dump();
                    r.finish_reason = "stop";
                    return r;
                  }()}};

  ReinvokeOptions retries;
  retries.attempts = 3;
  retries.semantic = {Expectation::satisfies("summary.md", "a prose summary")};
  retries.judge = hermit::supervisor::SemanticJudge{
      .chat = judge_script.fn(), .model = "judge", .num_ctx = 4096};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            retries, counted_factory(), "summarise the notes");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 2u);
  EXPECT_EQ(judge_script.seen.size(), 2u) << "once per structurally met attempt";

  // The retry was composed from the judge's sentence.
  const std::string& second = job.attempts[1].instruction;
  EXPECT_NE(second.find("summarise the notes"), std::string::npos);
  EXPECT_NE(second.find("holds a shell command"), std::string::npos);

  ASSERT_EQ(job.last().verdict.findings.size(), 2u);
  EXPECT_TRUE(job.last().verdict.met());
}

TEST_F(ReinvokeFixture, CriteriaWithNoJudgeRefuseTheJobBeforeAnyModelRun) {
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;

  Script script{};
  ReinvokeOptions retries;
  retries.semantic = {Expectation::satisfies("summary.md", "a prose summary")};
  // no judge

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            retries, counted_factory(), "summarise the notes");

  EXPECT_FALSE(job.error.empty());
  EXPECT_NE(job.error.find("no semantic judge"), std::string::npos) << job.error;
  EXPECT_TRUE(job.attempts.empty());
  EXPECT_EQ(sessions_opened_, 0u);
}

TEST_F(ReinvokeFixture, AStructuralExpectationInTheSemanticSetIsRefused) {
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;

  Script script{};
  Script judge_script{};
  ReinvokeOptions retries;
  retries.semantic = {Expectation::exists("summary.md")};
  retries.judge = hermit::supervisor::SemanticJudge{
      .chat = judge_script.fn(), .model = "judge", .num_ctx = 4096};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            retries, counted_factory(), "summarise the notes");

  EXPECT_FALSE(job.error.empty());
  EXPECT_NE(job.error.find("structural"), std::string::npos) << job.error;
  EXPECT_TRUE(job.attempts.empty());
}

TEST_F(ReinvokeFixture, AnUnreachableJudgeIsReportedNotRetried) {
  // The attempt itself succeeded; only the judge could not run. Undecidable is never
  // retried (a retry would need the same dead daemon), and it is not a job error --
  // the verdict simply says the criterion went undecided.
  TreeVerifier verifier{*sandbox_};
  LoopOptions options;
  options.verifier = &verifier;
  options.expected = {Expectation::exists("made.txt")};

  Script script{.replies = {call_reply({{"write", json{{"path", "made.txt"},
                                                       {"content", "hello"}}}}),
                            text_reply("done")}};
  Script judge_script{};  // empty: reports Unreachable when consulted

  ReinvokeOptions retries;
  retries.attempts = 3;
  retries.semantic = {Expectation::satisfies("made.txt", "a greeting")};
  retries.judge = hermit::supervisor::SemanticJudge{
      .chat = judge_script.fn(), .model = "judge", .num_ctx = 4096};

  const auto job = reinvoke(script.fn(), tools_->registry(), *sandbox_, std::move(options),
                            retries, counted_factory(), "make a file");

  EXPECT_TRUE(job.error.empty()) << job.error;
  ASSERT_EQ(job.attempts.size(), 1u);
  ASSERT_EQ(job.last().verdict.findings.size(), 2u);
  EXPECT_EQ(job.last().verdict.findings[0].outcome, Outcome::Met);
  EXPECT_EQ(job.last().verdict.findings[1].outcome, Outcome::Undecidable);
  EXPECT_FALSE(job.last().verdict.met());
}
