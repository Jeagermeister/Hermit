#include <hermit/ollama/client.h>

#include <gtest/gtest.h>

#include <limits>
#include <string>

using hermit::ollama::ChatReply;
using hermit::ollama::ModelCard;
using hermit::ollama::parse_chat_reply;
using hermit::ollama::parse_model_card;
using hermit::ollama::parse_num_ctx;
using hermit::ollama::parse_tags;
using hermit::ollama::strip_latest;
using hermit::ollama::TransportError;
using json = nlohmann::json;

namespace {

// The parameter block exactly as `/api/show` returned it for qwen35-agent:latest on
// kitchen-desktop, 2026-08-13. Captured rather than invented: the whitespace run
// between key and value is what the parser has to survive, and a hand-written sample
// would have quietly used one space.
constexpr const char* kRealParameters =
    "repeat_penalty                 1\n"
    "temperature                    1\n"
    "top_k                          20\n"
    "top_p                          0.95\n"
    "min_p                          0\n"
    "num_ctx                        131072\n"
    "presence_penalty               1.5";

}  // namespace

// --- parse_num_ctx -----------------------------------------------------------

TEST(ParseNumCtx, ReadsARealParameterBlock) {
  EXPECT_EQ(parse_num_ctx(kRealParameters), 131072u);
}

TEST(ParseNumCtx, AbsentKeyIsNullopt) {
  // The case R9 turns on: no pin at all. The caller must see "absent", not "0".
  EXPECT_FALSE(parse_num_ctx("temperature 1\ntop_k 20").has_value());
  EXPECT_FALSE(parse_num_ctx("").has_value());
}

TEST(ParseNumCtx, LongerKeysStartingTheSameWayDoNotMatch) {
  // A substring match here would read someone else's parameter as the context pin.
  EXPECT_FALSE(parse_num_ctx("num_ctx_override 262144").has_value());
  EXPECT_FALSE(parse_num_ctx("num_ctxx 8192").has_value());
}

TEST(ParseNumCtx, KeysEndingInNumCtxDoNotMatch) {
  EXPECT_FALSE(parse_num_ctx("foo_num_ctx 8192").has_value());
}

TEST(ParseNumCtx, TolerantOfSurroundingWhitespaceAndCarriageReturns) {
  EXPECT_EQ(parse_num_ctx("\n\n   num_ctx\t65536   \n\n"), 65536u);
  EXPECT_EQ(parse_num_ctx("num_ctx 65536\r\ntop_k 20"), 65536u);
}

TEST(ParseNumCtx, SingleLineWithNoTrailingNewline) {
  EXPECT_EQ(parse_num_ctx("num_ctx 65536"), 65536u);
}

TEST(ParseNumCtx, LastOccurrenceWins) {
  // Matches Ollama's own last-wins handling of a repeated Modelfile PARAMETER.
  EXPECT_EQ(parse_num_ctx("num_ctx 4096\nnum_ctx 131072"), 131072u);
}

TEST(ParseNumCtx, UnreadableValuesAreReportedAsNoPin) {
  // Each of these would be a silent 0 under a naive atoi, and 0 compares below any
  // floor -- which happens to fail correctly, but for the wrong reason. Reporting
  // "no pin" keeps the failure message truthful.
  EXPECT_FALSE(parse_num_ctx("num_ctx notanumber").has_value());
  EXPECT_FALSE(parse_num_ctx("num_ctx -1").has_value());
  EXPECT_FALSE(parse_num_ctx("num_ctx 65536junk").has_value());
  EXPECT_FALSE(parse_num_ctx("num_ctx").has_value());
  EXPECT_FALSE(parse_num_ctx("num_ctx 999999999999999999999999").has_value());
}

TEST(ParseNumCtx, ValueMustBeSeparatedFromTheKeyByWhitespace) {
  EXPECT_FALSE(parse_num_ctx("num_ctx65536").has_value());
}

// --- strip_latest ------------------------------------------------------------

TEST(StripLatest, RemovesOnlyTheSuffix) {
  EXPECT_EQ(strip_latest("qwen35-agent:latest"), "qwen35-agent");
  EXPECT_EQ(strip_latest("qwen35-agent"), "qwen35-agent");
  EXPECT_EQ(strip_latest("qwen3.6:35b-a3b-q8_0"), "qwen3.6:35b-a3b-q8_0");
}

