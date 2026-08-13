#include <hermes/ollama/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <string>
#include <utility>

#include <httplib.h>

namespace hermes::ollama {
namespace {

using nlohmann::json;

Failure transport_failure(httplib::Error error, std::string_view what) {
  const std::string where{what};
  switch (error) {
    case httplib::Error::Connection:
    case httplib::Error::ConnectionTimeout:
      // By far the most common failure in practice, and the one worth naming
      // precisely: Ollama is not running, or is not on the port we were told.
      return {TransportError::Unreachable, where + ": " + httplib::to_string(error)};
    case httplib::Error::Read:
    case httplib::Error::Write:
      return {TransportError::Timeout, where + ": " + httplib::to_string(error)};
    default:
      return {TransportError::Unreachable, where + ": " + httplib::to_string(error)};
  }
}

/// Serialise without throwing.
///
/// nlohmann's dump() throws on any byte that is not valid UTF-8, and this API returns
/// std::expected rather than throwing -- so an un-replaced dump() is not an error
/// path, it is std::terminate. That matters here more than in most codebases: the
/// supervisor's whole job is feeding file contents and tool output back to a model,
/// and one latin-1 or binary file would take the process down. Invalid bytes become
/// U+FFFD, which the model can see and reason about.
std::string dump_lossy(const json& value) {
  return value.dump(-1, ' ', /*ensure_ascii=*/false, json::error_handler_t::replace);
}

/// Restores the client's timeouts however the scope is left.
///
/// chat() raises them for the length of one generation. Without this, an exception
/// or an early return leaves the 600 s budget installed on the shared connection,
/// and the next metadata read inherits it -- which is precisely the ten-minute
/// startup hang the short metadata timeout exists to prevent.
class TimeoutScope {
 public:
  TimeoutScope(httplib::Client& http, std::chrono::seconds active,
               std::chrono::seconds restore)
      : http_(http), restore_(restore) {
    http_.set_read_timeout(active);
    http_.set_write_timeout(active);
  }
  ~TimeoutScope() {
    http_.set_read_timeout(restore_);
    http_.set_write_timeout(restore_);
  }
  TimeoutScope(const TimeoutScope&) = delete;
  TimeoutScope& operator=(const TimeoutScope&) = delete;

