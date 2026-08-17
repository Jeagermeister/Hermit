#include <hermes/supervisor/wire.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using hermes::ArgSpec;
using hermes::ArgType;
using hermes::Field;
using hermes::ToolError;
using hermes::ToolOutput;
using hermes::ToolRegistry;
using hermes::ToolRow;
using hermes::ToolSpec;
using hermes::supervisor::DecodeErrorKind;
using hermes::supervisor::raw_args_from;
using hermes::supervisor::render_error;
using hermes::supervisor::render_output;
using hermes::supervisor::tool_definition;
using hermes::supervisor::tool_definitions;
using json = nlohmann::json;

namespace {

constexpr std::array<ArgSpec, 3> kSpecArgs{{
    {.name = "path", .type = ArgType::Path, .required = true, .doc = "File to read."},
    {.name = "pattern", .type = ArgType::String, .required = false, .doc = "What to match."},
    {.name = "paths", .type = ArgType::PathList, .required = true, .doc = "Files to move."},
}};

constexpr ToolSpec kSpec{
    .name = "sample", .description = "A tool for testing.", .args = kSpecArgs};

/// Registration-order test needs two real tools; neither is ever invoked here.
class Stub : public hermes::Tool {
 public:
  explicit Stub(const ToolSpec& spec) : spec_(spec) {}
  const ToolSpec& spec() const noexcept override { return spec_; }

 private:
  std::expected<ToolOutput, ToolError> run(const hermes::ToolArgs&) override {
    return ToolOutput{};
  }
  const ToolSpec& spec_;
};

constexpr std::array<ArgSpec, 1> kOneArg{
    {{.name = "path", .type = ArgType::Path, .required = true, .doc = "A path."}}};
constexpr ToolSpec kFirst{.name = "alpha", .description = "First.", .args = kOneArg};
constexpr ToolSpec kSecond{.name = "beta", .description = "Second.", .args = kOneArg};

}  // namespace

// --- the tool surface --------------------------------------------------------

TEST(WireDefinition, RendersTheShapeOllamaDocuments) {
  const json definition = tool_definition(kSpec);

  EXPECT_EQ(definition["type"], "function");
  EXPECT_EQ(definition["function"]["name"], "sample");
  EXPECT_EQ(definition["function"]["description"], "A tool for testing.");
  EXPECT_EQ(definition["function"]["parameters"]["type"], "object");
}

TEST(WireDefinition, PathAndStringBothRenderAsStringAndListAsArray) {
  const json properties = tool_definition(kSpec)["function"]["parameters"]["properties"];

  EXPECT_EQ(properties["path"]["type"], "string");
  EXPECT_EQ(properties["pattern"]["type"], "string");
  EXPECT_EQ(properties["paths"]["type"], "array");
  EXPECT_EQ(properties["paths"]["items"]["type"], "string");
}

TEST(WireDefinition, CarriesEachArgumentsDocumentationThrough) {
  const json properties = tool_definition(kSpec)["function"]["parameters"]["properties"];

  EXPECT_EQ(properties["path"]["description"], "File to read.");
  EXPECT_EQ(properties["paths"]["description"], "Files to move.");
}

TEST(WireDefinition, ListsOnlyTheRequiredArgumentsAsRequired) {
  const json required = tool_definition(kSpec)["function"]["parameters"]["required"];

  ASSERT_TRUE(required.is_array());
  EXPECT_EQ(required.size(), 2u);
  EXPECT_NE(std::find(required.begin(), required.end(), "path"), required.end());
  EXPECT_NE(std::find(required.begin(), required.end(), "paths"), required.end());
  EXPECT_EQ(std::find(required.begin(), required.end(), "pattern"), required.end());
}

TEST(WireDefinition, OmitsRequiredEntirelyWhenNothingIsRequired) {
  static constexpr std::array<ArgSpec, 1> kOptional{
      {{.name = "pattern", .type = ArgType::String, .required = false, .doc = "Maybe."}}};
  static constexpr ToolSpec spec{
      .name = "loose", .description = "Nothing required.", .args = kOptional};

  const json parameters = tool_definition(spec)["function"]["parameters"];
  EXPECT_FALSE(parameters.contains("required"));
}

