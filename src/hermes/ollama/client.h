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
// Native Ollama endpoints throughout (`/api/tags`, `/api/show`, `/api/chat`), not the
// OpenAI-compatible one -- the latter cannot set `num_ctx`, which leaves the context
// window an unknowable property of the server. Full reasoning and the driver-deadlock
// incident that makes the cost concrete: DECISIONS.md D8.
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
#include <variant>
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

/// One tool call, as the model asked for it.
///
/// `arguments` arrives as a structured object on the native endpoint rather than as a
/// JSON string needing a second parse -- one of the two gains D8 recorded when it moved
/// off /v1/chat/completions. It is held as `json` rather than decoded here because this
/// layer does not know what any tool's arguments mean; `hermes::supervisor` translates
/// it into `RawArgs` against the tool's own declaration (ROUTING.md section 7).
///
/// `id` is echoed back verbatim in history. Measured on the fsops model set: none of
/// the seven chat templates render it, so it is carried for fidelity to the wire format
/// rather than because anything depends on it.
struct ToolCall {
  std::string id;
  std::string name;
  nlohmann::json arguments;  // object; `{}` for a no-argument call
};

struct ChatMessage {
  std::string role;
  std::string content;

  /// The tool this message reports the result of. Set only on `role == "tool"`.
  ///
  /// ⚠ Measured 2026-08-17 on Ollama 0.32.9, all five models probed: sending this
  /// changes nothing. `prompt_eval_count` was byte-identical with and without it
  /// (qwen3.5-9b 407/407, llama3.2-3b 169/169, hermes3-8b 319/319, granite4-7b
  /// 265/265, gemma4-e4b 161/161), so every template on this set drops it. Sent
  /// anyway because it is the field the native API documents and it costs nothing --
  /// but nothing here may be built on the model seeing it.
  ///
  /// Both this and `tool_calls` carry an explicit `{}` so that the many text-only
  /// messages built with designated initialisers -- every one in session.cpp --
  /// do not have to name them, and do not warn for omitting them.
  std::string tool_name{};

  /// The calls this assistant turn made, echoed back so the conversation stays
  /// coherent.
  ///
  /// Required, and not for tidiness: a tool result whose call is missing from history
  /// leaves the model answering a question it cannot see. Measured -- with the call
  /// echoed, all five models consumed the results and moved on; that is the sequence
  /// the agent loop emits. Set only on `role == "assistant"`.
  std::vector<ToolCall> tool_calls{};
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

  /// A tool surface to offer, or a response schema to constrain to -- **never both**.
  ///
  /// D12: `tools` and `format` are mutually exclusive on /api/chat, and sending both
  /// does not merely waste the schema -- it breaks tool calling on most models.
  /// Measured 2026-08-17, Ollama 0.32.9, temperature 0, two repeats, one `write` tool
  /// against a schema describing that same call:
  ///
  ///   model         tools alone   tools + format
  ///   qwen3.5-9b    correct       correct    (format ignored; eval_count identical)
  ///   qwen3.5-4b    correct       correct    (format ignored; eval_count identical)
  ///   gemma4-e4b    correct       correct
  ///   llama3.2-3b   correct       CORRUPT    arguments became the schema object
  ///   llama3.1-8b   correct       CORRUPT    arguments became the schema object
  ///   hermes3-8b    correct       NO CALL    the JSON landed in `content`
  ///   granite4-7b   correct       NO CALL    the JSON landed in `content`
  ///
  /// Seven of seven are correct with `tools` alone; four of seven break when `format`
  /// is added. The two NO CALL rows are R2's original failure exactly -- a whole tool
  /// call emitted as text that was never parsed as a call -- so the mechanism R2 asked
  /// for reintroduces the failure R2 was written about, on the very model
  /// (`llama3.2:3b`) that supplied its evidence.
  ///
  /// Hence a variant rather than two optional fields: the combination is
  /// unrepresentable, not merely discouraged. `Schema` remains the right shape for a
  /// structured *reply* with no tools offered, which is what D5 is retargeted to and
  /// what Tier 1's `triage` and `summarize` will use -- measured working in the same
  /// run.
  struct Tools {
    std::vector<nlohmann::json> definitions;
  };
  struct Schema {
    nlohmann::json schema;
  };
  std::variant<std::monostate, Tools, Schema> constraint;
};

