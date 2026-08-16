#include <hermes/core/tools/read.h>

#include <array>
#include <string>
#include <utility>

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
    auto file = read_file(p, max_file_bytes_);
    if (!file) {
      std::string reason =
          "read: " + p.relative().string() + ": " + to_string(file.error());
      if (file.error().kind == IoError::Kind::TooLarge) {
        // The graceful half of the refusal: say what still works at this
        // size, so the model's next call can be a useful one.
        reason += "; hash can verify it and list can identify it at any size";
      }
      return std::unexpected{ToolError{std::move(reason)}};
    }
    std::string hash = sha256_hex(file->bytes);
    out.rows.push_back({{{"path", p.relative().string()},
                         {"content", std::move(file->bytes)},
                         {"hash", std::move(hash)}}});
  }
  return out;
}

}  // namespace hermes
