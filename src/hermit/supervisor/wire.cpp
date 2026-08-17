#include <hermit/supervisor/wire.h>

#include <hermit/ollama/client.h>

#include <utility>
#include <variant>

namespace hermit::supervisor {
namespace {

using nlohmann::json;

/// The JSON Schema fragment for one declared argument.
json property_of(const ArgSpec& arg) {
  json property;
  switch (arg.type) {
    case ArgType::String:
    case ArgType::Path:
      // One string either way. The Path/String difference is about resolution against
      // the sandbox, which no schema keyword expresses -- see tool_definition.
      property = json{{"type", "string"}};
      break;
    case ArgType::PathList:
      property = json{{"type", "array"}, {"items", {{"type", "string"}}}};
      break;
  }
  if (!arg.doc.empty()) property["description"] = std::string{arg.doc};
  return property;
}

/// The JSON type name, for a refusal the model can read. `json::type_name()` already
/// says "number"/"boolean"/"null"/"object"/"array", which is the vocabulary wanted.
std::string type_name_of(const json& value) { return value.type_name(); }

}  // namespace

// --- 1. outbound: the tool surface -------------------------------------------

json tool_definition(const ToolSpec& spec) {
  json properties = json::object();
  json required = json::array();

  for (const auto& arg : spec.args) {
    properties[std::string{arg.name}] = property_of(arg);
    if (arg.required) required.push_back(std::string{arg.name});
  }

  json parameters{{"type", "object"}, {"properties", std::move(properties)}};
  if (!required.empty()) parameters["required"] = std::move(required);

  return json{{"type", "function"},
              {"function",
               {{"name", std::string{spec.name}},
                {"description", std::string{spec.description}},
                {"parameters", std::move(parameters)}}}};
}

std::vector<json> tool_definitions(const ToolRegistry& registry) {
  std::vector<json> definitions;
  const auto tools = registry.tools();
  definitions.reserve(tools.size());
  for (const auto& tool : tools) definitions.push_back(tool_definition(tool->spec()));
  return definitions;
}

// --- 2. inbound: the model's arguments ---------------------------------------

std::string_view to_string(DecodeErrorKind e) noexcept {
  switch (e) {
    case DecodeErrorKind::NotAnObject:      return "arguments were not an object";
    case DecodeErrorKind::NotAString:       return "expected a string";
    case DecodeErrorKind::NotAStringInList: return "expected a list of strings";
    case DecodeErrorKind::NestedTooDeep:    return "expected a string or a list of strings";
  }
  return "unknown decode error";
}

std::string DecodeError::message() const {
  std::string text;
  if (!arg.empty()) {
    text += "argument '";
    text += arg;
    text += "': ";
  }
  text += to_string(kind);
  if (!detail.empty()) {
    text += ", got ";
    text += detail;
  }
  return text;
}

std::expected<RawArgs, DecodeError> raw_args_from(const json& arguments) {
  // A call with no arguments at all arrives as `{}` from parse_tool_calls, so the only
  // way to be here with a non-object is a genuinely malformed `arguments` value.
  if (!arguments.is_object()) {
    return std::unexpected(DecodeError{
        .arg = {}, .kind = DecodeErrorKind::NotAnObject, .detail = type_name_of(arguments)});
  }

  RawArgs raw;
  raw.reserve(arguments.size());

  for (const auto& [name, value] : arguments.items()) {
    if (value.is_string()) {
      raw.push_back(RawArg{.name = name, .value = value.get<std::string>()});
      continue;
    }

    if (value.is_array()) {
      std::vector<std::string> items;
      items.reserve(value.size());
      for (const auto& element : value) {
        if (!element.is_string()) {
          return std::unexpected(DecodeError{.arg = name,
                                             .kind = DecodeErrorKind::NotAStringInList,
                                             .detail = type_name_of(element)});
        }
        items.push_back(element.get<std::string>());
      }
      raw.push_back(RawArg{.name = name, .value = std::move(items)});
      continue;
    }

    // An object has no ArgType at all; a number, boolean or null has the wrong one.
    // Both are refused rather than coerced -- see raw_args_from's header note.
    const auto kind =
        value.is_object() ? DecodeErrorKind::NestedTooDeep : DecodeErrorKind::NotAString;
    return std::unexpected(
        DecodeError{.arg = name, .kind = kind, .detail = type_name_of(value)});
  }

  return raw;
}

// --- 3. outbound: what the tool did ------------------------------------------

std::string render_output(const ToolOutput& output) {
  json rows = json::array();
  rows.get_ref<json::array_t&>().reserve(output.rows.size());

  for (const auto& row : output.rows) {
    json rendered = json::object();
    for (const auto& field : row.fields) {
      // FieldValue's alternatives map one-to-one onto JSON scalars, so a hash stays a
      // string, a line number stays a number, and nothing is stringified on the way
      // out. Section 5 again: metadata travels in its own typed field.
      std::visit([&](const auto& value) { rendered[field.name] = value; }, field.value);
    }
    rows.push_back(std::move(rendered));
  }

  // Never a bare dump(): these rows carry raw file bytes. See ollama::dump_lossy.
  return ollama::dump_lossy(rows);
}

std::string render_error(std::string_view reason) {
  // Refusals quote paths and a tool's own message, both of which can carry invalid
  // UTF-8 -- a refused `read` names the file it would not read.
  return ollama::dump_lossy(json{{"error", std::string{reason}}});
}

}  // namespace hermit::supervisor