struct ChatReply {
  std::string content;

  /// What the model asked to call, in the order it asked.
  ///
  /// Order and multiplicity both matter and are not hypothetical. Measured on one
  /// prompt naming two files: qwen3.5-9b and hermes3-8b each emitted 2 calls,
  /// llama3.2-3b emitted **3** -- `read a.txt`, `read b.txt`, then `read a.txt`
  /// again. A loop assuming one call per turn would silently drop work, and one
  /// assuming distinct calls would be wrong on the third. Empty is the normal case
  /// for a turn that answered in prose.
  std::vector<ToolCall> tool_calls;

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

  /// The ceiling this client will impose on any `num_ctx` it sends (D8).
  ///
  /// Exposed so a caller planning a context budget can read the limit from the client
  /// that will enforce it, instead of from a `ClientOptions` that merely ought to be
  /// the same one. A budget planned against a larger window than gets sent overflows,
  /// and the server's response to overflow is to discard most of the prompt silently.
  [[nodiscard]] std::uint64_t max_num_ctx() const noexcept { return options_.max_num_ctx; }

  /// GET /api/tags -- every installed model.
  [[nodiscard]] Result<std::vector<ModelTag>> tags() const;

  /// POST /api/show -- metadata for one model.
  [[nodiscard]] Result<ModelCard> show(std::string_view model) const;

  /// POST /api/chat -- one non-streamed completion.
  ///
  /// (Said `/v1/chat/completions` until 2026-08-13, which had been wrong since D8 moved
  /// this to the native endpoint; the file's own header comment and the implementation
  /// both said `/api/chat` while this line disagreed.)
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

/// Serialise JSON without throwing on bytes that are not valid UTF-8.
///
/// **Use this, never `json::dump()`, for anything carrying file content or a path.**
/// nlohmann's default dump handler throws `type_error.316` on the first invalid byte, and
/// this project's whole job is feeding file contents and tool output back to a model: one
/// latin-1 or binary file takes the process down with an uncaught exception. Invalid bytes
/// become U+FFFD instead, which the model can see and reason about.
///
/// Lives here, in the layer D2 put nlohmann in, because two layers need it -- the request
/// body and `supervisor/wire.cpp`'s tool results. It was private to client.cpp until
/// 2026-08-17, when `wire.cpp` was written with a bare `dump()` and crashed on a `read` of
/// a two-byte binary file. Duplicating the helper would have let that happen a third time.
[[nodiscard]] std::string dump_lossy(const nlohmann::json& value);

/// The whole /api/chat request body, as it goes on the wire.
///
/// Exposed for the same reason the parsers are: what this renders is where D12 is
/// enforced and where an assistant turn's `tool_calls` are rebuilt, and neither is
/// worth reaching a live daemon to check. `max_num_ctx` is passed rather than read from
/// a Client so the function stays pure.
[[nodiscard]] nlohmann::json build_chat_payload(const ChatRequest& request,
                                                std::uint64_t max_num_ctx);

/// Decode a `message.tool_calls` array into ToolCall values.
///
/// Skips entries that are not objects and entries whose function name is empty -- a
/// nameless call cannot be dispatched and is not worth propagating as a half-value.
/// Arguments that are absent or not an object become `{}`, which `parse_args` then
/// refuses for any tool that declares a required argument: the refusal belongs to the
/// layer that knows the declaration, not to this one.
[[nodiscard]] std::vector<ToolCall> parse_tool_calls(const nlohmann::json& calls);

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
