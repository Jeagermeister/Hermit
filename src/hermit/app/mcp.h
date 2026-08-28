#pragma once

// The MCP-over-stdio frontend (D7, ROUTING.md section 12 step 6): a subprocess speaking
// JSON-RPC on stdin/stdout, giving an MCP client (Claude, Kiro, any other) the same
// tool surface the CLI's `agent` command drives, published from the same descriptor
// list `supervisor/wire.h` already renders for Ollama (D4's one-declaration guarantee).
//
// Split in two for testability: `handle_message` is the protocol logic, pure over its
// inputs -- no I/O, no framing, no process lifetime -- so it is reachable from a test
// exactly the way `wire.cpp`'s functions are, without a real stdin/stdout conversation.
// `mcp_command` is the thin subcommand entry point `main()` calls, which owns the parts
// that cannot be unit tested: reading argv, composing the sandbox and tool set, and the
// read-a-line/write-a-line loop itself.

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <hermit/core/sandbox.h>
#include <hermit/core/tool.h>

namespace hermit::app {

/// One already-parsed JSON-RPC message in, at most one response out.
///
/// Returns `std::nullopt` for a notification (a message with no `"id"`): JSON-RPC 2.0
/// forbids a reply to one, whether or not its method is recognized. A malformed message
/// that cannot even be read as a notification (not an object, or missing `"method"`) is
/// answered with an Invalid Request error instead, per the spec's own rule for that case.
///
/// Never throws. A refused `tools/call` (unknown tool, bad arguments, the tool's own
/// failure) is not a JSON-RPC error -- it is `isError: true` inside a normal successful
/// result, matching MCP's distinction between "the protocol failed" and "the tool
/// declined". Only a malformed *protocol* message (missing method, `tools/call` with no
/// `name`) produces a JSON-RPC error object.
[[nodiscard]] std::optional<nlohmann::json> handle_message(
    ToolRegistry& registry, const Sandbox& sandbox, std::string_view server_name,
    std::string_view server_version, const nlohmann::json& request);

/// The `mcp` subcommand: compose the sandbox and tool set exactly as `agent`/`undo` do,
/// then read JSON-RPC messages from stdin and write responses to stdout until stdin
/// closes. Every other diagnostic (startup banner, retention note, a refused shell gate)
/// goes to stderr -- stdout on this frontend carries JSON-RPC and nothing else.
[[nodiscard]] int mcp_command(std::span<const std::string_view> args);

}  // namespace hermit::app
