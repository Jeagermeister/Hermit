#include <hermes/core/tools/find.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hermes/core/fsio.h>

namespace hermes {
namespace {

namespace fs = std::filesystem;

constexpr std::array<ArgSpec, 2> kArgs{{
    {.name = "pattern",
     .type = ArgType::String,
     .required = true,
     .doc = "fnmatch(3) glob matched against entry names, case-sensitive; "
            "'*' matches dotfiles"},
    {.name = "path",
     .type = ArgType::Path,
     .required = true,
     .doc = "directory to search from; '.' for the whole sandbox"},
}};

const ToolSpec kSpec{
    "find",
    "Paths whose name matches a glob pattern, walking a directory depth-first.",
    kArgs};

using DirHandle = std::unique_ptr<DIR, int (*)(DIR*)>;

struct Walker {
  std::string pattern;  // owned: fnmatch wants a C string, built once
  ToolOutput& out;

  /// Pre-order, entries sorted by name per directory, so output order is
  /// deterministic across runs and filesystems. Takes ownership of `fd`.
  std::optional<ToolError> walk(Fd fd, const fs::path& rel) {
    const int raw = fd.release();
    DirHandle dir{::fdopendir(raw), &::closedir};
    if (dir == nullptr) {
      const int e = errno;
      ::close(raw);
      return refuse(rel, IoError{.code = e});
    }

    std::vector<std::string> names;
    errno = 0;
    while (const dirent* de = ::readdir(dir.get())) {
      const std::string_view name{de->d_name};
      if (name != "." && name != "..") names.emplace_back(name);
      errno = 0;
    }
    if (errno != 0) return refuse(rel, IoError{.code = errno});
    std::ranges::sort(names);

    for (const std::string& name : names) {
      struct ::stat st {};
      if (::fstatat(::dirfd(dir.get()), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return refuse(rel / name, IoError{.code = errno});
      }
      const fs::path child = rel / name;
      if (::fnmatch(pattern.c_str(), name.c_str(), 0) == 0) {
        out.rows.push_back({{{"path", child.lexically_normal().string()}}});
      }
      if (S_ISDIR(st.st_mode)) {
        // O_NOFOLLOW on the descent too: a directory swapped for a symlink
        // between the stat and this open is a refusal, not a traversal.
        const int child_fd = ::openat(::dirfd(dir.get()), name.c_str(),
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child_fd < 0) return refuse(child, IoError{.code = errno});
        if (auto err = walk(Fd{child_fd}, child)) return err;
      }
    }
    return std::nullopt;
  }

  static std::optional<ToolError> refuse(const fs::path& rel, const IoError& e) {
    return ToolError{"find: " + rel.lexically_normal().string() + ": " + to_string(e)};
  }
};

}  // namespace

const ToolSpec& FindTool::spec() const noexcept { return kSpec; }

std::expected<ToolOutput, ToolError> FindTool::run(const ToolArgs& args) {
  const SandboxPath& start = *args.path("path");

  auto fd = open_in_root(start, O_RDONLY | O_DIRECTORY);
  if (!fd) {
    return std::unexpected{ToolError{
        "find: " + start.relative().string() + ": " + to_string(fd.error())}};
  }

  ToolOutput out;
  Walker walker{*args.string("pattern"), out};
  if (auto err = walker.walk(std::move(*fd), start.relative())) {
    return std::unexpected{std::move(*err)};
  }
  return out;
}

}  // namespace hermes
