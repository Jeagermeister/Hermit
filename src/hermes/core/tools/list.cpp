#include <hermes/core/tools/list.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hermes/core/fsio.h>

namespace hermes {
namespace {

constexpr std::array<ArgSpec, 1> kArgs{{
    {.name = "path",
     .type = ArgType::Path,
     .required = true,
     .doc = "directory to list, relative to the sandbox root"},
}};

const ToolSpec kSpec{
    "list",
    "Directory entries with type, size and the dev:ino:size:mtime:ctime identity tuple.",
    kArgs};

std::string_view type_of(mode_t mode) noexcept {
  if (S_ISREG(mode)) return "file";
  if (S_ISDIR(mode)) return "dir";
  if (S_ISLNK(mode)) return "symlink";
  return "other";
}

std::int64_t to_ns(const timespec& t) noexcept {
  return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

}  // namespace

const ToolSpec& ListTool::spec() const noexcept { return kSpec; }

std::expected<ToolOutput, ToolError> ListTool::run(const ToolArgs& args) {
  const SandboxPath& dir = *args.path("path");
  const auto refuse = [&dir](std::string_view what, const IoError& e) {
    std::string reason{"list: "};
    reason += dir.relative().string();
    if (!what.empty()) {
      reason += "/";
      reason += what;
    }
    reason += ": ";
    reason += to_string(e);
    return std::unexpected{ToolError{std::move(reason)}};
  };

  auto fd = open_in_root(dir, O_RDONLY | O_DIRECTORY);
  if (!fd) return refuse({}, fd.error());

  // fdopendir takes ownership of the descriptor on success; on failure it is
  // still ours to close.
  int raw = fd->release();
  DIR* d = ::fdopendir(raw);
  if (d == nullptr) {
    const int e = errno;
    ::close(raw);
    return refuse({}, IoError{.code = e});
  }

  struct Entry {
    std::string name;
    struct ::stat st;
  };
  std::vector<Entry> entries;

  errno = 0;
  while (const dirent* de = ::readdir(d)) {
    const std::string_view name{de->d_name};
    if (name == "." || name == "..") {
      errno = 0;
      continue;
    }
    struct ::stat st {};
    // AT_SYMLINK_NOFOLLOW: an entry that is a symlink is reported as one,
    // never followed -- following it here could describe a file outside the
    // root, which no result of this tool may do.
    if (::fstatat(::dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      const int e = errno;
      ::closedir(d);
      return refuse(name, IoError{.code = e});
    }
    entries.push_back({std::string{name}, st});
    errno = 0;
  }
  const int read_error = errno;
  ::closedir(d);
  if (read_error != 0) return refuse({}, IoError{.code = read_error});

  std::ranges::sort(entries, {}, &Entry::name);

  ToolOutput out;
  out.rows.reserve(entries.size());
  for (const Entry& e : entries) {
    out.rows.push_back({{
        {"path", (dir.relative() / e.name).lexically_normal().string()},
        {"type", std::string{type_of(e.st.st_mode)}},
        {"dev", static_cast<std::uint64_t>(e.st.st_dev)},
        {"ino", static_cast<std::uint64_t>(e.st.st_ino)},
        {"size", static_cast<std::uint64_t>(e.st.st_size)},
        {"mtime_ns", to_ns(e.st.st_mtim)},
        {"ctime_ns", to_ns(e.st.st_ctim)},
    }});
  }
  return out;
}

}  // namespace hermes