TEST(StripLatest, ADegenerateNameIsLeftAlone) {
  // ":latest" with nothing before it is a name, not a suffix; stripping it would
  // produce an empty model name that matches nothing.
  EXPECT_EQ(strip_latest(":latest"), ":latest");
}

// --- validate_base_url -------------------------------------------------------

TEST(ValidateBaseUrl, AcceptsLoopbackForms) {
  for (const char* url : {"http://localhost:11434", "http://127.0.0.1:11434", "http://[::1]:11434",
                          "http://localhost", "http://127.1.2.3:11434", "http://localhost:11434/"}) {
    EXPECT_TRUE(hermit::ollama::validate_base_url(url).has_value()) << url;
  }
}

TEST(ValidateBaseUrl, RejectsNonLoopbackHosts) {
  // D7 is a hard commitment, and this client carries sandbox file contents. A stub
  // server forging /api/tags and /api/show is enough to make the whole preflight pass,
  // so the host has to be refused before any of that can happen.
  for (const char* url : {"http://192.0.2.55:11434", "http://ollama.example.com:11434",
                          "http://[2001:db8::1]:11434", "http://10.0.0.5"}) {
    EXPECT_FALSE(hermit::ollama::validate_base_url(url).has_value()) << url;
  }
}

TEST(ValidateBaseUrl, RejectsHostnamesThatMerelyLookLikeLoopback) {
  // The guard used to ask `starts_with("127.")`, a string test where an address test
  // was needed. Every one of these is a registrable domain that satisfies the prefix,
  // resolves wherever its owner points it, and would have been sent sandbox contents.
  for (const char* url : {"http://127.0.0.1.example.com:11434", "http://127.evil.com:11434",
                          "http://127.0.0.1.attacker.net", "http://1270.0.0.1:11434",
                          "http://127.0.0.1.:11434", "http://127.0.0:11434",
                          "http://127.0.0.1.5:11434", "http://127.0.0.999:11434",
                          "http://localhost.evil.com:11434"}) {
    EXPECT_FALSE(hermit::ollama::validate_base_url(url).has_value()) << url;
  }
}

TEST(ValidateBaseUrl, RejectsUserinfoThatHidesTheRealHost) {
  // Talks to example.com while the report prints a string beginning 127.0.0.1.
  EXPECT_FALSE(
      hermit::ollama::validate_base_url("http://127.0.0.1:11434@example.com/").has_value());
  EXPECT_FALSE(hermit::ollama::validate_base_url("http://localhost@evil.com").has_value());
}

TEST(ValidateBaseUrl, AcceptsTheWholeLoopbackRangeNotJust127001) {
  // 127.0.0.0/8 is loopback in its entirety, so a real address anywhere in it is fine.
  for (const char* url : {"http://127.0.0.1", "http://127.1.2.3:11434", "http://127.255.255.255"}) {
    EXPECT_TRUE(hermit::ollama::validate_base_url(url).has_value()) << url;
  }
}

TEST(ValidateBaseUrl, RejectsSchemesHttplibWouldThrowOn) {
  // The likeliest typo for this flag. It used to abort the process.
  EXPECT_FALSE(hermit::ollama::validate_base_url("https://localhost:11434").has_value());
  EXPECT_FALSE(hermit::ollama::validate_base_url("localhost:11434").has_value());
  EXPECT_FALSE(hermit::ollama::validate_base_url("").has_value());
}

TEST(ValidateBaseUrl, RejectsAPathBecauseHttplibDiscardsIt) {
  // Accepting one means printing a URL in the report that is not the one used.
  EXPECT_FALSE(hermit::ollama::validate_base_url("http://localhost:11434/wrong").has_value());
}

TEST(ValidateBaseUrl, ClientOpenSurfacesTheRejectionAsAFailure) {
  hermit::ollama::ClientOptions options;
  options.base_url = "https://localhost:11434";
  const auto client = hermit::ollama::Client::open(options);
  EXPECT_FALSE(client.has_value());
}

