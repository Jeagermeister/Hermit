#include <hermes/ollama/preflight.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using hermes::ollama::Check;
using hermes::ollama::CheckId;
using hermes::ollama::evaluate_card;
using hermes::ollama::kMinimumContext;
using hermes::ollama::ModelCard;
using hermes::ollama::Policy;

namespace {

/// A model that passes everything, as the starting point each test degrades from --
/// so a failure names the one property that caused it rather than being one of
/// several things wrong at once.
ModelCard usable() {
  ModelCard card;
  card.reported_capabilities = true;
  card.capabilities = {"completion", "tools", "thinking"};
  card.num_ctx = 131072;
  card.architecture_context = 262144;
  card.family = "qwen35moe";
  return card;
}

/// Returns a copy rather than a pointer, and takes the vector by value, so a caller
/// writing `find(evaluate_card(...), id)` cannot end up holding a reference into a
/// temporary that has already died. The first draft of this returned `const Check*`
/// and segfaulted in exactly that shape.
std::optional<Check> find(std::vector<Check> checks, CheckId id) {
  const auto found = std::ranges::find_if(checks, [id](const Check& c) { return c.id == id; });
  if (found == checks.end()) return std::nullopt;
  return *found;
}

bool passed(std::vector<Check> checks, CheckId id) {
  const auto check = find(std::move(checks), id);
  return check.has_value() && check->ok;
}

}  // namespace

TEST(Preflight, AUsableModelPassesEveryStaticGate) {
  const auto checks = evaluate_card(usable(), Policy{});
  ASSERT_FALSE(checks.empty());
  for (const auto& check : checks) EXPECT_TRUE(check.ok) << check.detail;
}

// --- the context gate --------------------------------------------------------
//
// Gates on the ARCHITECTURE, not the Modelfile pin. Since D8 the client sets
// options.num_ctx per request, and that overrides the pin upward -- measured: a model
// pinned to 8192 and asked for 32768 loaded at 32768. So the pin decides nothing, and
// the only limit that survives is what the model was built to hold.

TEST(Preflight, AModelWhoseArchitectureCoversTheFloorPasses) {
  ModelCard card = usable();
  card.architecture_context = 262144;
  EXPECT_TRUE(passed(evaluate_card(card, Policy{}), CheckId::ContextWindow));
}

TEST(Preflight, AnUnpinnedModelIsNoLongerRejected) {
  // This is the flip. It used to fail as "unpinned", on the since-falsified premise
  // that an unpinned model runs at 4096. The client now states the context itself, so
  // an absent pin is simply not a problem.
  ModelCard card = usable();
  card.num_ctx.reset();
  card.architecture_context = 262144;
  EXPECT_TRUE(passed(evaluate_card(card, Policy{}), CheckId::ContextWindow));
}

TEST(Preflight, ALowPinDoesNotFailAModelWithARoomyArchitecture) {
  // Exactly the OpenCode tournament tags: pinned 32768 under a 262144 architecture.
  // The pin is overridden per request, so it cannot make the model unusable.
  ModelCard card = usable();
  card.num_ctx = 32768;
  card.architecture_context = 262144;

  const auto context = find(evaluate_card(card, Policy{}), CheckId::ContextWindow);
  ASSERT_TRUE(context.has_value());
  EXPECT_TRUE(context->ok) << context->detail;
  EXPECT_NE(context->detail.find("overrides"), std::string::npos)
      << "the pin should still be reported, just not enforced: " << context->detail;
}

TEST(Preflight, AnArchitectureBelowTheFloorIsRejected) {
  // llama3-groq-tool-use:8b from R9's evidence: tool-use-tuned and genuinely unable to
  // hold the context, which no request setting can fix.
  ModelCard card = usable();
  card.num_ctx = 8192;
  card.architecture_context = 8192;

  const auto context = find(evaluate_card(card, Policy{}), CheckId::ContextWindow);
  ASSERT_TRUE(context.has_value());
  EXPECT_FALSE(context->ok);
  EXPECT_NE(context->detail.find("8192"), std::string::npos);
  EXPECT_NE(context->detail.find("different model"), std::string::npos) << context->detail;
}

TEST(Preflight, AGenerousPinCannotRescueASmallArchitecture) {
  // The inverse trap. A Modelfile may name any number; it does not make the model
  // able to hold it.
  ModelCard card = usable();
  card.num_ctx = 262144;
  card.architecture_context = 8192;
  EXPECT_FALSE(passed(evaluate_card(card, Policy{}), CheckId::ContextWindow));
}

