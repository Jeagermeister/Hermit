#include <hermit/core/tools/shell.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <hermit/core/confine.h>

namespace hermit {
namespace {

constexpr std::array<ArgSpec, 1> kArgs{{
    {.name = "command",
     .type = ArgType::String,
     .required = true,
     .doc = "a full shell command line, run via `/usr/bin/sh -c`, confined to the sandbox root"},
}};

const ToolSpec kSpec{
    "shell",
    "Run a shell command, confined by the kernel to the sandbox root (D10). Prefer the "
    "structured file tools when one fits; use this for anything they do not cover.",
    kArgs};

ToolRow result_row(int exit_code, const CapturedStream& out, const CapturedStream& err) {
  return {{
      {"exit_code", static_cast<std::int64_t>(exit_code)},
      {"stdout", out.bytes},
      {"stdout_truncated", out.truncated},
      {"stderr", err.bytes},
      {"stderr_truncated", err.truncated},
  }};
}

}  // namespace

const ToolSpec& ShellTool::spec() const noexcept { return kSpec; }

std::expected<ToolOutput, ToolError> ShellTool::run(const ToolArgs& args) {
  const std::string& command = *args.string("command");
  if (command.empty()) {
    return std::unexpected{
        ToolError{"shell: empty command -- nothing to run"}};
  }

  const ConfineLimits limits{
      .timeout = timeout_,
      .stdout_cap = max_output_bytes_,
      .stderr_cap = max_output_bytes_,
  };
  auto result = run_confined(root_, std::vector<std::string>{"/usr/bin/sh", "-c", command},
                             /*allowed=*/{}, limits);
  if (!result) {
    return std::unexpected{ToolError{"shell: " + to_string(result.error())}};
  }
  if (result->timed_out) {
    // Byte counts, not the bytes themselves: ToolError::reason is a flat string with no
    // sibling-field slots, and byte-exact content is a promise this codebase makes only about
    // successful results (R5's read-back-and-compare). But whether the command produced nothing
    // or was partway through something useful is exactly the kind of thing a retry decides
    // differently on -- discarding it here would waste what run_confined already captured before
    // the kill.
    return std::unexpected{ToolError{
        "shell: command did not finish within " + std::to_string(timeout_.count()) +
        "ms and was killed (R8); captured " + std::to_string(result->stdout_capture.bytes.size()) +
        " bytes stdout, " + std::to_string(result->stderr_capture.bytes.size()) +
        " bytes stderr before the kill"}};
  }

  // A nonzero exit is data the model reacts to, not a tool failure -- the tool's one whole job
  // was running the command, confined, within budget, and it did.
  ToolOutput out;
  out.rows.push_back(result_row(result->exit_code, result->stdout_capture, result->stderr_capture));
  return out;
}

}  // namespace hermit
