#pragma once

// `shell` -- ROUTING.md section 4: the one tool that cannot be R1-correct by construction. A
// command string has no path argument for Sandbox::resolve to contain, so containment moves
// from the type system to the kernel: every call runs through confine::run_confined under D10's
// fixed Landlock grant table, one writable root.
//
// Registration is conditional: toolset.cpp appends this tool only when its caller supplies a
// ShellOptions, and the actual gate -- an explicit config flag AND a live probe_confinement()
// reporting Enforced -- is checked once at the call site (main.cpp), not here and not in
// toolset.cpp, both of which trust that the caller already checked. See toolset.h's own comment
// for why that gate is narrower than it once looked (D7's openat walk gates the still-unbuilt
// MCP frontend, not this one).

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <hermit/core/fsio.h>
#include <hermit/core/tool.h>

namespace hermit {

class ShellTool final : public Tool {
 public:
  /// `root` is confine::run_confined's writable grant -- the sandbox root, verbatim, not a
  /// per-call resolved path (shell has no Path argument to resolve one from). `timeout` is R8's
  /// per-call wall-clock bound: exceeding it kills the confined process group and the call
  /// reports as a ToolError, never as a ToolOutput row -- "a timeout treated as a failure rather
  /// than as missing data."
  ShellTool(std::filesystem::path root, std::chrono::milliseconds timeout,
            std::uint64_t max_output_bytes = kDefaultMaxReadBytes) noexcept
      : root_(std::move(root)), timeout_(timeout), max_output_bytes_(max_output_bytes) {}

  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;

  std::filesystem::path root_;
  std::chrono::milliseconds timeout_;
  std::uint64_t max_output_bytes_;
};

}  // namespace hermit
