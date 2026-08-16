#include <hermes/core/tools/read.h>

#include <array>
#include <string>
#include <utility>

#include <hermes/core/fsio.h>
#include <hermes/core/sha256.h>

namespace hermes {
namespace {

constexpr std::array<ArgSpec, 1> kArgs{{
    {.name = "paths",
     .type = ArgType::PathList,
     .required = true,
     .doc = "files to read, one or more, relative to the sandbox root"},
}};

const ToolSpec kSpec{
    "read",
    "Return the exact bytes of one or more files, with a content hash per file.",
    kArgs};

}  // namespace

const ToolSpec& ReadTool::spec() const noexcept { return kSpec; }

std::expected<ToolOutput, ToolError> ReadTool::run(const ToolArgs& args) {
  ToolOutput out;
  for (const SandboxPath& p : args.paths("paths")) {
    auto file = read_file(p);
    if (!file) {
      return std::unexpected{ToolError{
          "read: " + p.relative().string() + ": " + to_string(file.error())}};
    }
    std::string hash = sha256_hex(file->bytes);
    out.rows.push_back({{{"path", p.relative().string()},
                         {"content", std::move(file->bytes)},
                         {"hash", std::move(hash)}}});
  }
  return out;
}

}  // namespace hermes
