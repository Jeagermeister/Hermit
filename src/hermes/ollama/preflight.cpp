#include <hermes/ollama/preflight.h>

#include <algorithm>
#include <utility>

namespace hermes::ollama {
namespace {

Check fail(CheckId id, std::string detail) { return {id, false, std::move(detail)}; }
Check pass(CheckId id, std::string detail) { return {id, true, std::move(detail)}; }

Report finish(Report report) {
  report.ok = std::ranges::all_of(report.checks, [](const Check& c) { return c.ok; });
  return report;
}

}  // namespace

std::string_view to_string(CheckId id) noexcept {
  switch (id) {
    case CheckId::OllamaReachable: return "ollama reachable";
    case CheckId::ModelInstalled:  return "model installed";
    case CheckId::ModelMetadata:   return "model metadata";
    case CheckId::ToolCapability:  return "tool capability";
    case CheckId::ContextWindow:   return "context window";
    case CheckId::InferenceWarmup: return "inference warmup";
  }
  return "unknown check";
}

std::vector<Check> evaluate_card(const ModelCard& card, const Policy& policy) {
  std::vector<Check> checks;

  if (policy.require_tools) {
    if (!card.reported_capabilities) {
      // Not the same as "no tools". An Ollama too old to report capabilities cannot
      // answer the question, and answering it anyway -- in either direction -- is the
      // fail-open this whole requirement exists to prevent.
      checks.push_back(fail(CheckId::ToolCapability,
                            "this Ollama did not report a capability list, so tool support "
                            "cannot be confirmed; upgrade Ollama"));
    } else if (!card.has_capability("tools")) {
      std::string detail = "model does not support tools";
      if (!card.capabilities.empty()) {
        detail += "; reports";
        for (const auto& capability : card.capabilities) detail += " " + capability;
      }
      checks.push_back(fail(CheckId::ToolCapability, std::move(detail)));
    } else {
      checks.push_back(pass(CheckId::ToolCapability, "tools"));
    }
  }

  // The Modelfile pin is reported but never gates -- see the note in preflight.h. The
  // client overrides it per request, so the only limit that survives is the one the
  // model was built with.
  const auto pin_note = [&card] {
    return card.num_ctx ? "; Modelfile pins " + std::to_string(*card.num_ctx) + ", which this "
                                                                                "client overrides"
                        : std::string{};
  };

  if (!card.architecture_context) {
    // Fails closed: without the ceiling there is no way to tell whether the context
    // this client intends to request is one the model can actually hold.
    checks.push_back(fail(CheckId::ContextWindow,
                          "/api/show did not report a context length for this architecture, so "
                          "the model's ceiling cannot be established" +
                              pin_note()));
  } else if (*card.architecture_context < policy.minimum_context) {
    // Nothing can be done about this one. Unlike a low pin, it is not a Modelfile
    // mistake -- the model was not built to hold this much, and no request setting
    // changes that. It needs a different model.
    checks.push_back(fail(CheckId::ContextWindow,
                          "the architecture supports only " +
                              std::to_string(*card.architecture_context) + ", below the " +
                              std::to_string(policy.minimum_context) +
                              " floor; no per-request setting exceeds what the model was built "
                              "for, so this needs a different model" +
                              pin_note()));
  } else {
    checks.push_back(pass(CheckId::ContextWindow,
                          "architecture supports " + std::to_string(*card.architecture_context) +
                              pin_note()));
  }

  return checks;
}

Report preflight(const Client& client, std::string_view model, const Policy& policy) {
  Report report;
  report.model = std::string{model};

  const auto tags = client.tags();
  if (!tags) {
    report.checks.push_back(fail(CheckId::OllamaReachable,
                                 tags.error().message() + " at " + client.base_url()));
    return finish(std::move(report));
  }
  report.checks.push_back(pass(CheckId::OllamaReachable,
                               client.base_url() + ", " + std::to_string(tags->size()) +
                                   " models installed"));

  const std::string_view wanted = strip_latest(model);
  const bool installed = std::ranges::any_of(
      *tags, [&](const ModelTag& tag) { return strip_latest(tag.name) == wanted; });
  if (!installed) {
    report.checks.push_back(
        fail(CheckId::ModelInstalled, std::string{model} + " is not installed; `ollama pull` it"));
    // Everything below asks about a model that is not there. Stop.
    return finish(std::move(report));
  }
  report.checks.push_back(pass(CheckId::ModelInstalled, std::string{model}));

  const auto card = client.show(model);
  if (!card) {
    report.checks.push_back(fail(CheckId::ModelMetadata, card.error().message()));
    return finish(std::move(report));
  }
  report.checks.push_back(pass(CheckId::ModelMetadata,
                               card->family.empty() ? "read" : card->family + ", " +
                                                                   card->parameter_size + ", " +
                                                                   card->quantization));
  report.card = *card;

  for (auto& check : evaluate_card(*card, policy)) report.checks.push_back(std::move(check));

  // Only warm up a model that has already passed the static gates: loading a 30 GB
  // model to discover it has no tools is exactly the slow failure R9 exists to avoid.
  const bool static_gates_passed =
      std::ranges::all_of(report.checks, [](const Check& c) { return c.ok; });
  if (policy.warmup && static_gates_passed) {
    ChatRequest warmup;
    warmup.model = std::string{model};
    warmup.messages.push_back({"user", "Reply OK."});
    warmup.max_tokens = 16;
    // State the context rather than inheriting the server default, which is the whole
    // point of the native endpoint. The client clamps this, so the floor cannot be
    // used to request something the card will not hold.
    warmup.num_ctx = policy.minimum_context;

    const auto reply = client.chat(warmup);
    if (!reply) {
      report.checks.push_back(fail(CheckId::InferenceWarmup, reply.error().message()));
    } else if (!reply->generated()) {
      // A well-formed reply that contains no evidence the model produced anything.
      // "I could not tell whether generation happened" is not a pass.
      report.checks.push_back(fail(CheckId::InferenceWarmup,
                                   "the daemon answered but the reply shows no generated "
                                   "tokens, no content and no reasoning"));
    } else {
      // Deliberately does not require non-empty content. A thinking model spends the
      // budget on reasoning before it writes anything, so an empty content here is
      // the expected result, not a failure -- see ChatReply::completion_tokens. What
      // is being proved is that the model loads and generates; whether it had room to
      // finish a sentence in 16 tokens is not the question.
      std::string detail = reply->truncated() ? "generated, truncated at the warmup budget"
                                              : "completion returned";
      if (!reply->reasoning.empty()) detail += "; thinking model";
      report.checks.push_back(pass(CheckId::InferenceWarmup, std::move(detail)));
    }
  }

  return finish(std::move(report));
}

const Check* Report::first_failure() const noexcept {
  const auto found = std::ranges::find_if(checks, [](const Check& c) { return !c.ok; });
  return found == checks.end() ? nullptr : &*found;
}

std::string Report::render() const {
  std::string text = "preflight " + model + ": " + (ok ? "ok" : "FAILED") + "\n";
  for (const auto& check : checks) {
    text += "  ";
    text += check.ok ? "ok    " : "FAIL  ";
    text += to_string(check.id);
    if (!check.detail.empty()) {
      text += "\n          ";
      text += check.detail;
    }
    text += "\n";
  }
  return text;
}

}  // namespace hermes::ollama