TEST(Preflight, AnUnreportedArchitectureIsRejectedAsUndetermined) {
  // Fail-closed: with no ceiling there is no way to tell whether the context this
  // client intends to request is one the model can hold.
  ModelCard card = usable();
  card.architecture_context.reset();

  const auto context = find(evaluate_card(card, Policy{}), CheckId::ContextWindow);
  ASSERT_TRUE(context.has_value());
  EXPECT_FALSE(context->ok);
  EXPECT_NE(context->detail.find("ceiling cannot be established"), std::string::npos)
      << context->detail;
}

TEST(Preflight, ExactlyTheFloorIsAccepted) {
  ModelCard card = usable();
  card.architecture_context = kMinimumContext;
  EXPECT_TRUE(passed(evaluate_card(card, Policy{}), CheckId::ContextWindow));
}

TEST(Preflight, OneBelowTheFloorIsRejected) {
  ModelCard card = usable();
  card.architecture_context = kMinimumContext - 1;
  EXPECT_FALSE(passed(evaluate_card(card, Policy{}), CheckId::ContextWindow));
}

TEST(Preflight, TheFloorIsPolicyNotAConstant) {
  ModelCard card = usable();
  card.architecture_context = 32768;

  Policy relaxed;
  relaxed.minimum_context = 32768;
  EXPECT_TRUE(passed(evaluate_card(card, relaxed), CheckId::ContextWindow));
}

// --- the tools gate ----------------------------------------------------------

TEST(Preflight, AModelWithoutToolsIsRejected) {
  // phi3.5:3.8b, the other model from R9's evidence.
  ModelCard card = usable();
  card.capabilities = {"completion"};

  const auto tools = find(evaluate_card(card, Policy{}), CheckId::ToolCapability);
  ASSERT_TRUE(tools.has_value());
  EXPECT_FALSE(tools->ok);
  EXPECT_NE(tools->detail.find("completion"), std::string::npos)
      << "say what it does report, so the answer is diagnosable: " << tools->detail;
}

TEST(Preflight, AnOllamaThatReportsNoCapabilityListIsRejected) {
  // Distinct from "no tools", and the reason `reported_capabilities` exists. An
  // Ollama too old to answer cannot be read as having answered no.
  ModelCard card = usable();
  card.reported_capabilities = false;
  card.capabilities.clear();

  const auto tools = find(evaluate_card(card, Policy{}), CheckId::ToolCapability);
  ASSERT_TRUE(tools.has_value());
  EXPECT_FALSE(tools->ok);
  EXPECT_NE(tools->detail.find("upgrade Ollama"), std::string::npos) << tools->detail;
}

TEST(Preflight, TheTwoWaysOfLackingToolsGiveDifferentMessages) {
  ModelCard silent = usable();
  silent.reported_capabilities = false;
  silent.capabilities.clear();

  ModelCard refused = usable();
  refused.capabilities = {"completion"};

  const auto from_silent = find(evaluate_card(silent, Policy{}), CheckId::ToolCapability);
  const auto from_refused = find(evaluate_card(refused, Policy{}), CheckId::ToolCapability);
  ASSERT_TRUE(from_silent.has_value());
  ASSERT_TRUE(from_refused.has_value());
  EXPECT_NE(from_silent->detail, from_refused->detail);
}

TEST(Preflight, TheToolsGateCanBeWaived) {
  // The core is useful with no model at all (D7), and a non-tool model is a
  // legitimate thing to talk to for something that is not the agent loop.
  ModelCard card = usable();
  card.capabilities = {"completion"};

  Policy no_tools;
  no_tools.require_tools = false;
  const auto checks = evaluate_card(card, no_tools);
  EXPECT_FALSE(find(checks, CheckId::ToolCapability).has_value());
  for (const auto& check : checks) EXPECT_TRUE(check.ok) << check.detail;
}

// --- independence ------------------------------------------------------------

TEST(Preflight, BothGatesFailIndependentlyAndBothAreReported) {
  // Not short-circuited: being told about the context only to hit the tools failure
  // on the next run is the slow-failure pattern R9 exists to remove.
  ModelCard card;
  card.reported_capabilities = true;
  card.capabilities = {"completion"};
  card.architecture_context = 8192;

  const auto checks = evaluate_card(card, Policy{});
  EXPECT_FALSE(passed(checks, CheckId::ToolCapability));
  EXPECT_FALSE(passed(checks, CheckId::ContextWindow));
}
