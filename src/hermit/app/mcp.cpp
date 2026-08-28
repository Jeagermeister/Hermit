#include <hermit/app/mcp.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <hermit/app/config.h>
#include <hermit/app/toolset.h>
#include <hermit/core/confine.h>
#include <hermit/ollama/client.h>
#include <hermit/supervisor/loop.h>
#include <hermit/supervisor/undo.h>
#include <hermit/supervisor/wire.h>

#ifndef HERMIT_VERSION
#define HERMIT_VERSION "0.0.0"  // set by CMakeLists.txt from PROJECT_VERSION; this is a fallback only
#endif

namespace hermit::app {
namespace {

using nlohmann::json;

constexpr std::string_view kServerName = "hermit";
constexpr std::string_view kServerVersion = HERMIT_VERSION;

// The MCP specification's revisions are dated strings ("2024-11-05", "2025-03-26",
// "2025-06-18", ...). This is the newest one this implementation was written against;
// confirm it against the live specification before relying on it, since a newer
// revision may exist by the time this runs. The negotiation *rule* below (echo the
// client's version back only if it matches this one) is stable regardless.
constexpr std::string_view kProtocolVersion = "2025-06-18";

// Named rather than left as bare literals: every other error vocabulary in this
// codebase (DecodeErrorKind, SandboxError, ConfineErrorKind, ...) is an enum, and the
// JSON-RPC 2.0 spec fixes these five exact values -- they are not this project's to
// invent, but they are still worth a name.
enum class JsonRpcError : int {
  ParseError = -32700,
  InvalidRequest = -32600,
  MethodNotFound = -32601,
  InvalidParams = -32602,
};

json make_error(const json& id, JsonRpcError code, std::string message) {
  return json{{"jsonrpc", "2.0"},
              {"id", id},
              {"error", json{{"code", static_cast<int>(code)}, {"message", std::move(message)}}}};
}

json make_result(const json& id, json result) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json handle_initialize(const json& id, const json& request, std::string_view server_name,
                       std::string_view server_version) {
  std::string negotiated{kProtocolVersion};
  if (const auto params = request.find("params"); params != request.end() && params->is_object()) {
    if (const auto requested = params->find("protocolVersion");
        requested != params->end() && requested->is_string() &&
        requested->get<std::string>() == kProtocolVersion) {
      negotiated = requested->get<std::string>();
    }
  }
  return make_result(id, json{{"protocolVersion", negotiated},
                              {"capabilities", json{{"tools", json::object()}}},
                              {"serverInfo", json{{"name", std::string{server_name}},
                                                  {"version", std::string{server_version}}}}});
}

json handle_tools_list(const json& id, ToolRegistry& registry) {
  return make_result(id, json{{"tools", supervisor::mcp_tool_definitions(registry)}});
}

json handle_tools_call(const json& id, const json& request, ToolRegistry& registry,
                       const Sandbox& sandbox) {
  const auto params = request.find("params");
  if (params == request.end() || !params->is_object()) {
    return make_error(id, JsonRpcError::InvalidParams, "Invalid params: tools/call requires a params object");
  }
  const auto name = params->find("name");
  if (name == params->end() || !name->is_string()) {
    return make_error(id, JsonRpcError::InvalidParams, "Invalid params: tools/call requires a string 'name'");
  }

  // `null` is treated the same as absent, not refused: some clients serialize an
  // omitted optional field as an explicit null rather than dropping the key, and a
  // no-argument call is exactly what that means here. Anything else non-object (a
  // string, a number, an array) has no reasonable reading and is refused.
  json arguments = json::object();
  if (const auto args_it = params->find("arguments");
      args_it != params->end() && !args_it->is_null()) {
    if (!args_it->is_object()) {
      return make_error(id, JsonRpcError::InvalidParams, "Invalid params: 'arguments' must be an object");
    }
    arguments = *args_it;
  }

  const ollama::ToolCall call{
      .id = "", .name = name->get<std::string>(), .arguments = std::move(arguments)};
  const supervisor::Dispatched dispatched = supervisor::dispatch_call(registry, sandbox, call);

  // Never a JSON-RPC error: an unknown tool, bad arguments, or the tool's own refusal
  // are all `dispatch_call`'s "never fails" vocabulary -- the protocol succeeded, the
  // tool declined, and MCP's own isError flag is exactly that distinction.
  return make_result(id, json{{"content", json::array({json{{"type", "text"},
                                                             {"text", dispatched.content}}})},
                              {"isError", dispatched.refused}});
}

}  // namespace

std::optional<json> handle_message(ToolRegistry& registry, const Sandbox& sandbox,
                                   std::string_view server_name, std::string_view server_version,
                                   const json& request) {
  const bool request_shaped = request.is_object() && request.contains("method");
  const json id = (request.is_object() && request.contains("id")) ? request.at("id") : json();

  if (!request_shaped) {
    // Not even a notification's minimum shape (an object with "method"). Per JSON-RPC
    // 2.0: id is echoed when one is available, and null when it cannot be determined.
    return make_error(id, JsonRpcError::InvalidRequest, "Invalid Request");
  }

  const auto& method_field = request.at("method");
  if (!method_field.is_string()) {
    return make_error(id, JsonRpcError::InvalidRequest, "Invalid Request: 'method' must be a string");
  }
  const std::string method = method_field.get<std::string>();

  if (!request.contains("id")) {
    // A notification: JSON-RPC 2.0 forbids a reply to one, recognized method or not.
    // Nothing here has long-running work for e.g. notifications/cancelled to interrupt.
    return std::nullopt;
  }

  if (method == "initialize") return handle_initialize(id, request, server_name, server_version);
  if (method == "tools/list") return handle_tools_list(id, registry);
  if (method == "tools/call") return handle_tools_call(id, request, registry, sandbox);
  return make_error(id, JsonRpcError::MethodNotFound, "Method not found: " + method);
}

int mcp_command(std::span<const std::string_view> args) {
  // `--backups` is subcommand-local, exactly as it is for `agent`/`undo` in main.cpp --
  // not part of the shared Config/load() flag set.
  std::optional<std::filesystem::path> backup_dir;
  std::vector<std::string_view> passthrough;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--backups") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: --backups needs a value\n";
        return 2;
      }
      backup_dir = std::filesystem::path{args[++i]};
      continue;
    }
    passthrough.push_back(args[i]);
  }

  std::vector<std::string_view> positional;
  const auto config =
      load(passthrough, {.sandbox_root = true, .model = false, .ollama = false}, positional);
  if (!config) {
    std::cerr << "error: " << config.error().message() << '\n';
    return 2;
  }
  if (!positional.empty()) {
    std::cerr << "error: mcp takes no positional arguments, got: " << positional.front() << '\n';
    return 2;
  }

  auto box = Sandbox::open(config->sandbox_root);
  if (!box) {
    std::cerr << "error: " << to_string(box.error()) << ": " << config->sandbox_root << '\n';
    return 1;
  }

  const auto store_dir = resolve_backup_dir(*box, std::move(backup_dir));
  if (!store_dir) {
    std::cerr << "error: --backups must be outside --root (R4): " << to_string(store_dir.error())
              << '\n';
    return 2;
  }

  // D14: a retention failure is a note, not a stop -- the frontend starting must not be
  // blocked by archive bookkeeping. Fixed at the CLI's own default (72h); this surface
  // does not expose --keep-hours today.
  {
    constexpr std::chrono::hours kKeepFor{72};
    const auto pruned = supervisor::prune(*store_dir, kKeepFor,
                                          std::filesystem::file_time_type::clock::now());
    if (!pruned) {
      std::cerr << "note: retention skipped: " << pruned.error() << '\n';
    } else if (pruned->generations > 0) {
      std::cerr << "retention: pruned " << pruned->generations
                << (pruned->generations == 1 ? " undo generation older than "
                                             : " undo generations older than ")
                << kKeepFor.count() << "h from " << store_dir->string() << '\n';
    }
  }

  // Registration gated on a LIVE probe, never on the config flag alone -- the same
  // reasoning and the same hard refusal as agent_command in main.cpp (ROUTING.md
  // section 8: "gate on the probe, never on the platform"; DECISIONS.md D11).
  std::optional<ShellOptions> shell_options;
  if (config->shell.enabled) {
    auto probe = probe_confinement();
    if (!probe || *probe != ConfinementProbeResult::Enforced) {
      std::cerr << "error: shell is enabled in configuration, but kernel confinement could "
                   "not be confirmed enforced on this machine ("
                << (probe ? std::string{to_string(*probe)} : to_string(probe.error()))
                << "). Refusing to start rather than expose shell unconfined (DECISIONS.md, D11).\n";
      return 1;
    }
    shell_options = ShellOptions{
        box->root(), std::chrono::duration_cast<std::chrono::milliseconds>(config->shell.timeout)};
  }

  auto tools = ToolSet::tier0(*store_dir, shell_options);
  if (!tools) {
    std::cerr << "error: composing the tool set failed: " << to_string(tools.error().kind) << '\n';
    return 1;
  }

  std::cerr << "hermit mcp: root=" << box->root() << " backups=" << *store_dir
            << " tools=" << tools->registry().tools().size()
            << (shell_options ? " (shell enabled)" : "") << '\n';

  // A generous cap above what a legitimate call needs -- `write`/`edit`'s content
  // argument is bounded by core's own 16 MiB read cap before JSON escaping inflates it,
  // so this leaves headroom rather than pricing the escaping precisely. Refusing here
  // stops an oversized or abusive line before it reaches json::parse or a tool at all.
  //
  // The one gap this does not close: std::getline itself has no bound, so a peer that
  // sends bytes forever without ever writing '\n' grows the line unbounded before this
  // check runs at all. Accepted rather than solved -- D7's threat model is a locally
  // spawned, already-trusted parent process (no port, no network peer), not an attacker
  // controlling the pipe's bytes, and closing that fully would mean hand-rolling
  // buffered line reading for a peer this project does not defend against.
  constexpr std::size_t kMaxLineBytes = 64u * 1024 * 1024;

  // The loop itself: one JSON-RPC message per line, in and out, until stdin closes.
  // MCP's stdio transport ends when the client closes its side -- there is no explicit
  // shutdown method to wait for.
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;  // a blank keep-alive line is tolerated, not answered

    if (line.size() > kMaxLineBytes) {
      std::cout << ollama::dump_lossy(make_error(
                        json(), JsonRpcError::ParseError,
                        "Parse error: message exceeds the " +
                            std::to_string(kMaxLineBytes / (1024 * 1024)) + " MiB limit"))
                << '\n'
                << std::flush;
      continue;
    }

    json parsed = json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
      // The one failure handle_message structurally cannot see: it takes an
      // already-parsed value, and this line never became one.
      std::cout << ollama::dump_lossy(make_error(json(), JsonRpcError::ParseError, "Parse error"))
                << '\n'
                << std::flush;
      continue;
    }

    if (auto response =
            handle_message(tools->registry(), *box, kServerName, kServerVersion, parsed)) {
      // Never a bare dump(): tool output can carry raw file bytes, which can be invalid
      // UTF-8. See wire.cpp's own render_output for the incident this guards against.
      std::cout << ollama::dump_lossy(*response) << '\n' << std::flush;
    }
  }
  return 0;
}

}  // namespace hermit::app
