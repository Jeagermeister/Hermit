#pragma once

// The only layer that speaks HTTP (DECISIONS.md, D7).
//
// Talks to a local Ollama over loopback and nothing else: no TLS, no auth, no proxies,
// no retries on connect. A connection refused here means Ollama is not running, which
// is a startup failure to report, not a condition to paper over.
//
// httplib.h is deliberately absent from this header. It is a large header-only library
// and every translation unit that included it would pay for it, so it lives behind a
// pimpl in client.cpp. That also keeps D7's layering honest by construction: nothing
// outside this target can reach the HTTP client even by accident.
//
// Native Ollama endpoints throughout -- /api/tags, /api/show, /api/chat.
//
// The OpenAI-compatible /v1/chat/completions was used first and was dropped on
// 2026-08-13. It cannot set `num_ctx`, which made every model's context window a
// property of its Modelfile and of the Ollama server's own default -- and that
// default turned out to be unreportable through any API and to have changed between
// releases, which is what falsified R9's original "unpinned means 4096" evidence.
// Setting the context per request replaces an unknowable with a stated value.
//
// Two smaller gains came with it: tool-call arguments arrive as a structured object
// rather than a JSON string needing a second parse, and `format` takes a schema
// directly instead of the OpenAI json_schema wrapper. The first was measured against
// the daemon directly -- this client does not send or parse tool calls yet; that is
// Phase 2.
//
// The cost is real and is handled in ChatRequest::num_ctx -- being able to set the
// context means being able to set it past what the GPU can hold, which Ollama will
// not stop you from doing.

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace hermes::ollama {

enum class TransportError {
  Unreachable,      // connection refused, no route -- Ollama is probably not running
  Timeout,          // connected, but no response inside the budget
  HttpStatus,       // reached the server, got a non-2xx back
  MalformedJson,    // 2xx whose body did not parse
  UnexpectedShape,  // parsed, but not the structure the endpoint documents
};

std::string_view to_string(TransportError e) noexcept;

/// A failed request, carrying enough detail to print one actionable line at startup.
struct Failure {
  TransportError kind;
  std::string detail;

  [[nodiscard]] std::string message() const;
};

template <typename T>
using Result = std::expected<T, Failure>;

/// One entry from /api/tags.
struct ModelTag {
  std::string name;  // as Ollama reports it, ":latest" suffix included
  std::uint64_t size_bytes = 0;
  std::string modified_at;
};

/// The parts of /api/show that R9 turns on. Everything here is reported as Ollama
/// gave it, including the absences -- the preflight, not the parser, decides what an
/// absence means.
struct ModelCard {
  /// Ollama's own capability list, e.g. ["completion", "vision", "tools", "thinking"].
  std::vector<std::string> capabilities;

  /// True when /api/show returned a `capabilities` key at all. An older Ollama that
  /// never reports capabilities is a different situation from a model that genuinely
  /// lacks `tools`, and R9 must not silently merge them into one "no".
  bool reported_capabilities = false;

  /// `num_ctx` from the Modelfile parameter block.
  ///
  /// Informational only. Since D8 this client sends its own `options.num_ctx`, which
  /// overrides a pin upward -- measured: a model pinned to 8192, asked for 32768,
  /// loaded at 32768. So a pin neither guarantees nor limits anything here. Absent
  /// simply means the Modelfile did not set one; it is *not* evidence that the model
  /// runs at 4096, which is what an earlier revision of this comment claimed.
  std::optional<std::uint64_t> num_ctx;

  /// `<arch>.context_length` from model_info: the context the model was built for.
  ///
  /// This is the gate (see preflight.h), because it is the one limit no per-request
  /// setting can exceed. On qwen35-agent:latest it reads 262144 while the Modelfile
  /// pins 131072 -- the two are genuinely different numbers, which is what makes
  /// reading the wrong one a real mistake rather than a pedantic one.
  std::optional<std::uint64_t> architecture_context;

  std::string family;
  std::string parameter_size;
  std::string quantization;

  [[nodiscard]] bool has_capability(std::string_view name) const;
};

struct ChatMessage {
  std::string role;
  std::string content;
};

struct ChatRequest {
  std::string model;
  std::vector<ChatMessage> messages;
  double temperature = 0.0;

  /// Generation cap, sent as `options.num_predict`.
  ///
  /// ⚠ Spent on reasoning first on a thinking model. Too small a value returns empty
  /// content with no error -- see ChatReply::completion_tokens.
  std::optional<int> max_tokens;

  /// Context window for this request, sent as `options.num_ctx`.
  ///
  /// ⚠ **Clamped to ClientOptions::max_num_ctx before it is sent, and that clamp is
  /// not optional.** Ollama applies whatever it is given with no check that the KV
  /// cache will fit in VRAM. Asking for 262144 on a 29 GB model against a 48 GB card
  /// hard-froze `kitchen-desktop` on 2026-08-13: the request was accepted, amdgpu
  /// thrashed evicting and restoring ranges, and the machine died with no OOM kill
  /// and no GPU reset. Being able to set this is the reason for using /api/chat at
  /// all; the clamp is the price.
  ///
  /// Absent means "say nothing", which leaves the server default in force -- exactly
  /// the undetermined state R9 refuses. Set it.
  std::optional<std::uint64_t> num_ctx;

  /// JSON schema for constrained decoding (D5). Sent as `format`, which takes the
  /// schema directly rather than the OpenAI wrapper.
  std::optional<nlohmann::json> schema;
};

struct ChatReply {
  std::string content;

  /// Thinking-model output, which the native endpoint returns in its own `thinking`
  /// field rather than inside content (the OpenAI-compatible one called it `reasoning`). Kept rather than dropped: a model that "returned nothing"
  /// has usually reasoned itself out of its token budget, and that is invisible
  /// without this.
  std::string reasoning;

  std::string finish_reason;
  std::uint64_t prompt_tokens = 0;

  /// From `eval_count`. **Includes thinking tokens**, as does the `num_predict`
  /// budget — the two agree, and a budget built on this field does not undercount.
  ///
  /// An earlier revision of this comment claimed the opposite. It was wrong, and the
  /// re-measurement is recorded here because the mistake is an easy one to repeat.
  /// Model gemma31-agent, prompt "Give me a city and its country.", `num_ctx` 32768,
  /// `temperature` 0, varying only the budget:
  ///
  ///   num_predict=20   done=length  eval_count=20   thinking=74 ch   content=0 ch
  ///   num_predict=80   done=length  eval_count=80   thinking=274 ch  content=0 ch
  ///   num_predict=300  done=stop    eval_count=148  thinking=487 ch  content=12 ch
  ///
  /// The first two rows settle it on their own: content is *empty* while `eval_count`
  /// is 20 and 80. A count covering only content would read 0. The third row agrees —
  /// 487+12 characters is roughly 125 tokens against a reported 148.
  ///
  /// Identical on /v1/chat/completions for the same generation (148 there too), so it
  /// is Ollama's accounting and not an artifact of either API shape.
  ///
  /// What survives, and it is the part that matters for R8: **a budget too small for
  /// the thinking returns empty content with no error at all.** The model reasons
  /// first, exhausts the budget, and writes nothing. Two tells, both reliable:
  /// `done_reason == "length"` (see truncated()), and `eval_count == num_predict`.
  std::uint64_t completion_tokens = 0;

  /// True when generation stopped at the token budget rather than because the model
  /// finished. Worth checking before trusting an empty `content`.
  [[nodiscard]] bool truncated() const { return finish_reason == "length"; }

  /// Whether the model demonstrably produced tokens.
  ///
  /// Empty content alone does not mean nothing happened -- a thinking model spends
  /// its budget reasoning first -- so this asks the broader question. Used by the
  /// warmup check, which otherwise passes on a well-formed reply containing no
  /// evidence that the model ran at all.
  [[nodiscard]] bool generated() const {
    return completion_tokens > 0 || !content.empty() || !reasoning.empty();
  }
};

/// Deliberately at namespace scope rather than nested inside Client. As a nested type
/// it cannot be used as a defaulted argument -- `Client(Options = {})` is rejected by
/// gcc 15.2, the only compiler this has been built with, though it is accepted once
/// the same struct sits outside the class. Not worth a two-constructor workaround to
/// keep the nesting.
struct ClientOptions {
  std::string base_url = "http://localhost:11434";

  /// Short by design: this is a loopback connect, so anything slow is a wedged
  /// daemon rather than a slow network, and waiting longer only delays the report.
  std::chrono::seconds connect_timeout{2};

  /// Metadata reads. /api/show can be slow the first time a large model is touched.
  std::chrono::seconds metadata_timeout{30};

  /// Generation. Generous because local generation genuinely is slow; R8's real
  /// per-turn budget lands on top of this rather than replacing it.
  std::chrono::seconds chat_timeout{600};

  /// Hard ceiling on any `num_ctx` this client will send. **A safety limit, not a
  /// tuning knob.**
  ///
  /// Ollama performs no admission control on context size, so an oversized request
  /// is not rejected or reduced -- it takes the machine down (see
  /// ChatRequest::num_ctx). Nothing in the API reports how much VRAM is free, so this
  /// client cannot compute a safe value; it can only refuse to exceed one it was
  /// told. 65536 is the default because that is what the bench/fsops runs used -- on the
  /// MSI laptop's 16 GB RTX 5080, note, not on this 48 GB W7900. A value known to work on
  /// a smaller card, not one derived from this one. Raise it deliberately, against a
  /// specific card, and never above what that card can hold.
  std::uint64_t max_num_ctx = 65536;
};

/// Blocking and single-threaded, per D1. One Client owns one connection; it is not
/// safe to share across threads, and under D1 there are none to share it with.
class Client {
 public:
  using Options = ClientOptions;

  /// The only way to build a Client, so a rejected URL is a Failure to report rather
  /// than an exception out of a constructor. httplib's own client throws on a scheme
  /// it does not know -- `--url https://...`, the likeliest typo for this flag, used
  /// to reach the user as an abort with a C++ exception message.
  ///
  /// Also where D7's loopback commitment is *enforced* rather than merely stated.
  /// This client carries sandbox file contents to the model, and a single flag
  /// pointing at a remote host would send them off the machine; a forged /api/tags
  /// and /api/show is enough to make the whole preflight pass. Rejecting a non-local
  /// host here is what makes the layering claim true instead of aspirational.
  [[nodiscard]] static std::expected<Client, Failure> open(ClientOptions options = {});

  ~Client();

  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  [[nodiscard]] const std::string& base_url() const noexcept { return options_.base_url; }

  /// GET /api/tags -- every installed model.
  [[nodiscard]] Result<std::vector<ModelTag>> tags() const;

  /// POST /api/show -- metadata for one model.
  [[nodiscard]] Result<ModelCard> show(std::string_view model) const;

  /// POST /v1/chat/completions -- one non-streamed completion.
  [[nodiscard]] Result<ChatReply> chat(const ChatRequest& request) const;

 private:
  explicit Client(ClientOptions options);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  ClientOptions options_;
};

// --- parsing, exposed for testing -------------------------------------------
// These are pure functions over bytes Ollama sent. They are declared here rather than
// hidden in the .cpp so the suite can exercise the response shapes that matter --
// missing keys, a model that was never pinned, an unparseable parameter block --
// without a live daemon and without the mocking machinery it would otherwise take.

/// Pull `num_ctx` out of a Modelfile parameter block:
///
///     repeat_penalty                 1
///     num_ctx                        131072
///
/// Returns nullopt when the key is absent, which is the case that matters most.
/// Matches whole keys only, so `foo_num_ctx` is not `num_ctx`. On a value too large
/// for uint64 returns nullopt: an unreadable pin is not a satisfied pin.
[[nodiscard]] std::optional<std::uint64_t> parse_num_ctx(std::string_view parameters);

/// Enforce D7: an `http://` URL naming a loopback host, and nothing else.
///
/// Accepts localhost, 127.0.0.0/8 and [::1], with an optional port. Rejects https
/// (httplib is built without TLS, so it could only throw), any non-local host, and a
/// trailing path -- httplib silently discards the path, so accepting one would print
/// a URL in the report that is not the URL being talked to.
[[nodiscard]] std::expected<void, Failure> validate_base_url(std::string_view url);

/// Build the `options` object for /api/chat, applying the num_ctx clamp.
///
/// Pure, and exposed for testing on purpose: the clamp is a safety property, and the
/// only other way to check it is to send an oversized request to a real daemon --
/// which is the exact action that hard-froze the machine. Testing it here means the
/// guard against that failure never has to reproduce it.
[[nodiscard]] nlohmann::json build_chat_options(const ChatRequest& request,
                                                std::uint64_t max_num_ctx);

[[nodiscard]] Result<ModelCard> parse_model_card(const nlohmann::json& body);
[[nodiscard]] Result<std::vector<ModelTag>> parse_tags(const nlohmann::json& body);
[[nodiscard]] Result<ChatReply> parse_chat_reply(const nlohmann::json& body);

/// Ollama treats "qwen35-agent" and "qwen35-agent:latest" as the same model, and
/// /api/tags always reports the suffix while users rarely type it.
[[nodiscard]] std::string_view strip_latest(std::string_view model) noexcept;

}  // namespace hermes::ollama