 private:
  httplib::Client& http_;
  std::chrono::seconds restore_;
};

/// Parse a 2xx body, or say precisely which of the two ways it was wrong.
Result<json> parse_body(const httplib::Result& response, std::string_view what) {
  if (!response) return std::unexpected(transport_failure(response.error(), what));

  if (response->status < 200 || response->status >= 300) {
    // Ollama puts a useful message in the body on 404 ("model not found") and 400.
    // Truncated because a stack trace in a startup error helps nobody.
    std::string detail = std::string{what} + ": HTTP " + std::to_string(response->status);
    if (!response->body.empty()) detail += " -- " + response->body.substr(0, 400);
    return std::unexpected(Failure{TransportError::HttpStatus, std::move(detail)});
  }

  json parsed = json::parse(response->body, nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return std::unexpected(Failure{TransportError::MalformedJson,
                                   std::string{what} + ": response body did not parse as JSON"});
  }
  return parsed;
}

Failure wrong_shape(std::string_view what) {
  return {TransportError::UnexpectedShape, std::string{what}};
}

std::string_view trim(std::string_view text) noexcept {
  const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!text.empty() && is_space(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && is_space(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return text;
}

/// Read a JSON value that Ollama may report as either a number or a string.
std::optional<std::uint64_t> as_uint(const json& value) {
  if (value.is_number_unsigned()) return value.get<std::uint64_t>();
  if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) return std::nullopt;
    return static_cast<std::uint64_t>(signed_value);
  }
  return std::nullopt;
}

std::string string_or_empty(const json& object, std::string_view key) {
  const auto found = object.find(key);
  if (found == object.end() || !found->is_string()) return {};
  return found->get<std::string>();
}

}  // namespace

std::string_view to_string(TransportError e) noexcept {
  switch (e) {
    case TransportError::Unreachable:     return "cannot reach Ollama";
    case TransportError::Timeout:         return "Ollama timed out";
    case TransportError::HttpStatus:      return "Ollama returned an error status";
    case TransportError::MalformedJson:   return "Ollama returned unparseable JSON";
    case TransportError::UnexpectedShape: return "Ollama returned an unexpected structure";
  }
  return "unknown transport error";
}

std::string Failure::message() const {
  std::string text{to_string(kind)};
  if (!detail.empty()) text += " (" + detail + ")";
  return text;
}

std::string_view strip_latest(std::string_view model) noexcept {
  constexpr std::string_view kSuffix = ":latest";
  if (model.size() > kSuffix.size() && model.ends_with(kSuffix)) {
    model.remove_suffix(kSuffix.size());
  }
  return model;
}

bool ModelCard::has_capability(std::string_view name) const {
  return std::ranges::find(capabilities, name) != capabilities.end();
}

std::optional<std::uint64_t> parse_num_ctx(std::string_view parameters) {
  constexpr std::string_view kKey = "num_ctx";
  std::optional<std::uint64_t> found;

  // Line-oriented rather than a regex: the format is one `key<spaces>value` per line,
  // and a hand-rolled scan makes the whole-key requirement obvious. `foo_num_ctx` must
  // not match, because silently reading someone else's parameter as the context pin is
  // exactly the kind of near-miss R9 exists to catch.
  //
  // Last occurrence wins, matching Ollama's own last-wins handling of a repeated
  // Modelfile PARAMETER.
  while (!parameters.empty()) {
    const auto break_at = parameters.find('\n');
    const std::string_view line = trim(parameters.substr(0, break_at));
    parameters = (break_at == std::string_view::npos) ? std::string_view{}
                                                      : parameters.substr(break_at + 1);

    if (!line.starts_with(kKey)) continue;
    std::string_view rest = line.substr(kKey.size());
    // The key must be followed by whitespace, or it was a longer key that merely
    // started the same way.
    if (rest.empty() || std::isspace(static_cast<unsigned char>(rest.front())) == 0) continue;
    rest = trim(rest);
    if (rest.empty()) continue;

    std::uint64_t value = 0;
    const auto* const first = rest.data();
    const auto* const last = rest.data() + rest.size();
    const auto [stop, error] = std::from_chars(first, last, value);
    // Trailing junk, a negative, or a value past uint64 all land here. An unreadable
    // pin is reported as no pin: the caller then treats it as unpinned and refuses,
    // rather than guessing a number.
    //
    // It must *clear* rather than skip. Under last-wins, an unreadable final pin is
    // the one in force, so keeping the earlier readable value would report a pin that
    // has been superseded -- passing the gate on data that could not be read, which is
    // the fail-open shape this whole file is written against.
    if (error != std::errc{} || stop != last) {
      found.reset();
      continue;
    }
    found = value;
  }
  return found;
}

Result<ModelCard> parse_model_card(const json& body) {
  if (!body.is_object()) return std::unexpected(wrong_shape("/api/show: body is not an object"));

  ModelCard card;

  if (const auto capabilities = body.find("capabilities"); capabilities != body.end()) {
    if (!capabilities->is_array()) {
      return std::unexpected(wrong_shape("/api/show: capabilities is not an array"));
    }
    card.reported_capabilities = true;
    for (const auto& entry : *capabilities) {
      if (entry.is_string()) card.capabilities.push_back(entry.get<std::string>());
    }
  }

  if (const auto parameters = body.find("parameters"); parameters != body.end()) {
    if (parameters->is_string()) card.num_ctx = parse_num_ctx(parameters->get<std::string>());
  }

  if (const auto info = body.find("model_info"); info != body.end() && info->is_object()) {
    // The key is architecture-qualified -- "qwen35moe.context_length", "gemma3.context_length"
    // -- so it is found by suffix rather than by name. Only one is ever present.
    for (const auto& [key, value] : info->items()) {
      if (key.ends_with(".context_length")) {
        card.architecture_context = as_uint(value);
        break;
      }
    }
  }

  if (const auto details = body.find("details"); details != body.end() && details->is_object()) {
    card.family = string_or_empty(*details, "family");
    card.parameter_size = string_or_empty(*details, "parameter_size");
    card.quantization = string_or_empty(*details, "quantization_level");
  }

  return card;
}

Result<std::vector<ModelTag>> parse_tags(const json& body) {
  if (!body.is_object()) return std::unexpected(wrong_shape("/api/tags: body is not an object"));
  const auto models = body.find("models");
  if (models == body.end() || !models->is_array()) {
    return std::unexpected(wrong_shape("/api/tags: no `models` array"));
  }

  std::vector<ModelTag> found;
  found.reserve(models->size());
  for (const auto& entry : *models) {
    if (!entry.is_object()) continue;
    ModelTag tag;
    tag.name = string_or_empty(entry, "name");
    if (tag.name.empty()) continue;
    if (const auto size = entry.find("size"); size != entry.end()) {
      tag.size_bytes = as_uint(*size).value_or(0);
    }
    tag.modified_at = string_or_empty(entry, "modified_at");
    found.push_back(std::move(tag));
  }
  return found;
}

json build_chat_options(const ChatRequest& request, std::uint64_t max_num_ctx) {
  json options{{"temperature", request.temperature}};
  if (request.max_tokens) options["num_predict"] = *request.max_tokens;

  // The clamp. Ollama accepts any num_ctx it is given and makes no attempt to check
  // that the KV cache fits in VRAM; exceeding what the card holds does not fail, it
  // deadlocks the driver and takes the machine with it. Nothing in the API reports
  // free VRAM, so this cannot compute a safe number -- only refuse to exceed one.
  if (request.num_ctx) options["num_ctx"] = std::min(*request.num_ctx, max_num_ctx);

  return options;
}

Result<ChatReply> parse_chat_reply(const json& body) {
  if (!body.is_object()) {
    return std::unexpected(wrong_shape("/api/chat: body is not an object"));
  }

  // A reply with no `message` is not a completion. Accepting one made the warmup check
  // pass against an empty envelope, which proves only that something answered on the
  // port -- the question /api/tags already settled.
  const auto message = body.find("message");
  if (message == body.end() || !message->is_object()) {
    return std::unexpected(wrong_shape("/api/chat: no `message`"));
  }

  ChatReply reply;

  // Native names, which differ from the OpenAI-compatible ones this used to read:
  // done_reason not finish_reason, thinking not reasoning, and the token counts sit
  // at the top level rather than under `usage`.
  reply.finish_reason = string_or_empty(body, "done_reason");

  // A model that returns only tool calls sends content: null, which is a legitimate
  // reply and not a parse failure. Tool calls themselves land in Phase 2 with the
  // agent loop; this reads the text half only.
  reply.content = string_or_empty(*message, "content");
  reply.reasoning = string_or_empty(*message, "thinking");

  if (const auto prompt = body.find("prompt_eval_count"); prompt != body.end()) {
    reply.prompt_tokens = as_uint(*prompt).value_or(0);
  }
  if (const auto completion = body.find("eval_count"); completion != body.end()) {
    reply.completion_tokens = as_uint(*completion).value_or(0);
  }
  return reply;
}

// --- the client --------------------------------------------------------------

struct Client::Impl {
  explicit Impl(const ClientOptions& options) : http(options.base_url) {
    http.set_connection_timeout(options.connect_timeout);
    http.set_read_timeout(options.metadata_timeout);
    http.set_write_timeout(options.metadata_timeout);
    // Loopback only, per D7. A redirect would mean something is answering that is not
    // the Ollama we were pointed at, so it is not followed.
    http.set_follow_location(false);
    http.set_keep_alive(true);
  }

