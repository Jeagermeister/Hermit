#pragma once

// The JSON edge between `core`'s pure data and the model's wire format.
//
// ROUTING.md section 7 put the tool descriptors in `core` with no JSON types in them,
// and rendering them "in a layer that already has nlohmann". This is that layer. It is
// in `supervisor` rather than `app` for one structural reason: `supervisor` is the
// lowest target that links **both** `hermes_core` (for ToolSpec, RawArgs, ToolOutput)
// and `hermes_ollama` (which brings nlohmann), so putting the rendering here needs no
// new link edge and keeps `hermes_ollama` free of any dependency on `core`. `app` can
// reach it for `mcp.cpp`, which is the second consumer D4's one-declaration guarantee
// was written for.
//
// Nothing here holds state and nothing here does I/O. Every function is a pure
// translation, so the whole surface is testable without a daemon -- the same property
// session.h and client.h were built for.
//
// --- Three translations, in the direction the loop uses them -----------------
//
//  1. `tool_definition`  ToolSpec       -> the `tools` entry Ollama is offered.
//  2. `raw_args_from`    call arguments -> RawArgs, for core's parse_args.
//  3. `render_output`    ToolOutput     -> the `content` of a tool message.
//
// The declaration is read once in direction 1 and never re-stated in direction 2:
// what an argument *means* is checked by `parse_args` against the same ToolSpec that
// generated the schema, so there is no second copy of the argument list here to fall
// out of step with it. That is R2's drift guarantee holding across the wire.

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/core/tool.h>

namespace hermes::supervisor {

// --- 1. outbound: the tool surface -------------------------------------------

/// One tool, as Ollama's `tools` array wants it:
///
///     {"type": "function",
///      "function": {"name": ..., "description": ...,
///                   "parameters": {"type": "object", "properties": {...},
///                                  "required": [...]}}}
///
/// `ArgType::String` and `ArgType::Path` both render as `{"type": "string"}`. That is
/// deliberate and not a lost distinction: the difference between them is *where the
/// value is resolved*, which happens in `parse_args` against the sandbox, and there is
/// no JSON Schema keyword that means "inside the root". Telling the model a path is a
/// string is accurate; telling it anything more would be a claim this layer cannot
/// enforce.
///
/// `required` is omitted entirely when no argument is required, rather than emitted as
/// `[]` -- an empty required list is legal JSON Schema but is not what "nothing is
/// required" conventionally looks like, and no tool in Tier 0 exercises it today.
[[nodiscard]] nlohmann::json tool_definition(const ToolSpec& spec);

/// Every tool in `registry`, in registration order.
///
/// Order is stable because `ToolRegistry::tools()` is, which matters for more than
/// tidiness: an unstable tool order changes the prompt bytes and therefore the
/// server's prompt cache hit, turning a reproducible run into a noisy one.
[[nodiscard]] std::vector<nlohmann::json> tool_definitions(const ToolRegistry& registry);

// --- 2. inbound: the model's arguments ---------------------------------------

enum class DecodeErrorKind {
  /// `arguments` was not a JSON object. Nothing can be read out of it.
  NotAnObject,

  /// A scalar that is not a string, where every declared argument type is textual.
  /// Refused rather than stringified -- see the note on `raw_args_from`.
  NotAString,

  /// An array with a non-string element. Same rule, one level down.
  NotAStringInList,

  /// An array of arrays, or an object as a value. No ArgType has that shape.
  NestedTooDeep,
};

std::string_view to_string(DecodeErrorKind e) noexcept;

/// A refused decode, in the same shape `ArgError` uses: which argument, and what was
/// wrong with it. Rendered back to the model as a tool result it can act on.
struct DecodeError {
  std::string arg;  // empty for NotAnObject, which is about the whole object
  DecodeErrorKind kind;
  std::string detail;  // the offending JSON type, for the message

  [[nodiscard]] std::string message() const;
};

/// Translate a tool call's `arguments` object into `RawArgs`.
///
/// Spec-free on purpose. `RawArgs` has exactly two shapes -- scalar and list -- and
/// which one an argument *should* be is `parse_args`'s question, answered against the
/// declaration it also validates names and presence against. Passing the spec in here
/// too would put a second copy of that knowledge on the inbound path, which is the
/// drift R2 forbids. So this decides shape from the JSON alone and lets the mismatch
/// surface as `ArgErrorKind::WrongShape` one layer up, with the argument named.
///
/// **Non-strings are refused, not converted.** All three ArgTypes are textual, so a
/// number, boolean or null is a category error rather than a value needing coercion.
/// Stringifying it would be exactly the "guessing what was meant" ROUTING.md section 4
/// forbids of a tool, and it converts a visible mistake into an invisible one: `42`
/// becoming the filename `"42"` is a call that runs and does the wrong thing. A
/// refusal is one loud line the model can correct, which section 3 asks for.
///
/// `null` is refused too, and that is the one worth stating outright, because reading
/// it as "argument absent" is a *tempting* and defensible reading. It is declined for
/// the same reason: an optional argument silently dropped means the tool runs without
/// something the model believed it had sent, and the result looks like success.
[[nodiscard]] std::expected<RawArgs, DecodeError> raw_args_from(const nlohmann::json& arguments);

// --- 3. outbound: what the tool did ------------------------------------------

/// Render rows as the `content` of a `role: "tool"` message: a JSON array of objects,
/// one per row, each field under its own name.
///
/// ROUTING.md section 5 is the constraint -- no decoration, ever, and content travels
/// exact in its own field beside its metadata rather than interleaved with it. A JSON
/// array satisfies that directly: `read` returns `[{"path": ..., "content": ...,
/// "sha256": ...}]` and the bytes are one field's value, with nothing prepended to any
/// line and no end-of-file marker anywhere.
///
/// ⚠ **The honest residue: JSON escaping is still an encoding the model can echo.**
/// Section 5's second incident was a harness's own `N|` prefixes being written back
/// into a file as content. Escaping is a weaker version of the same hazard -- a model
/// reading `"a\nb"` can write the four characters `a\nb`. It is accepted rather than
/// solved, for two reasons that are worth being explicit about: there is no
/// delimiter-free alternative (any text framing *is* decoration, which is what section
/// 5 forbids), and it is the encoding every tool-calling model was trained against, so
/// deviating from it trades a known small risk for an unmeasured one. What actually
/// catches it is downstream: R5's read-back byte-compares what landed, and R3's hash
/// makes a wrong write loud rather than plausible.
///
/// An empty result is `[]` -- a tool that matched nothing succeeded and found nothing,
/// which is a different statement from a refusal and must not render the same way.
[[nodiscard]] std::string render_output(const ToolOutput& output);

/// Render a refusal as the `content` of a `role: "tool"` message: `{"error": reason}`.
///
/// An object, where success is an array, so the two are distinguishable by shape and
/// not only by reading the keys. Every refusal reaching the model goes through here --
/// a registry miss, a decode refusal, an argument refusal, a tool's own ToolError --
/// so there is one vocabulary on the wire rather than four.
[[nodiscard]] std::string render_error(std::string_view reason);

}  // namespace hermes::supervisor