TEST(WireDefinition, RendersRegistrationOrderStablySoThePromptBytesDoNotMove) {
  ToolRegistry registry;
  ASSERT_TRUE(registry.add(std::make_unique<Stub>(kFirst)).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<Stub>(kSecond)).has_value());

  const auto definitions = tool_definitions(registry);
  ASSERT_EQ(definitions.size(), 2u);
  EXPECT_EQ(definitions[0]["function"]["name"], "alpha");
  EXPECT_EQ(definitions[1]["function"]["name"], "beta");
}

// --- decoding the model's arguments ------------------------------------------

TEST(WireDecode, TakesStringsAsScalars) {
  const auto raw = raw_args_from(json{{"path", "notes.txt"}});
  ASSERT_TRUE(raw.has_value());
  ASSERT_EQ(raw->size(), 1u);
  EXPECT_EQ((*raw)[0].name, "path");
  ASSERT_TRUE(std::holds_alternative<std::string>((*raw)[0].value));
  EXPECT_EQ(std::get<std::string>((*raw)[0].value), "notes.txt");
}

TEST(WireDecode, TakesArraysOfStringsAsLists) {
  const auto raw = raw_args_from(json{{"paths", json::array({"a.txt", "b.txt"})}});
  ASSERT_TRUE(raw.has_value());
  ASSERT_EQ(raw->size(), 1u);
  ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>((*raw)[0].value));
  const auto& items = std::get<std::vector<std::string>>((*raw)[0].value);
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0], "a.txt");
  EXPECT_EQ(items[1], "b.txt");
}

TEST(WireDecode, AnEmptyObjectIsAnEmptyArgumentListNotAFailure) {
  const auto raw = raw_args_from(json::object());
  ASSERT_TRUE(raw.has_value());
  EXPECT_TRUE(raw->empty());
}

TEST(WireDecode, AnEmptyArrayDecodesAsAnEmptyListForParseArgsToRefuse) {
  // EmptyPathList is core's refusal to make, against the declaration. This layer's job
  // is only to report the shape faithfully.
  const auto raw = raw_args_from(json{{"paths", json::array()}});
  ASSERT_TRUE(raw.has_value());
  ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>((*raw)[0].value));
  EXPECT_TRUE(std::get<std::vector<std::string>>((*raw)[0].value).empty());
}

TEST(WireDecode, RefusesANonObjectOutright) {
  const auto raw = raw_args_from(json::array({"path", "notes.txt"}));
  ASSERT_FALSE(raw.has_value());
  EXPECT_EQ(raw.error().kind, DecodeErrorKind::NotAnObject);
  EXPECT_TRUE(raw.error().arg.empty());
}

TEST(WireDecode, RefusesANumberRatherThanStringifyingIt) {
  // 42 becoming the filename "42" is a call that runs and does the wrong thing. The
  // refusal names the argument so the model can correct one specific field.
  const auto raw = raw_args_from(json{{"path", 42}});
  ASSERT_FALSE(raw.has_value());
  EXPECT_EQ(raw.error().kind, DecodeErrorKind::NotAString);
  EXPECT_EQ(raw.error().arg, "path");
  EXPECT_NE(raw.error().message().find("path"), std::string::npos);
  EXPECT_NE(raw.error().message().find("number"), std::string::npos);
}

TEST(WireDecode, RefusesABoolean) {
  const auto raw = raw_args_from(json{{"path", true}});
  ASSERT_FALSE(raw.has_value());
  EXPECT_EQ(raw.error().kind, DecodeErrorKind::NotAString);
}

TEST(WireDecode, RefusesNullRatherThanReadingItAsAbsent) {
  // The tempting reading is "argument omitted". Declined deliberately: a dropped
  // optional argument means the tool runs without something the model believed it
  // sent, and the result looks like success.
  const auto raw = raw_args_from(json{{"pattern", nullptr}});
  ASSERT_FALSE(raw.has_value());
  EXPECT_EQ(raw.error().kind, DecodeErrorKind::NotAString);
  EXPECT_EQ(raw.error().arg, "pattern");
}

TEST(WireDecode, RefusesAnObjectValueAsNestedTooDeep) {
  const auto raw = raw_args_from(json{{"path", json{{"name", "notes.txt"}}}});
  ASSERT_FALSE(raw.has_value());
  EXPECT_EQ(raw.error().kind, DecodeErrorKind::NestedTooDeep);
  EXPECT_EQ(raw.error().arg, "path");
}

