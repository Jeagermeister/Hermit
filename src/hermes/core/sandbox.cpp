#include <hermes/core/sandbox.h>

#include <algorithm>
#include <deque>
#include <system_error>
#include <utility>

#include <climits>

namespace hermes {
namespace fs = std::filesystem;

namespace {
// Matches Linux MAXSYMLINKS. A chain longer than this is a loop in practice.
//
// This, not kMaxPathLength below, is what bounds the memory a resolution can consume.
// Symlink targets are read from the filesystem rather than supplied by the caller, and
// each expansion splices a fresh target's components into `pending` -- so the input
// length limit does not constrain them at all. The real ceiling is roughly
// hops x components-per-target, a few MB.
//
// Measured 2026-08-13 against a 41-link chain whose targets each carry ~1,900
// components: a two-byte input ("l1") peaked at 3.2 MB of live heap. Harmless, but
// it is the hop limit doing the work there, not the length check.
constexpr int kMaxSymlinkHops = 40;

// The kernel would reject anything longer anyway, and the bound matters more than it
// looks: resolution is **quadratic in the number of components**, not linear in the
// input as an earlier version of this comment claimed.
//
// `out = out / component` re-splits the whole accumulated path into libstdc++'s
// component vector on every append, so the cost is ~63 bytes per component squared.
// Measured 2026-08-13, appending N components: 63.61 / 63.31 / 63.16 / 63.08 bytes per
// N^2 at N = 100 / 200 / 400 / 800, with wall time quadrupling as N doubles.
//
// At this bound -- "a/" repeated 2040 times -- that is ~254 MB of malloc traffic and
// ~21 ms for a single call. Bounded and survivable, but two whole startup budgets for
// one path, which is why the check is not merely defensive: without it a megabyte-long
// argument from a model in a generation loop would cost minutes.
//
// Worth revisiting if path resolution ever shows up in a profile. Fixing it properly
// means dropping fs::path for lstat(2) on a plain string, since constructing a path
// parses it too; accumulating into a std::string alone does not help.
constexpr std::size_t kMaxPathLength = PATH_MAX;
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
    case PathError::TooLong:         return "path exceeds PATH_MAX";
    case PathError::FilesystemError: return "path could not be resolved";
  }
  return "unknown path error";
}

std::expected<Sandbox, SandboxError> Sandbox::open(const fs::path& root) {
  std::error_code ec;

  const auto status = fs::status(root, ec);
  if (ec) {
    // Same distinction as in walk(): only "absent" means absent. This branch fails
    // closed either way, so it is a diagnostics fix rather than a safety one.
    return std::unexpected(ec == std::errc::no_such_file_or_directory
                               ? SandboxError::RootNotFound
                               : SandboxError::FilesystemError);
  }
  if (!fs::exists(status)) {
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
      // "Not there" and "I could not find out what is there" are different answers and
      // must not be merged. Only the first is safe to treat as a plain name.
      //
      // Merging them is a fail-open: on EACCES this returned a path with an *unexpanded
      // symlink* still in it, and contains() -- which assumes a fully resolved path --
      // then accepted it. A link inside an unreadable directory pointing out of the root
      // yielded a SandboxPath reporting "sub/esc/secret.txt" that read a file outside.
      // A model can produce that state itself: chmod 000 on a directory it owns.
      const bool absent = (ec == std::errc::no_such_file_or_directory ||
                           ec == std::errc::not_a_directory);
      if (!absent) {
        return std::unexpected(PathError::FilesystemError);
      }
      // Genuinely not there yet: writes must be able to name files that do not exist.
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
  if (raw.size() > kMaxPathLength) {
    return std::unexpected(PathError::TooLong);
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
