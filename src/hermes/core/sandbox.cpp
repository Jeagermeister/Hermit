#include <hermes/core/sandbox.h>

#include <algorithm>
#include <deque>
#include <system_error>
#include <utility>

namespace hermes {
namespace fs = std::filesystem;

namespace {
// Matches Linux MAXSYMLINKS. A chain longer than this is a loop in practice.
constexpr int kMaxSymlinkHops = 40;
}  // namespace

std::string_view to_string(SandboxError e) noexcept {
  switch (e) {
    case SandboxError::RootNotFound:     return "sandbox root does not exist";
    case SandboxError::RootNotDirectory: return "sandbox root is not a directory";
    case SandboxError::FilesystemError:  return "sandbox root could not be canonicalised";
  }
  return "unknown sandbox error";
}

std::string_view to_string(PathError e) noexcept {
  switch (e) {
    case PathError::Empty:           return "path is empty";
    case PathError::EmbeddedNul:     return "path contains a NUL byte";
    case PathError::EscapesRoot:     return "path resolves outside the sandbox root";
    case PathError::FilesystemError: return "path could not be resolved";
  }
  return "unknown path error";
}

std::expected<Sandbox, SandboxError> Sandbox::open(const fs::path& root) {
  std::error_code ec;

  const auto status = fs::status(root, ec);
  if (ec || !fs::exists(status)) {
    return std::unexpected(SandboxError::RootNotFound);
  }
  if (!fs::is_directory(status)) {
    return std::unexpected(SandboxError::RootNotDirectory);
  }

  // Canonicalise once, here. If the root is reached through a symlink, every path we
  // later resolve will have had its symlinks expanded, so the two must be expressed in
  // the same terms or containment comparisons silently fail open.
  //
  // Note this is the one place the process working directory can matter: a relative
  // root is canonicalised against it. That is a deliberate, one-time admin decision at
  // startup -- see the note on Sandbox::open in the header. Nothing in resolve() ever
  // consults it.
  fs::path canonical = fs::canonical(root, ec);
  if (ec) {
    return std::unexpected(SandboxError::FilesystemError);
  }

  return Sandbox{std::move(canonical)};
}

bool Sandbox::contains(const fs::path& absolute_resolved) const {
  // Component-wise, not string-prefix: "/srv/work2" must not count as inside
  // "/srv/work", which a starts_with check would happily accept.
  //
  // Private, and it must stay private: it assumes its argument is already absolute and
  // fully resolved. Fed a path still containing ".." or an unexpanded symlink it
  // answers "inside" for things that are not, i.e. it fails open. resolve() is the
  // only caller and only ever passes it the output of walk().
  const auto [root_it, _] =
      std::mismatch(root_.begin(), root_.end(),
                    absolute_resolved.begin(), absolute_resolved.end());
  return root_it == root_.end();
}

std::expected<std::filesystem::path, PathError> Sandbox::walk(const fs::path& input) const {
  // Resolve one component at a time, expanding symlinks as they are encountered --
  // the same order the kernel uses.
  //
  // Doing this textually instead (lexically_normal, then expand) is wrong whenever a
  // ".." follows a symlink: "link/.." collapses to nothing before anyone notices that
  // `link` pointed somewhere else entirely. With `link -> a/b` that silently yields
  // the root instead of "a", so a write lands in the wrong directory with the right
  // name -- the exact R1 failure. Worse, if `link` points outside the root, textual
  // collapse rebases the path back inside it, which R1 forbids outright.
  std::deque<fs::path> pending(input.begin(), input.end());

  fs::path out = input.root_path();
  if (!pending.empty() && pending.front() == out) {
    pending.pop_front();
  }

  int hops = 0;
  while (!pending.empty()) {
    const fs::path component = pending.front();
    pending.pop_front();

    if (component.empty() || component == ".") {
      continue;  // "." and the trailing empty component of "a/b/"
    }
    if (component == "..") {
      // Now correct, because `out` is already fully resolved up to this point.
      if (out != out.root_path()) {
        out = out.parent_path();
      }
      continue;
    }

    fs::path next = out / component;

    std::error_code ec;
    const auto link_status = fs::symlink_status(next, ec);
    if (ec) {
      // Does not exist (or is unreadable): treat as a plain name. Writes must be able
      // to name files that are not there yet.
      out = std::move(next);
      continue;
    }

    if (!fs::is_symlink(link_status)) {
      out = std::move(next);
      continue;
    }

    if (++hops > kMaxSymlinkHops) {
      return std::unexpected(PathError::FilesystemError);  // symlink loop
    }

    fs::path target = fs::read_symlink(next, ec);
    if (ec) {
      return std::unexpected(PathError::FilesystemError);
    }

    // Splice the target's components in front of whatever is still pending, so the
    // rest of the original path continues to resolve after the link is expanded.
    std::deque<fs::path> expanded(target.begin(), target.end());
    if (target.is_absolute()) {
      out = target.root_path();
      if (!expanded.empty()) {
        expanded.pop_front();  // drop the leading "/"
      }
    }
    // A relative target resolves against the directory holding the link, which is
    // exactly the current `out`.
    while (!expanded.empty()) {
      pending.push_front(std::move(expanded.back()));
      expanded.pop_back();
    }
  }

  return out;
}

std::expected<SandboxPath, PathError> Sandbox::resolve(std::string_view raw) const {
  if (raw.empty()) {
    return std::unexpected(PathError::Empty);
  }
  if (raw.find('\0') != std::string_view::npos) {
    // Would truncate silently at the syscall boundary: the path checked here and the
    // path opened later would differ.
    return std::unexpected(PathError::EmbeddedNul);
  }

  const fs::path input{std::string(raw)};

  // Relative paths resolve against the root. The process working directory is never
  // consulted -- that divergence is precisely the R1 failure.
  const fs::path candidate = input.is_absolute() ? input : (root_ / input);

  auto resolved = walk(candidate);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  // Containment is judged on the fully resolved path, and only there. A path may leave
  // the root and come back ("../<root-name>/f", or a link out and a ".." back in) --
  // what matters is the file it finally names.
  if (!contains(*resolved)) {
    return std::unexpected(PathError::EscapesRoot);
  }

  // NOTE: between this check and the eventual open() a symlink could be swapped in.
  // Closing that race needs openat(O_NOFOLLOW) walking one component at a time. The
  // threat model here is a confused 3B model, not an attacker with local shell access,
  // so the race is documented rather than closed. Revisit if untrusted input ever
  // reaches this API.

  fs::path relative = resolved->lexically_relative(root_);
  if (relative.empty()) {
    relative = ".";  // the root itself
  }
  return SandboxPath{std::move(*resolved), std::move(relative)};
}

}  // namespace hermes
