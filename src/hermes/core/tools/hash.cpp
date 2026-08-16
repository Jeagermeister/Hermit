#include <hermes/core/tools/hash.h>

#include <array>

#include <hermes/core/fsio.h>
#include <hermes/core/sha256.h>

namespace hermes {
namespace {

constexpr std::array<ArgSpec, 1> kArgs{{
    {.name = "paths",
     .type = ArgType::PathList,
     .required = true,
     .doc = "files to hash, one or more, relative to the sandbox root"},
}};

const ToolSpec kSpec{"hash", "Return a content hash (SHA-256, lowercase hex) per file.",
                     kArgs};

}  // namespace

const ToolSpec& HashTool::spec() const noexcept { return kSpec; }

std::expected<ToolOutput, ToolError> HashTool::run(const ToolArgs& args) {
  ToolOutput out;
  for (const SandboxPath& p : args.paths("paths")) {
    // Whole-file read rather than streaming: the same code path `read` trusts,
    // and file sizes here are bounded by the sandbox's working-set reality
    // rather than the tool. Revisit alongside any read size cap.
    auto file = read_file(p);
    if (!file) {
      return std::unexpected{ToolError{
          "hash: " + p.relative().string() + ": " + to_string(file.error())}};
    }
    out.rows.push_back({{{"path", p.relative().string()},
                         {"hash", sha256_hex(file->bytes)}}});
  }
  return out;
}

}  // namespace hermes