// --- build_chat_options: the num_ctx clamp -----------------------------------
//
// This is a safety property, not a formatting one. An unclamped num_ctx hard-froze
// kitchen-desktop on 2026-08-13 (262144 requested on a 29 GB model, 48 GB card):
// Ollama accepted it, amdgpu thrashed, and the machine died with no OOM kill and no
// GPU reset. These run offline precisely so the guard is never checked by reproducing
// the thing it guards against.

TEST(BuildChatOptions, ClampsNumCtxToTheCeiling) {
  hermit::ollama::ChatRequest request;
  request.num_ctx = 262144;  // the value that took the machine down
  const auto options = hermit::ollama::build_chat_options(request, 65536);
  EXPECT_EQ(options.at("num_ctx").get<std::uint64_t>(), 65536u);
}

TEST(BuildChatOptions, LeavesAValueUnderTheCeilingAlone) {
  hermit::ollama::ChatRequest request;
  request.num_ctx = 32768;
  EXPECT_EQ(hermit::ollama::build_chat_options(request, 65536).at("num_ctx").get<std::uint64_t>(),
            32768u);
}

TEST(BuildChatOptions, ExactlyTheCeilingIsUnchanged) {
  hermit::ollama::ChatRequest request;
  request.num_ctx = 65536;
  EXPECT_EQ(hermit::ollama::build_chat_options(request, 65536).at("num_ctx").get<std::uint64_t>(),
            65536u);
}

TEST(BuildChatOptions, ClampHoldsAtAbsurdValues) {
  // A config typo or an overflowed computation must not become a request the driver
  // dies on. There is no input that produces a num_ctx above the ceiling.
  hermit::ollama::ChatRequest request;
  for (const std::uint64_t requested :
       {std::uint64_t{1} << 40, std::numeric_limits<std::uint64_t>::max()}) {
    request.num_ctx = requested;
    EXPECT_EQ(hermit::ollama::build_chat_options(request, 8192).at("num_ctx").get<std::uint64_t>(),
              8192u);
  }
}

TEST(BuildChatOptions, AnAbsentNumCtxSendsNoKeyAtAll) {
  // Sending nothing leaves the server default in force. That is the undetermined
  // state R9 refuses, but it is the caller's to fix -- inventing a value here would
  // hide it.
  hermit::ollama::ChatRequest request;
  EXPECT_FALSE(hermit::ollama::build_chat_options(request, 65536).contains("num_ctx"));
}

TEST(BuildChatOptions, CarriesTemperatureAndPredictBudget) {
  hermit::ollama::ChatRequest request;
  request.temperature = 0.0;
  request.max_tokens = 16;
  const auto options = hermit::ollama::build_chat_options(request, 65536);
  EXPECT_EQ(options.at("num_predict").get<int>(), 16);
  EXPECT_DOUBLE_EQ(options.at("temperature").get<double>(), 0.0);
  EXPECT_FALSE(options.contains("max_tokens")) << "that is the OpenAI name, not Ollama's";
}

TEST(BuildChatOptions, AnUnsetTemperatureIsNotSentAtAll) {
  // The default. Sending any number pins sampling, and 0 pins it hardest -- which is
  // what this client silently did to every run until bench/delta's first sweep showed
  // hermit deterministic while the baseline arm sampled freely (2026-08-18). The
  // model's own default is only reachable by omission, so omission must be
  // representable, and must be what an untouched request does.
  const hermit::ollama::ChatRequest request;
  EXPECT_FALSE(hermit::ollama::build_chat_options(request, 65536).contains("temperature"));
}

// --- parse_model_card --------------------------------------------------------

TEST(ParseModelCard, ReadsARealShowResponse) {
  // Shape and values from /api/show for qwen35-agent:latest on kitchen-desktop.
  const json body{
      {"capabilities", json::array({"completion", "vision", "tools", "thinking"})},
      {"parameters", kRealParameters},
      {"model_info", {{"qwen35moe.context_length", 262144}, {"qwen35moe.block_count", 48}}},
      {"details",
       {{"family", "qwen35moe"}, {"parameter_size", "36.0B"}, {"quantization_level", "Q4_K_M"}}},
  };

  const auto card = parse_model_card(body);
  ASSERT_TRUE(card.has_value()) << card.error().message();
  EXPECT_TRUE(card->reported_capabilities);
  EXPECT_TRUE(card->has_capability("tools"));
  EXPECT_FALSE(card->has_capability("embedding"));
  EXPECT_EQ(card->num_ctx, 131072u);
  EXPECT_EQ(card->architecture_context, 262144u);
  EXPECT_EQ(card->family, "qwen35moe");
  EXPECT_EQ(card->quantization, "Q4_K_M");
}

