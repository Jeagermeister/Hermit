#include <hermes/core/tools/move.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hermes/core/fsio.h>
#include <hermes/core/sha256.h>

namespace hermes {
namespace {

namespace fs = std::filesystem;

constexpr std::array<ArgSpec, 2> kArgs{{
    {.name = "from",
     .type = ArgType::Path,
     .required = true,
     .doc = "file to move, relative to the sandbox root"},
    {.name = "to",
     .type = ArgType::Path,
     .required = true,
     .doc = "destination path; must not already exist"},
}};

const ToolSpec kSpec{
    "move",
    "Move one file: hash the source, rename (never replacing), verify the destination.",
    kArgs};

std::unexpected<ToolError> refuse(std::string why) {
  return std::unexpected{ToolError{"move: " + std::move(why)}};
}

std::expected<std::string, IoError> hash_fd(int fd) {
  Sha256 digest;
  char buf[65536];
  off_t offset = 0;
  for (;;) {
    const ssize_t n = ::pread(fd, buf, sizeof buf, offset);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected{IoError{.code = errno}};
    }
    if (n == 0) break;
    digest.update({buf, static_cast<std::size_t>(n)});
    offset += n;
  }
  return to_hex(digest.finish());
}

/// The suggestion half of the no-replace refusal: the first free
/// "stem (N)extension" beside the requested destination. A stat race on the
/// probe is harmless -- this is guidance, and the eventual move re-checks
/// with RENAME_NOREPLACE either way.
std::string suggest_free_name(const SandboxPath& to) {
  const std::string stem = to.path().stem().string();
  const std::string ext = to.path().extension().string();
  for (int n = 1; n < 100; ++n) {
    const std::string name = stem + " (" + std::to_string(n) + ")" + ext;
    const fs::path candidate = to.path().parent_path() / name;
    struct ::stat st {};
    if (::fstatat(AT_FDCWD, candidate.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 &&
        errno == ENOENT) {
      return (to.relative().parent_path() / name).lexically_normal().string();
    }
  }
  return {};
}

}  // namespace

const ToolSpec& MoveTool::spec() const noexcept { return kSpec; }

std::expected<ToolOutput, ToolError> MoveTool::run(const ToolArgs& args) {
  const SandboxPath& from = *args.path("from");
  const SandboxPath& to = *args.path("to");

  auto source = open_regular(from);
  if (!source) {
    return refuse(from.relative().string() + ": " + to_string(source.error()));
  }
  auto source_hash = hash_fd(source->fd.get());
  if (!source_hash) {
    return refuse(from.relative().string() + ": " + to_string(source_hash.error()));
  }

  std::error_code ec;
  fs::create_directories(to.path().parent_path(), ec);
  if (ec) {
    return refuse(to.relative().string() +
                  ": cannot create parent directories: " + ec.message());
  }

  if (::renameat2(AT_FDCWD, from.path().c_str(), AT_FDCWD, to.path().c_str(),
                  RENAME_NOREPLACE) != 0) {
    const int e = errno;
    if (e == EEXIST) {
      std::string why = to.relative().string() +
                        ": destination exists -- refusing to replace it";
      if (const std::string free_name = suggest_free_name(to); !free_name.empty()) {
        why += "; '" + free_name +
               "' is free: move there explicitly if a renamed copy is wanted, "
               "or read and write the destination to replace its content";
      }
      return refuse(std::move(why));
    }
    return refuse(to.relative().string() + ": " + to_string(IoError{.code = e}));
  }

  // R3, the destination end: hash what actually landed under the new name.
  auto dest = open_regular(to);
  if (!dest) {
    return refuse(to.relative().string() + ": moved, but verification failed: " +
                  to_string(dest.error()));
  }
  auto dest_hash = hash_fd(dest->fd.get());
  if (!dest_hash) {
    return refuse(to.relative().string() + ": moved, but verification failed: " +
                  to_string(dest_hash.error()));
  }

  // The rename happened; the observations are true regardless of the verdict.
  observed_.record_absent(from.relative());
  observed_.record_present(to.relative(), tuple_from(dest->meta));

  if (*dest_hash != *source_hash) {
    return refuse(to.relative().string() +
                  ": moved, but the destination hash does not match the source "
                  "(R3); the file is in an unexpected state");
  }

  const IdentityTuple t = tuple_from(dest->meta);
  ToolOutput out;
  out.rows.push_back({{
      {"from", from.relative().string()},
      {"to", to.relative().string()},
      {"hash", *dest_hash},
      {"dev", t.dev},
      {"ino", t.ino},
      {"size", t.size},
      {"mtime_ns", t.mtime_ns},
      {"ctime_ns", t.ctime_ns},
  }});
  return out;
}

}  // namespace hermes
