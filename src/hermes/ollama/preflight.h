#pragma once

// R9 --- Preflight the model before running anything.
//
// Evidence (REQUIREMENTS.md): two installed models were disqualified by exactly these
// gates -- phi3.5:3.8b has no `tools` capability, and llama3-groq-tool-use:8b reports
// 8192 context. The tool-use-tuned model is the one that could not be loaded.
//
// R9's third piece of stated evidence -- "Ollama's default num_ctx is 4096, so every
// model needs a pinned variant" -- is **retracted**; see the note on the context gate
// below. It is false on 0.32.9, and the pinned variants turn out to be unnecessary
// besides. The two disqualified models above are unaffected.
//
// The point is to fail at startup rather than 60 runs into a job. A run that dies in
// the middle costs the whole batch; the same failure reported before the first request
// costs a second.
//
// Every check here fails *closed*. "I could not determine the context window" is
// reported as a failure, not waved through -- the same distinction the sandbox's EACCES
// fix turned on, in a place where the cost of guessing wrong is a wasted overnight run
// rather than an escape.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <hermes/ollama/client.h>

namespace hermes::ollama {

/// ⚠ **The context gate asks about the architecture, not the Modelfile pin.** The pin
/// decides nothing this client cares about: since D8 the client sets `options.num_ctx`
/// on every request, which overrides a Modelfile pin upward, and an unpinned model has
/// no knowable default either -- it loads above 4096 by a rule the API does not expose,
/// and not reliably at its full architecture either: `nemotron-3.5-lightning:30b` reports
/// a 1048576 architecture and loaded unpinned at 262144, well below it (DECISIONS.md D8).
/// Unpinned is therefore *less* knowable, not more. What no per-request setting can
/// exceed is the architecture itself, so that is the ceiling worth gating on, and
/// `/api/show` reports it directly.
///
/// The pin is still *shown*, because it is useful for diagnosing a model that behaves
/// oddly. It simply no longer decides anything.

/// The floor a model must be pinned at to be usable.
///
/// Upstream Hermes hard-refuses under 64_000 (`MINIMUM_CONTEXT_LENGTH`); bench/fsops
/// enforces 65536. This takes the higher, because every pinned variant that has
/// actually been run sits at 65536 or above, so the two numbers have never yet
/// disagreed about a real model -- and if they ever do, the stricter one is the one
/// that keeps this comparable with the fsops results.
inline constexpr std::uint64_t kMinimumContext = 65536;

enum class CheckId {
  OllamaReachable,  // the daemon answers at all
  ModelInstalled,   // the named model is one of the installed tags
  ModelMetadata,    // /api/show returned something we could read
  ToolCapability,   // Ollama reports `tools` for this model
  ContextWindow,    // num_ctx is pinned, and pinned high enough
  InferenceWarmup,  // one real completion came back
};

std::string_view to_string(CheckId id) noexcept;

struct Check {
  CheckId id;
  bool ok = false;
  std::string detail;
};

struct Policy {
  std::uint64_t minimum_context = kMinimumContext;
  bool require_tools = true;

  /// Send one two-token completion. Off by default: it is the only check that costs
  /// real time, since it pays the model load. Worth it before a long batch, wasteful
  /// before a single interactive command.
  bool warmup = false;
};

struct Report {
  std::string model;
  bool ok = false;
  std::vector<Check> checks;
  std::optional<ModelCard> card;

  /// Multi-line, one check per line, for printing to stderr at startup. Ends with a
  /// newline. Explains what to *do* about a failure, not merely that it happened --
  /// an unpinned model means "build a variant with PARAMETER num_ctx", and saying so
  /// is the difference between a loud failure and a useful one.
  [[nodiscard]] std::string render() const;

  /// The first failing check, for a one-line summary.
  [[nodiscard]] const Check* first_failure() const noexcept;
};

/// Run the full R9 preflight against a live daemon.
///
/// Stops at the first failure that makes later checks meaningless: if Ollama is
/// unreachable there is nothing to ask about the model, and reporting four more
/// "not checked" lines buries the one line that matters.
[[nodiscard]] Report preflight(const Client& client, std::string_view model,
                               const Policy& policy = {});

/// The card-only half, pure and separable from the network: given metadata, decide
/// whether the model is usable. Exposed so the decision table can be tested against
/// every shape Ollama produces without a daemon, including the shapes that are
/// awkward to produce on demand -- an unpinned model, an Ollama too old to report
/// capabilities, an architecture too small for the floor, and a generous pin over a
/// small architecture (which no request setting can rescue).
[[nodiscard]] std::vector<Check> evaluate_card(const ModelCard& card, const Policy& policy);

}  // namespace hermes::ollama