  mutable httplib::Client http;
};

namespace {

/// True only for a host that genuinely names this machine.
///
/// The first version of this asked `host.starts_with("127.")`, which is a *string*
/// test where an *address* test was needed: `127.evil.com` is a perfectly registrable
/// domain that satisfies it, resolves wherever its owner points it, and would have
/// been handed sandbox file contents. Parsing the octets is what closes that.
bool is_loopback_host(std::string_view host) {
  if (host == "localhost" || host == "::1") return true;

  // A trailing dot makes this a root-anchored DNS name rather than an address
  // literal, and resolution of "127.0.0.1." is the resolver's business, not ours.
  // An IPv4 literal never carries one, so it is simply not this.
  if (host.ends_with('.')) return false;

  // A dotted quad in 127.0.0.0/8, and nothing else. Every component must be numeric,
  // so any label that is not a number -- the whole trick above -- fails here.
  std::array<unsigned, 4> octets{};
  std::size_t count = 0;
  while (!host.empty() && count < octets.size()) {
    const auto dot = host.find('.');
    const std::string_view part = host.substr(0, dot);
    if (part.empty()) return false;

    unsigned value = 0;
    const auto* const last = part.data() + part.size();
    const auto [stop, error] = std::from_chars(part.data(), last, value);
    if (error != std::errc{} || stop != last || value > 255) return false;

    octets[count++] = value;
    if (dot == std::string_view::npos) {
      host = {};
      break;
    }
    host = host.substr(dot + 1);
  }

  return host.empty() && count == 4 && octets[0] == 127;
}

}  // namespace

std::expected<void, Failure> validate_base_url(std::string_view url) {
  constexpr std::string_view kScheme = "http://";
  const auto reject = [](std::string detail) {
    return std::unexpected(Failure{TransportError::Unreachable, std::move(detail)});
  };

  if (!url.starts_with(kScheme)) {
    return reject("base URL must begin with http:// (this build has no TLS, and D7 "
                  "keeps inference on loopback): " +
                  std::string{url});
  }
  std::string_view rest = url.substr(kScheme.size());

  // httplib ignores any path, so accepting one would mean reporting a URL that is not
  // the one being used.
  if (const auto slash = rest.find('/');
      slash != std::string_view::npos && slash + 1 != rest.size()) {
    return reject("base URL must not carry a path; Ollama is addressed at the root: " +
                  std::string{url});
  }
  rest = rest.substr(0, rest.find('/'));

  // Userinfo would let the real host hide behind a loopback-looking prefix:
  // "http://127.0.0.1:11434@example.com/" talks to example.com while the report
  // prints the whole string. Refused outright rather than parsed.
  if (rest.contains('@')) {
    return reject("base URL must not contain userinfo, which hides the real host: " +
                  std::string{url});
  }

  std::string_view host = rest;
  if (host.starts_with('[')) {  // [::1]:11434
    const auto close = host.find(']');
    if (close == std::string_view::npos) return reject("malformed IPv6 host: " + std::string{url});
    host = host.substr(1, close - 1);
  } else if (const auto colon = host.rfind(':'); colon != std::string_view::npos) {
    host = host.substr(0, colon);
  }

  if (!is_loopback_host(host)) {
    return reject("D7 permits loopback Ollama only, and this client carries sandbox file "
                  "contents; refusing host \"" +
                  std::string{host} + "\"");
  }
  return {};
}

Client::Client(ClientOptions options)
    : impl_(std::make_unique<Impl>(options)), options_(std::move(options)) {}

std::expected<Client, Failure> Client::open(ClientOptions options) {
  if (auto ok = validate_base_url(options.base_url); !ok) {
    return std::unexpected(ok.error());
  }
  // Validated above, so httplib has no unknown scheme left to throw on. Caught anyway:
  // a constructor that aborts the process is not an acceptable failure mode for a
  // value the user typed.
  try {
    return Client{std::move(options)};
  } catch (const std::exception& e) {
    return std::unexpected(Failure{TransportError::Unreachable,
                                   std::string{"could not open HTTP client: "} + e.what()});
  }
}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

Result<std::vector<ModelTag>> Client::tags() const {
  auto body = parse_body(impl_->http.Get("/api/tags"), "/api/tags");
  if (!body) return std::unexpected(body.error());
  return parse_tags(*body);
}

Result<ModelCard> Client::show(std::string_view model) const {
  const json request{{"model", std::string{model}}};
  auto body = parse_body(impl_->http.Post("/api/show", dump_lossy(request), "application/json"),
                         "/api/show");
  if (!body) return std::unexpected(body.error());
  return parse_model_card(*body);
}

Result<ChatReply> Client::chat(const ChatRequest& request) const {
  json messages = json::array();
  // Reserved rather than grown 1->2->4->8. Irrelevant for preflight's single nine-byte
  // message, but this is the agent loop's hot path once file contents flow through it,
  // where the payload is held three times over anyway (this array, the DOM, the dumped
  // string) and doubling reallocations on top of that is pure waste.
  messages.get_ref<json::array_t&>().reserve(request.messages.size());
  for (const auto& message : request.messages) {
    messages.push_back({{"role", message.role}, {"content", message.content}});
  }

  json payload{
      {"model", request.model},
      {"messages", std::move(messages)},
      {"options", build_chat_options(request, options_.max_num_ctx)},
      {"stream", false},  // D1: blocking and single-threaded, so there is nothing to stream into
  };
  // Constrained decoding (D5). The native endpoint takes the schema directly, with no
  // OpenAI-style {"type":"json_schema","json_schema":{...}} wrapper around it.
  if (request.schema) payload["format"] = *request.schema;

  // Generation needs the long budget; metadata reads must not, or a wedged daemon
  // would hold startup for ten minutes instead of failing in two seconds. Scoped, so
  // the restore happens on every exit and not just the happy one.
  Result<json> body = [&] {
    const TimeoutScope budget{impl_->http, options_.chat_timeout, options_.metadata_timeout};
    return parse_body(impl_->http.Post("/api/chat", dump_lossy(payload), "application/json"),
                      "/api/chat");
  }();

  if (!body) return std::unexpected(body.error());
  return parse_chat_reply(*body);
}

}  // namespace hermes::ollama