TEST(WireDecode, RefusesANonStringInsideAList) {
  const auto raw = raw_args_from(json{{"paths", json::array({"a.txt", 7})}});
  ASSERT_FALSE(raw.has_value());
  EXPECT_EQ(raw.error().kind, DecodeErrorKind::NotAStringInList);
  EXPECT_EQ(raw.error().arg, "paths");
}

TEST(WireDecode, RefusesTheSchemaLeakageD12Measured) {
  // The exact corruption `tools` + `format` produced on llama3.2-3b and llama3.1-8b:
  // the format object arrived as the arguments, `tool` key included. Well-formed JSON,
  // so nothing here rejects it -- it is parse_args that refuses `tool` as UnknownArg.
  // Asserted so the D12 story stays testable: this layer passes it through intact.
  const auto raw = raw_args_from(
      json{{"tool", "write"}, {"path", "hello.txt"}, {"content", "HERMES-OK"}});
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(raw->size(), 3u);
}

// --- rendering what the tool did ---------------------------------------------

TEST(WireOutput, RendersOneObjectPerRowWithFieldsUnderTheirOwnNames) {
  ToolOutput output;
  output.rows.push_back(ToolRow{.fields = {Field{.name = "path", .value = std::string{"a.txt"}},
                                           Field{.name = "content", .value = std::string{"aaaa"}}}});

  const json parsed = json::parse(render_output(output));
  ASSERT_TRUE(parsed.is_array());
  ASSERT_EQ(parsed.size(), 1u);
  EXPECT_EQ(parsed[0]["path"], "a.txt");
  EXPECT_EQ(parsed[0]["content"], "aaaa");
}

TEST(WireOutput, ContentTravelsExactWithNothingPrependedToAnyLine) {
  // ROUTING.md section 5's requirement, as an assertion: the round trip returns the
  // bytes unchanged -- no `N|` prefixes, no end-of-file marker, no ellipsis.
  const std::string exact = "line one\nline two\n\tindented\n";
  ToolOutput output;
  output.rows.push_back(
      ToolRow{.fields = {Field{.name = "content", .value = exact}}});

  const json parsed = json::parse(render_output(output));
  EXPECT_EQ(parsed[0]["content"].get<std::string>(), exact);
}

TEST(WireOutput, KeepsNumericMetadataNumericRatherThanStringified) {
  ToolOutput output;
  output.rows.push_back(ToolRow{
      .fields = {Field{.name = "line", .value = std::int64_t{12}},
                 Field{.name = "bytes", .value = std::uint64_t{4096}},
                 Field{.name = "is_dir", .value = true},
                 Field{.name = "sha256", .value = std::string{"abc123"}}}});

  const json parsed = json::parse(render_output(output));
  EXPECT_TRUE(parsed[0]["line"].is_number());
  EXPECT_EQ(parsed[0]["line"], 12);
  EXPECT_TRUE(parsed[0]["bytes"].is_number());
  EXPECT_TRUE(parsed[0]["is_dir"].is_boolean());
  EXPECT_TRUE(parsed[0]["sha256"].is_string());
}

TEST(WireOutput, AnEmptyResultIsAnEmptyArrayAndNotAnError) {
  // "matched nothing" and "refused" are different statements and must not look alike.
  EXPECT_EQ(render_output(ToolOutput{}), "[]");
  EXPECT_TRUE(json::parse(render_output(ToolOutput{})).is_array());
}

TEST(WireOutput, ARefusalIsAnObjectSoShapeAloneDistinguishesItFromSuccess) {
  const json parsed = json::parse(render_error("read refused: no such file"));
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed["error"], "read refused: no such file");
  EXPECT_FALSE(json::parse(render_output(ToolOutput{})).is_object());
}

TEST(WireOutput, EscapesAQuoteInContentSoTheResultStaysParseable) {
  const std::string awkward = R"(he said "hi" and \ then stopped)";
  ToolOutput output;
  output.rows.push_back(ToolRow{.fields = {Field{.name = "content", .value = awkward}}});

  const json parsed = json::parse(render_output(output));
  EXPECT_EQ(parsed[0]["content"].get<std::string>(), awkward);
}