TEST(ParseModelCard, KeepsTheTwoContextNumbersApart) {
  // The trap this exists to avoid: an architecture that supports 262144 says nothing
  // about what the model is running at. Gating on the wrong one admits a model that
  // then runs at 4096.
  const json body{
      {"capabilities", json::array({"tools"})},
      {"parameters", "temperature 1"},
      {"model_info", {{"qwen35moe.context_length", 262144}}},
  };

  const auto card = parse_model_card(body);
  ASSERT_TRUE(card.has_value());
  EXPECT_FALSE(card->num_ctx.has_value());
  EXPECT_EQ(card->architecture_context, 262144u);
}

TEST(ParseModelCard, MissingCapabilitiesIsRecordedAsUnreportedNotAsEmpty) {
  const auto card = parse_model_card(json{{"parameters", kRealParameters}});
  ASSERT_TRUE(card.has_value());
  EXPECT_FALSE(card->reported_capabilities);
  EXPECT_TRUE(card->capabilities.empty());
}

TEST(ParseModelCard, EmptyCapabilitiesListIsReportedButEmpty) {
  const auto card =
      parse_model_card(json{{"capabilities", json::array()}, {"parameters", kRealParameters}});
  ASSERT_TRUE(card.has_value());
  EXPECT_TRUE(card->reported_capabilities);
  EXPECT_TRUE(card->capabilities.empty());
}

TEST(ParseModelCard, FindsContextLengthUnderAnyArchitecturePrefix) {
  for (const std::string architecture : {"gemma3", "llama", "qwen35moe", "nemotronh"}) {
    const json body{{"model_info", {{architecture + ".context_length", 65536}}}};
    const auto card = parse_model_card(body);
    ASSERT_TRUE(card.has_value()) << architecture;
    EXPECT_EQ(card->architecture_context, 65536u) << architecture;
  }
}

TEST(ParseModelCard, ToleratesEveryOptionalKeyBeingAbsent) {
  const auto card = parse_model_card(json::object());
  ASSERT_TRUE(card.has_value());
  EXPECT_FALSE(card->num_ctx.has_value());
  EXPECT_FALSE(card->architecture_context.has_value());
  EXPECT_TRUE(card->family.empty());
}

TEST(ParseModelCard, RejectsBodiesThatAreNotObjects) {
  const auto card = parse_model_card(json::array({1, 2, 3}));
  ASSERT_FALSE(card.has_value());
  EXPECT_EQ(card.error().kind, TransportError::UnexpectedShape);
}

TEST(ParseModelCard, RejectsACapabilitiesFieldOfTheWrongType) {
  // A string where an array belongs would otherwise silently become "no capabilities",
  // which reads as "no tools" and blames the model for a protocol change.
  const auto card = parse_model_card(json{{"capabilities", "tools"}});
  ASSERT_FALSE(card.has_value());
  EXPECT_EQ(card.error().kind, TransportError::UnexpectedShape);
}

// --- parse_tags --------------------------------------------------------------

TEST(ParseTags, ReadsInstalledModels) {
  const json body{{"models", json::array({
                                  json{{"name", "qwen35-agent:latest"},
                                       {"size", 23000000000},
                                       {"modified_at", "2026-08-07T12:00:00Z"}},
                                  json{{"name", "gemma31-agent:latest"}, {"size", 19000000000}},
                              })}};

  const auto tags = parse_tags(body);
  ASSERT_TRUE(tags.has_value()) << tags.error().message();
  ASSERT_EQ(tags->size(), 2u);
  EXPECT_EQ((*tags)[0].name, "qwen35-agent:latest");
  EXPECT_EQ((*tags)[0].size_bytes, 23000000000u);
  EXPECT_EQ((*tags)[1].modified_at, "");
}

