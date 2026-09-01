#include <hermit/core/tools/find.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hermit/core/fsio.h>

namespace hermit {
namespace {

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

  /// One directory being walked: the open handle, its sorted entries, how far through
  /// them we are, and where the shared path buffer has to be cut back to when it is
  /// finished. The index is what makes an explicit stack equivalent to the recursion it
  /// replaces -- it is the resume point a return address used to carry.
  ///
  /// A frame deliberately does **not** store its own path. One buffer for the whole walk
  /// (`path` in walk(), below) holds the current directory's relative path and is
  /// extended and cut back as the walk descends and unwinds, so the bytes held are
  /// proportional to the current depth rather than to the sum of every open level's
  /// path. Giving each frame its own copy is the obvious shape and is quadratic in
  /// depth: measured before this was changed, a 20,000-level chain held ~840 MB of path.
  struct Frame {
    DirHandle dir;
    std::vector<std::string> names;
    std::size_t next = 0;
    std::size_t restore_to = 0;  // path.resize() target once this directory is done
  };

  /// Pre-order, entries sorted by name per directory, so output order is
  /// deterministic across runs and filesystems. Takes ownership of `fd`.
  ///
  /// Explicit stack rather than recursion, the same choice and the same reason as
  /// TreeVerifier::snapshot (supervisor/verify.cpp): directory depth is a property of
  /// the tree rather than of anything this process sets -- a nested checkout, an
  /// extracted archive, whatever a `shell` step left behind -- and a stack overflow is
  /// not a failure this can report. It was a recursive walk until 2026-09-01, and a
  /// tree roughly 20,000 levels deep killed the process outright on a stock 8 MiB
  /// stack: the whole supervisor, or the MCP server, gone on one `find` call.
  ///
  /// **What that bought, stated at its real width.** Depth now costs heap and open
  /// descriptors instead of stack. Heap is linear in depth here and descriptors are one
  /// per level -- the latter was equally true of the recursion, and is inherent to
  /// keeping O_NOFOLLOW on every descent, since a walk that closed a level would have to
  /// re-open it by a path it has already been told not to trust. So a tree deep enough
  /// still ends the call: it ends it as an EMFILE refusal the model can read, which is
  /// this file's contract for a directory it cannot open, rather than as a signal.
  ///
  /// Paths are plain strings, not fs::path, for the reason TreeVerifier's walk gives:
  /// entry names cannot contain '/' and "." / ".." are filtered at readdir, so
  /// `parent + '/' + name` already *is* the normal form `lexically_normal()` would
  /// produce -- without the re-parse of the whole accumulated path that `fs::path`'s
  /// `operator/` costs on every append (sandbox.cpp measures that same shape at ~63
  /// bytes per component squared).
  std::optional<ToolError> walk(Fd fd, const std::string& rel) {
    std::vector<Frame> stack;

    // The relative path of whichever directory is on top of the stack. Empty is the
    // sandbox root itself, which is why a row reads "a.c" and not "./a.c".
    std::string path = rel;

    // Open one directory and make it the new top of the stack, reading and sorting its
    // entries up front exactly as the recursive version did on entry. `path` already
    // names it, so a refusal here names it too.
    const auto descend = [&](Fd dir_fd, std::size_t restore_to) -> std::optional<ToolError> {
      const int raw = dir_fd.release();
      DirHandle dir{::fdopendir(raw), &::closedir};
      if (dir == nullptr) {
        const int e = errno;
        ::close(raw);
        return refuse(path, IoError{.code = e});
      }

      std::vector<std::string> names;
      errno = 0;
      while (const dirent* de = ::readdir(dir.get())) {
        const std::string_view name{de->d_name};
        if (name != "." && name != "..") names.emplace_back(name);
        errno = 0;
      }
      if (errno != 0) return refuse(path, IoError{.code = errno});
      std::ranges::sort(names);

      stack.push_back(Frame{.dir = std::move(dir),
                            .names = std::move(names),
                            .next = 0,
                            .restore_to = restore_to});
      return std::nullopt;
    };

    if (auto err = descend(std::move(fd), path.size())) return err;

    while (!stack.empty()) {
      Frame& top = stack.back();
      if (top.next >= top.names.size()) {
        path.resize(top.restore_to);
        stack.pop_back();  // this directory is finished; its handle closes with it
        continue;
      }

      // Moved out of the frame, not referenced into it: `descend` below can reallocate
      // `stack` and move every frame with it. The slot is never read again -- `next` has
      // already passed it -- so moving rather than copying costs nothing and saves an
      // allocation per entry.
      const std::string name = std::move(top.names[top.next++]);

      // Extend the shared buffer to name this entry, remembering where to cut back to.
      const std::size_t parent_len = path.size();
      if (!path.empty()) path += '/';
      path += name;

      struct ::stat st {};
      if (::fstatat(::dirfd(top.dir.get()), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return refuse(path, IoError{.code = errno});
      }
      if (::fnmatch(pattern.c_str(), name.c_str(), 0) == 0) {
        out.rows.push_back({{{"path", path}}});
      }

      if (S_ISDIR(st.st_mode)) {
        // O_NOFOLLOW on the descent too: a directory swapped for a symlink
        // between the stat and this open is a refusal, not a traversal. Owned the
        // moment it exists, so no raw descriptor is ever in flight across a call.
        Fd child{::openat(::dirfd(top.dir.get()), name.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (!child.valid()) return refuse(path, IoError{.code = errno});
        // `top` is dead from here: descend() pushes, which may reallocate. Descending
        // immediately rather than queueing the directory for later is what preserves the
        // pre-order the recursion produced -- a subtree is emitted in full before its
        // parent moves on to the next sibling. `path` stays extended, naming the child.
        if (auto err = descend(std::move(child), parent_len)) return err;
        continue;
      }
      path.resize(parent_len);  // not a directory: cut back for the next sibling
    }
    return std::nullopt;
  }

  /// The root is the empty string here (see walk), but a refusal naming it must still
  /// read as a path -- "find: : ..." names nothing. "." is what SandboxPath spells the
  /// root, and what this line said before the walk moved off fs::path.
  static std::optional<ToolError> refuse(const std::string& rel, const IoError& e) {
    return ToolError{"find: " + (rel.empty() ? std::string{"."} : rel) + ": " + to_string(e)};
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
  // SandboxPath spells the root ".", and the walk spells it "" so that joining a name
  // onto it yields "a.c" rather than "./a.c" -- the form every row has always carried.
  const std::string base =
      start.relative() == "." ? std::string{} : start.relative().string();
  if (auto err = walker.walk(std::move(*fd), base)) {
    return std::unexpected{std::move(*err)};
  }
  return out;
}

}  // namespace hermit
