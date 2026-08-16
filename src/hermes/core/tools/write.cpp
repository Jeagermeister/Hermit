#include <hermes/core/tools/write.h>

#include <array>
#include <cerrno>
#include <filesystem>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hermes/core/fsio.h>
#include <hermes/core/sha256.h>

namespace hermes {
namespace {

namespace fs = std::filesystem;

constexpr std::array<ArgSpec, 2> kArgs{{
    {.name = "path",
     .type = ArgType::Path,
     .required = true,
     .doc = "file to write, relative to the sandbox root"},
    {.name = "content",
     .type = ArgType::String,
     .required = true,
     .doc = "the file's entire new content, byte-exact"},
}};

const ToolSpec kSpec{
    "write",
    "Write a whole file: create-if-absent, or replace a file observed this session.",
    kArgs};

std::unexpected<ToolError> refuse(const SandboxPath& p, std::string_view why) {
  return std::unexpected{
      ToolError{"write: " + p.relative().string() + ": " + std::string{why}}};
}

/// The rows every successful mutation returns: the resolved path, the new
/// content's hash, and the fresh identity tuple in the same currency `list`
/// speaks, so the observation is immediately usable again.
ToolRow result_row(const SandboxPath& p, bool created, const std::string& hash,
                   const IdentityTuple& t) {
  return {{
      {"path", p.relative().string()},
      {"created", created},
      {"hash", hash},
      {"dev", t.dev},
      {"ino", t.ino},
      {"size", t.size},
      {"mtime_ns", t.mtime_ns},
      {"ctime_ns", t.ctime_ns},
  }};
}

}  // namespace

const ToolSpec& WriteTool::spec() const noexcept { return kSpec; }

std::expected<ToolOutput, ToolError> WriteTool::run(const ToolArgs& args) {
  const SandboxPath& target = *args.path("path");
  const std::string& content = *args.string("content");
  const auto view = observed_.lookup(target.relative());

  std::error_code ec;
  fs::create_directories(target.path().parent_path(), ec);
  if (ec) return refuse(target, "cannot create parent directories: " + ec.message());

  bool created = false;
  if (view.status == ObservedState::Status::Present) {
    // Replace, guarded: one open carries the staleness check, the R4 backup,
    // and the mode to preserve -- all statements about the same object.
    auto current = open_regular(target);
    if (!current) {
      if (current.error().kind == IoError::Kind::Kernel &&
          current.error().code == ENOENT) {
        observed_.record_absent(target.relative());
        return refuse(target, "was present when last observed, now missing; "
                              "read or list before writing");
      }
      return refuse(target, to_string(current.error()));
    }
    if (tuple_from(current->meta) != view.tuple) {
      // Deliberately NOT recorded as the new observation: recording here
      // would let an immediate retry pass the guard without re-reading
      // content, which is the read-first rule with the reading removed.
      return refuse(target, "changed since last observed; read it again first");
    }
    if (auto kept = backups_.preserve_fd(target.relative(), current->fd.get());
        !kept) {
      return refuse(target, "backup failed: " + to_string(kept.error()) +
                                "; nothing was written");
    }
    auto temp = write_temp_beside(target, content,
                                  current->meta.st_mode & 07777);
    if (!temp) return refuse(target, to_string(temp.error()));
    if (::rename(temp->c_str(), target.path().c_str()) != 0) {
      const int e = errno;
      ::unlink(temp->c_str());
      return refuse(target, to_string(IoError{.code = e}));
    }
  } else {
    // Create-if-absent. link() refuses an existing target atomically, so a
    // file that appeared since observation is preserved, never replaced.
    ::mode_t mask = ::umask(0);
    ::umask(mask);
    auto temp = write_temp_beside(target, content, 0666 & ~mask);
    if (!temp) return refuse(target, to_string(temp.error()));
    const int linked = ::link(temp->c_str(), target.path().c_str());
    const int link_errno = errno;
    ::unlink(temp->c_str());
    if (linked != 0) {
      if (link_errno == EEXIST) {
        return refuse(target,
                      "exists but was never read this session; read it first, "
                      "then write to replace it");
      }
      return refuse(target, to_string(IoError{.code = link_errno}));
    }
    created = true;
  }

  // R5: read back and compare before the turn may succeed. The read-back's
  // stat is also the observation recorded -- the state the caller now knows.
  auto back = read_file(target, std::max<std::uint64_t>(content.size(), 1));
  if (!back) return refuse(target, "read-back failed: " + to_string(back.error()));
  if (back->bytes != content) {
    return refuse(target, "read-back does not match what was written (R5); "
                          "the file is in an unexpected state");
  }
  observed_.record_present(target.relative(), tuple_from(back->meta));

  ToolOutput out;
  out.rows.push_back(result_row(target, created, sha256_hex(content),
                                tuple_from(back->meta)));
  return out;
}

}  // namespace hermes