TEST(ParseTags, SkipsEntriesWithNoUsableName) {
  const json body{{"models", json::array({json{{"size", 1}}, json{{"name", "real:latest"}}})}};
  const auto tags = parse_tags(body);
  ASSERT_TRUE(tags.has_value());
  ASSERT_EQ(tags->size(), 1u);
  EXPECT_EQ((*tags)[0].name, "real:latest");
}

TEST(ParseTags, NoModelsArrayIsAnError) {
  EXPECT_FALSE(parse_tags(json::object()).has_value());
  EXPECT_FALSE(parse_tags(json{{"models", "none"}}).has_value());
}

TEST(ParseTags, AnEmptyInstallIsValidAndEmpty) {
  // A running Ollama with nothing pulled is a real state, and distinct from an error.
  const auto tags = parse_tags(json{{"models", json::array()}});
  ASSERT_TRUE(tags.has_value());
  EXPECT_TRUE(tags->empty());
}

// --- parse_chat_reply --------------------------------------------------------

TEST(ParseChatReply, ReadsARealNativeResponse) {
  // Shape as /api/chat returns it on Ollama 0.32.9: no `choices` envelope, token
  // counts at the top level, and `done_reason` rather than `finish_reason`.
  const json body{
      {"message", {{"role", "assistant"}, {"content", "OK"}}},
      {"done", true},
      {"done_reason", "stop"},
      {"prompt_eval_count", 12},
      {"eval_count", 2},
  };

  const auto reply = parse_chat_reply(body);
  ASSERT_TRUE(reply.has_value()) << reply.error().message();
  EXPECT_EQ(reply->content, "OK");
  EXPECT_EQ(reply->finish_reason, "stop");
  EXPECT_EQ(reply->prompt_tokens, 12u);
  EXPECT_EQ(reply->completion_tokens, 2u);
}

TEST(ParseChatReply, ReadsThinkingAsItsOwnField) {
  // The native endpoint calls it `thinking`; the OpenAI-compatible one called it
  // `reasoning`. Dropping it makes "the model returned nothing" indistinguishable
  // from "the model thought itself out of its budget".
  const json body{{"message", {{"content", ""}, {"thinking", "let me think"}}},
                  {"done_reason", "length"}};
  const auto reply = parse_chat_reply(body);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->reasoning, "let me think");
  EXPECT_TRUE(reply->content.empty());
  EXPECT_TRUE(reply->truncated());
}

TEST(ParseChatReply, TruncationIsDistinguishedFromCompletion) {
  const auto reply = [](const char* reason) {
    return parse_chat_reply(json{{"message", {{"content", "x"}}}, {"done_reason", reason}});
  };
  ASSERT_TRUE(reply("length").has_value());
  EXPECT_TRUE(reply("length")->truncated());
  EXPECT_FALSE(reply("stop")->truncated());
}

TEST(ParseChatReply, AReplyWithNoMessageIsRejected) {
  // An empty envelope proves only that something answered on the port. Accepting it
  // let the warmup check pass without any evidence the model generated anything.
  EXPECT_FALSE(parse_chat_reply(json::object()).has_value());
  EXPECT_FALSE(parse_chat_reply(json{{"done", true}, {"done_reason", "stop"}}).has_value());
  EXPECT_FALSE(parse_chat_reply(json{{"message", "text"}}).has_value());
}

TEST(ParseChatReply, GeneratedDistinguishesRealOutputFromAnEmptyReply) {
  const auto parse = [](json message, int tokens) {
    return parse_chat_reply(json{{"message", std::move(message)}, {"eval_count", tokens}});
  };
  EXPECT_FALSE(parse(json{{"content", ""}}, 0)->generated());
  EXPECT_TRUE(parse(json{{"content", "OK"}}, 0)->generated());
  // A thinking model that spent the whole budget reasoning still generated.
  EXPECT_TRUE(parse(json{{"content", ""}, {"thinking", "hmm"}}, 0)->generated());
  EXPECT_TRUE(parse(json{{"content", ""}}, 5)->generated());
}

