#include <hermes/core/tools/read.h>

#include <array>
#include <cerrno>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

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
  // Presence commits only when the whole call succeeds: a refused multi-file
  // read delivered no bytes, and a presence recorded from it would let a
  // later write pass the staleness gate on content the model never received
  // -- the exact class the guard exists to catch. Absence is the safe
  // direction and records immediately: create-if-absent is independently
  // honest (link()-no-replace), so a stale absence cannot destroy anything.
  std::vector<std::pair<std::filesystem::path, IdentityTuple>> seen;
  for (const SandboxPath& p : args.paths("paths")) {
    auto file = read_file(p, max_file_bytes_);
    if (!file) {
      if (file.error().kind == IoError::Kind::Kernel &&
          file.error().code == ENOENT) {
        observed_.record_absent(p.relative());  // a miss records absence (section 4)
      }
      std::string reason =
          "read: " + p.relative().string() + ": " + to_string(file.error());
      if (file.error().kind == IoError::Kind::TooLarge) {
        // The graceful half of the refusal: say what still works at this
        // size, so the model's next call can be a useful one.
        reason += "; hash can verify it and list can identify it at any size";
      }
      return std::unexpected{ToolError{std::move(reason)}};
    }
    seen.emplace_back(p.relative(), tuple_from(file->meta));
    std::string hash = sha256_hex(file->bytes);
    out.rows.push_back({{{"path", p.relative().string()},
                         {"content", std::move(file->bytes)},
                         {"hash", std::move(hash)}}});
  }
  for (const auto& [relative, tuple] : seen) {
    observed_.record_present(relative, tuple);
  }
  return out;
}

}  // namespace hermes
