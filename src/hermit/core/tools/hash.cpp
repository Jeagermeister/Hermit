#include <hermit/core/tools/hash.h>

#include <array>
#include <cerrno>

#include <unistd.h>

#include <hermit/core/fsio.h>
#include <hermit/core/sha256.h>

namespace hermit {
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
    // Streamed, deliberately uncapped: verification is exactly the job that
    // must not degrade with size, and constant memory makes the cap
    // unnecessary here. `read` is the tool that pays for returning bodies.
    auto opened = open_regular(p);
    if (!opened) {
      return std::unexpected{ToolError{
          "hash: " + p.relative().string() + ": " + to_string(opened.error())}};
    }
    Sha256 digest;
    char buf[65536];
    for (;;) {
      ssize_t n = ::read(opened->fd.get(), buf, sizeof buf);
      if (n < 0) {
        if (errno == EINTR) continue;
        return std::unexpected{ToolError{"hash: " + p.relative().string() + ": " +
                                         to_string(IoError{.code = errno})}};
      }
      if (n == 0) break;
      digest.update({buf, static_cast<std::size_t>(n)});
    }
    out.rows.push_back({{{"path", p.relative().string()},
                         {"hash", to_hex(digest.finish())}}});
  }
  return out;
}

}  // namespace hermit