TEST(ParseChatReply, AReplyWithNoThinkingFieldIsFine) {
  const auto reply = parse_chat_reply(json{{"message", {{"content", "OK"}}}});
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(reply->reasoning.empty());
}

TEST(ParseChatReply, NullContentIsAValidReplyNotAFailure) {
  // What a model returns when it answers purely with tool calls. Treating it as a
  // parse error would turn the intended path into a startup failure.
  const json body{{"message", {{"content", nullptr}}}, {"done_reason", "stop"}};
  const auto reply = parse_chat_reply(body);
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(reply->content.empty());
}

// --- tool calls on the wire (Phase 2) ----------------------------------------
//
// The measurements these assertions stand on are recorded in D12 and in client.h:
// `tools` alone is correct on 7 of 7 models probed; adding `format` breaks 4 of them.
// So the type makes the combination unrepresentable, and these tests pin the two
// halves it does allow.

namespace {

hermit::ollama::ChatRequest minimal_request() {
  hermit::ollama::ChatRequest request;
  request.model = "test-model";
  request.messages.push_back({.role = "user", .content = "hello"});
  request.num_ctx = 8192;
  return request;
}

}  // namespace

TEST(ChatToolCalls, ParsesNameAndStructuredArguments) {
  const auto calls = hermit::ollama::parse_tool_calls(nlohmann::json::parse(R"([
    {"id": "call_abc", "function": {"name": "write",
     "arguments": {"path": "hello.txt", "content": "HERMIT-OK"}}}
  ])"));

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].id, "call_abc");
  EXPECT_EQ(calls[0].name, "write");
  EXPECT_EQ(calls[0].arguments["path"], "hello.txt");
  EXPECT_EQ(calls[0].arguments["content"], "HERMIT-OK");
}

TEST(ChatToolCalls, KeepsOrderAndDuplicatesBecauseModelsEmitBoth) {
  // Measured: llama3.2-3b answered one prompt with `read a.txt`, `read b.txt`,
  // `read a.txt` -- three calls, one repeated. Deduplicating or collapsing here would
  // silently drop work the model asked for.
  const auto calls = hermit::ollama::parse_tool_calls(nlohmann::json::parse(R"([
    {"function": {"name": "read", "arguments": {"path": "a.txt"}}},
    {"function": {"name": "read", "arguments": {"path": "b.txt"}}},
    {"function": {"name": "read", "arguments": {"path": "a.txt"}}}
  ])"));

  ASSERT_EQ(calls.size(), 3u);
  EXPECT_EQ(calls[0].arguments["path"], "a.txt");
  EXPECT_EQ(calls[1].arguments["path"], "b.txt");
  EXPECT_EQ(calls[2].arguments["path"], "a.txt");
}

TEST(ChatToolCalls, AbsentArgumentsBecomeAnEmptyObjectRatherThanNull) {
  const auto calls = hermit::ollama::parse_tool_calls(
      nlohmann::json::parse(R"([{"function": {"name": "list"}}])"));

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_TRUE(calls[0].arguments.is_object());
  EXPECT_TRUE(calls[0].arguments.empty());
}

TEST(ChatToolCalls, SkipsANamelessCallRatherThanPropagatingAHalfValue) {
  const auto calls = hermit::ollama::parse_tool_calls(nlohmann::json::parse(R"([
    {"function": {"name": "", "arguments": {}}},
    {"function": {"arguments": {}}},
    {"not_a_function": true},
    "a bare string",
    {"function": {"name": "read", "arguments": {"path": "a.txt"}}}
  ])"));

  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].name, "read");
}

TEST(ChatToolCalls, ANonArrayYieldsNothingRatherThanThrowing) {
  EXPECT_TRUE(hermit::ollama::parse_tool_calls(nlohmann::json::object()).empty());
  EXPECT_TRUE(hermit::ollama::parse_tool_calls(nlohmann::json()).empty());
}

TEST(ChatToolCalls, AReplyCarryingOnlyCallsHasEmptyContentAndIsNotAFailure) {
  // The usual shape of a working agent turn: every correct call in D12's table came
  // back with content empty.
  const auto reply = hermit::ollama::parse_chat_reply(nlohmann::json::parse(R"({
    "message": {"role": "assistant", "content": "",
                "tool_calls": [{"function": {"name": "read", "arguments": {"path": "a.txt"}}}]},
    "done_reason": "stop", "prompt_eval_count": 334, "eval_count": 99
  })"));

  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(reply->content.empty());
  ASSERT_EQ(reply->tool_calls.size(), 1u);
  EXPECT_EQ(reply->tool_calls[0].name, "read");
  EXPECT_EQ(reply->prompt_tokens, 334u);
}

