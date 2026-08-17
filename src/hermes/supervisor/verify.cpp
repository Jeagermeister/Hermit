#include <hermes/supervisor/verify.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hermes/core/fsio.h>
#include <hermes/core/sha256.h>

namespace hermes::supervisor {
namespace {

namespace fs = std::filesystem;

/// Hash one regular file by streaming it, so a large file does not have to be held whole.
///
/// Returns nullopt when the file could not be read, which the caller records as
/// `readable = false` rather than treating as fatal: one unreadable file is a fact about
/// the tree, while an unreadable *directory* hides a subtree and is not survivable.
std::optional<std::string> hash_regular(int dir_fd, const char* name,
                                        std::uint64_t& hashed_bytes) {
  const int fd = ::openat(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return std::nullopt;
  Fd owned{fd};

  Sha256 digest;
  std::array<char, 64 * 1024> buffer{};
  while (true) {
    const ssize_t got = ::read(owned.get(), buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (got == 0) break;
    digest.update(std::string_view{buffer.data(), static_cast<std::size_t>(got)});
    hashed_bytes += static_cast<std::uint64_t>(got);
  }
  return to_hex(digest.finish());
}

}  // namespace

std::string_view to_string(ChangeKind k) noexcept {
  switch (k) {
    case ChangeKind::Created:            return "created";
    case ChangeKind::Deleted:            return "deleted";
    case ChangeKind::Modified:           return "modified";
    case ChangeKind::TouchedOnly:        return "touched";
    case ChangeKind::ReadabilityChanged: return "readability";
    case ChangeKind::TypeChanged:        return "type";
  }
  return "unknown";
}

std::string_view to_string(VerifyErrorKind e) noexcept {
  switch (e) {
    case VerifyErrorKind::DirectoryUnreadable: return "a directory could not be read";
    case VerifyErrorKind::RootUnreadable:      return "the sandbox root could not be opened";
  }
  return "unknown verification error";
}

std::string VerifyError::message() const {
  std::string text{to_string(kind)};
  if (!path.empty()) {
    text += ": ";
    text += path;
  }
  if (code != 0) {
    text += ": ";
    text += std::strerror(code);
  }
  // A partial answer is the adjacent-success ROUTING.md section 3 forbids, so this says
  // outright that the snapshot was abandoned rather than trimmed.
  text += " (no snapshot was taken; a partial one would report the hidden subtree as deleted)";
  return text;
}

std::size_t Changeset::substantive() const noexcept {
  return static_cast<std::size_t>(
      std::count_if(changes.begin(), changes.end(), [](const Change& c) {
        return c.kind == ChangeKind::Created || c.kind == ChangeKind::Deleted ||
               c.kind == ChangeKind::Modified || c.kind == ChangeKind::TypeChanged;
      }));
}

std::string Changeset::render() const {
  std::string text;
  for (const Change& change : changes) {
    text += to_string(change.kind);
    text += "  ";
    text += change.path;
    // Hashes are abbreviated for a report, never for a comparison -- 12 hex characters is
    // enough for a human to match two lines by eye and not enough to be mistaken for the
    // identity R3 actually compares, which is the full digest in `before`/`after`.
    if (!change.before.empty() || !change.after.empty()) {
      text += "  ";
      text += change.before.empty() ? std::string{"-"} : change.before.substr(0, 12);
      text += " -> ";
      text += change.after.empty() ? std::string{"-"} : change.after.substr(0, 12);
    }
    text += '\n';
  }
  return text;
}

std::expected<TreeSnapshot, VerifyError> TreeVerifier::snapshot(
    const TreeSnapshot* previous) const {
  hashed_bytes_ = 0;
  entries_walked_ = 0;

  TreeSnapshot out;

  const int root_fd =
      ::open(sandbox_->root().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root_fd < 0) {
    return std::unexpected(VerifyError{.kind = VerifyErrorKind::RootUnreadable,
                                       .path = {},
                                       .code = errno});
  }

  // Explicit stack rather than recursion: a deep tree is attacker-influenced input here,
  // and a stack overflow is not a failure this can report.
  struct Pending {
    Fd fd;
    fs::path relative;
  };
  std::vector<Pending> stack;
  stack.push_back(Pending{Fd{root_fd}, fs::path{}});

  while (!stack.empty()) {
    Pending current = std::move(stack.back());
    stack.pop_back();

    // fdopendir takes ownership of the descriptor *on success*; closedir releases it.
    // On failure it takes nothing, so the released fd is ours to close -- otherwise every
    // failed snapshot leaks one, and a caller that retries leaks monotonically.
    const int dir_fd = current.fd.release();
    DIR* dir = ::fdopendir(dir_fd);
    if (dir == nullptr) {
      // errno first: `.path`'s string() allocates, and allocation may set errno even when
      // it succeeds, which would report the wrong reason for the refusal.
      const int failure = errno;
      ::close(dir_fd);
      return std::unexpected(VerifyError{.kind = VerifyErrorKind::DirectoryUnreadable,
                                         .path = current.relative.string(),
                                         .code = failure});
    }
    struct DirGuard {
      DIR* d;
      ~DirGuard() { ::closedir(d); }
    } guard{dir};

    std::vector<std::string> names;
    errno = 0;
    while (const dirent* entry = ::readdir(dir)) {
      const std::string_view name{entry->d_name};
      if (name != "." && name != "..") names.emplace_back(name);
      errno = 0;
    }
    if (errno != 0) {
      return std::unexpected(VerifyError{.kind = VerifyErrorKind::DirectoryUnreadable,
                                         .path = current.relative.string(),
                                         .code = errno});
    }
    // Sorted for a deterministic *walk*, not for a deterministic report -- `out` is a
    // std::map, so path ordering in a Changeset comes from the container and would hold
    // with no sort at all. What this buys is that two runs over the same tree fail on the
    // same entry and hit any bound in the same order, which is what makes a refusal
    // reproducible. Cheap beside the stat and hash it precedes.
    std::ranges::sort(names);

    for (const std::string& name : names) {
      struct ::stat st {};
      if (::fstatat(::dirfd(dir), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        // A vanished entry between readdir and stat is a real event, not an error: the
        // tree is live and the model is working in it. It simply is not there now, and
        // the next snapshot's absence says so.
        if (errno == ENOENT) continue;
        return std::unexpected(VerifyError{.kind = VerifyErrorKind::DirectoryUnreadable,
                                           .path = (current.relative / name).string(),
                                           .code = errno});
      }

      const fs::path child = current.relative / name;
      const std::string key = child.lexically_normal().string();
      ++entries_walked_;

      FileState state;
      state.tuple = tuple_from(st);
      state.is_dir = S_ISDIR(st.st_mode);
      state.is_symlink = S_ISLNK(st.st_mode);

      if (S_ISREG(st.st_mode)) {
        // The incremental step: an unchanged identity tuple means the content cannot have
        // moved without ctime moving with it, so the previous hash still describes this
        // file and does not need recomputing.
        const auto seen = previous != nullptr ? previous->find(key) : TreeSnapshot::const_iterator{};
        const bool reusable = previous != nullptr && seen != previous->end() &&
                              !seen->second.is_dir && !seen->second.is_symlink &&
                              seen->second.readable && seen->second.tuple == state.tuple;
        if (reusable) {
          state.sha256 = seen->second.sha256;
        } else if (auto hashed = hash_regular(::dirfd(dir), name.c_str(), hashed_bytes_)) {
          state.sha256 = std::move(*hashed);
        } else {
          state.readable = false;
        }
      }

      out.emplace(key, std::move(state));

      if (S_ISDIR(st.st_mode)) {
        const int child_fd = ::openat(::dirfd(dir), name.c_str(),
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child_fd < 0) {
          if (errno == ENOENT) continue;  // vanished, as above
          return std::unexpected(VerifyError{.kind = VerifyErrorKind::DirectoryUnreadable,
                                             .path = key,
                                             .code = errno});
        }
        stack.push_back(Pending{Fd{child_fd}, child});
      }
    }
  }

  return out;
}

Changeset diff(const TreeSnapshot& before, const TreeSnapshot& after) {
  Changeset out;

  // Both are ordered by path, so one merge walk produces a path-ordered changeset without
  // a sort afterwards.
  auto lhs = before.begin();
  auto rhs = after.begin();
  while (lhs != before.end() || rhs != after.end()) {
    if (rhs == after.end() || (lhs != before.end() && lhs->first < rhs->first)) {
      out.changes.push_back(
          Change{.path = lhs->first, .kind = ChangeKind::Deleted, .before = lhs->second.sha256});
      ++lhs;
      continue;
    }
    if (lhs == before.end() || rhs->first < lhs->first) {
      out.changes.push_back(
          Change{.path = rhs->first, .kind = ChangeKind::Created, .after = rhs->second.sha256});
      ++rhs;
      continue;
    }

    const FileState& was = lhs->second;
    const FileState& now = rhs->second;

    if (was.is_dir != now.is_dir || was.is_symlink != now.is_symlink) {
      out.changes.push_back(Change{.path = lhs->first,
                                   .kind = ChangeKind::TypeChanged,
                                   .before = was.sha256,
                                   .after = now.sha256});
    } else if (was.readable != now.readable) {
      out.changes.push_back(Change{.path = lhs->first,
                                   .kind = ChangeKind::ReadabilityChanged,
                                   .before = was.sha256,
                                   .after = now.sha256});
    } else if (was.readable && was.sha256 != now.sha256) {
      // R3's rule, and the only branch that claims bytes moved.
      out.changes.push_back(Change{.path = lhs->first,
                                   .kind = ChangeKind::Modified,
                                   .before = was.sha256,
                                   .after = now.sha256});
    } else if (was.tuple != now.tuple) {
      // The tuple moved and the content did not. Reported, and reported separately,
      // because calling this a modification would make the report untrustworthy in the
      // direction that matters: a reader who learns the noise is noise stops reading.
      out.changes.push_back(Change{.path = lhs->first, .kind = ChangeKind::TouchedOnly});
    }

    ++lhs;
    ++rhs;
  }

  return out;
}

}  // namespace hermes::supervisor