TEST(ChatPayload, SendsToolsAndNoFormatForTheToolsConstraint) {
  auto request = minimal_request();
  request.constraint = hermit::ollama::ChatRequest::Tools{
      .definitions = {nlohmann::json{{"type", "function"},
                                     {"function", {{"name", "read"}}}}}};

  const auto payload = hermit::ollama::build_chat_payload(request, 65536);
  ASSERT_TRUE(payload.contains("tools"));
  EXPECT_EQ(payload["tools"].size(), 1u);
  EXPECT_EQ(payload["tools"][0]["function"]["name"], "read");
  EXPECT_FALSE(payload.contains("format"));
}

TEST(ChatPayload, SendsFormatAndNoToolsForTheSchemaConstraint) {
  auto request = minimal_request();
  request.constraint =
      hermit::ollama::ChatRequest::Schema{.schema = nlohmann::json{{"type", "object"}}};

  const auto payload = hermit::ollama::build_chat_payload(request, 65536);
  ASSERT_TRUE(payload.contains("format"));
  EXPECT_EQ(payload["format"]["type"], "object");
  EXPECT_FALSE(payload.contains("tools"));
}

TEST(ChatPayload, SendsNeitherKeyWhenUnconstrained) {
  const auto payload = hermit::ollama::build_chat_payload(minimal_request(), 65536);
  EXPECT_FALSE(payload.contains("tools"));
  EXPECT_FALSE(payload.contains("format"));
}

TEST(ChatPayload, OmitsAnEmptyToolListRatherThanSendingToolsColonEmptyArray) {
  auto request = minimal_request();
  request.constraint = hermit::ollama::ChatRequest::Tools{};

  const auto payload = hermit::ollama::build_chat_payload(request, 65536);
  EXPECT_FALSE(payload.contains("tools"));
}

TEST(ChatPayload, RebuildsAnAssistantTurnsCallsInTheShapeVerifiedToRoundTrip) {
  // Verified 2026-08-17 against all five models: this shape renders a byte-identical
  // prompt to the server's own verbatim object (equal prompt_eval_count), so nothing
  // depends on the `function.index` field dropped here.
  auto request = minimal_request();
  hermit::ollama::ToolCall call;
  call.id = "call_abc";
  call.name = "read";
  call.arguments = nlohmann::json{{"path", "a.txt"}};
  request.messages.push_back(
      {.role = "assistant", .content = "", .tool_calls = {call}});
  request.messages.push_back(
      {.role = "tool", .content = R"([{"path":"a.txt"}])", .tool_name = "read"});

  const auto payload = hermit::ollama::build_chat_payload(request, 65536);
  const auto& assistant = payload["messages"][1];
  EXPECT_EQ(assistant["role"], "assistant");
  ASSERT_TRUE(assistant.contains("tool_calls"));
  EXPECT_EQ(assistant["tool_calls"][0]["id"], "call_abc");
  EXPECT_EQ(assistant["tool_calls"][0]["type"], "function");
  EXPECT_EQ(assistant["tool_calls"][0]["function"]["name"], "read");
  EXPECT_EQ(assistant["tool_calls"][0]["function"]["arguments"]["path"], "a.txt");

  const auto& result = payload["messages"][2];
  EXPECT_EQ(result["role"], "tool");
  EXPECT_EQ(result["tool_name"], "read");
}

TEST(ChatPayload, OmitsToolFieldsOnPlainTextMessages) {
  // An empty tool_calls on every user turn is noise the template may or may not
  // tolerate, and there is no reason to find out.
  const auto payload = hermit::ollama::build_chat_payload(minimal_request(), 65536);
  const auto& user = payload["messages"][0];
  EXPECT_FALSE(user.contains("tool_calls"));
  EXPECT_FALSE(user.contains("tool_name"));
}
