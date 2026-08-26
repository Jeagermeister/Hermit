#pragma once

// The one open primitive -- ROUTING.md section 12 step 5. Tools never spell
// ::open/::openat themselves; every open of a SandboxPath goes through
// open_in_root (reads and publication's parent directory both), which walks
// from SandboxPath::sandbox_root() one openat(O_NOFOLLOW) component at a time
// instead of trusting the pre-expanded absolute path string. A symlink swapped
// into any component -- not just the final one -- after resolve() and before
// this call is refused (ELOOP), never followed: this is what closes D6's
// worked example ("link -> a/b", right filename wrong directory) rather than
// merely documenting it.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <sys/stat.h>

#include <hermit/core/sandbox.h>

namespace hermit {

/// Owning file descriptor. Move-only; closes on destruction.
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) noexcept : fd_(fd) {}
  ~Fd() { reset(); }
  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  /// Give up ownership -- for APIs like fdopendir that take the descriptor.
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

 private:
  void reset() noexcept;
  int fd_ = -1;
};

/// An I/O refusal, reported as data. Kernel refusals carry errno; the two
/// refusals that are ours carry what a caller needs to phrase guidance --
/// which is the point of the cap (settled 2026-08-16): the refusal is a
/// graceful redirection, never a truncated read, because a partial answer is
/// the adjacent-success ROUTING.md section 3 forbids.
struct IoError {
  enum class Kind {
    Kernel,      // errno in `code`
    NotRegular,  // directory, device, socket, FIFO -- never "content"
    TooLarge,    // over the read cap; `size` and `cap` are set
    Refused,     // a precondition of ours, not the kernel's; `note` says which
  };
  Kind kind = Kind::Kernel;
  int code = 0;
  std::uint64_t size = 0;
  std::uint64_t cap = 0;
  std::string_view note{};  // Refused only; must reference a string literal
};

[[nodiscard]] std::string to_string(const IoError& e);

/// The read cap's stand-in default. The cap is *configuration* (ROUTING.md
/// section 9): when a composition layer exists it passes the configured value
/// into each tool's constructor; nothing reads this constant at run time
/// except those constructors' defaults.
inline constexpr std::uint64_t kDefaultMaxReadBytes = 16u * 1024 * 1024;

/// Open `path`, re-walked fresh from sandbox_root() -- see the file header. O_NOFOLLOW
/// and O_CLOEXEC are OR'd in unconditionally at every component, not just the last.
[[nodiscard]] std::expected<Fd, IoError> open_in_root(const SandboxPath& path, int flags);

/// An open directory, plus the bare name of the entry inside it that `target` names --
/// what publication (mkdirat/linkat/renameat2) anchors on instead of an absolute path.
struct ParentHandle {
  Fd dir;
  std::string name;
};

/// Walk to `target`'s containing directory the same way open_in_root does, without
/// opening `target` itself -- for callers that publish into it rather than read it.
/// `create_missing` mkdirat's any missing intermediate directory as the walk reaches
/// it (mode 0777, matching write's/move's existing "missing parents are created"
/// contract); with it false, a missing intermediate directory is refused (ENOENT),
/// matching edit's "never creates parents" contract.
///
/// Refused with IoError::Kind::Refused if `target` names the sandbox root itself --
/// the root has no in-root parent to publish into.
[[nodiscard]] std::expected<ParentHandle, IoError> open_parent_in_root(
    const SandboxPath& target, bool create_missing);

/// A regular file, open, with the stat taken from that fd -- the same object
/// even if the name has since been retargeted.
struct OpenedFile {
  Fd fd;
  struct ::stat meta {};
};

/// Open a regular file for reading, refusing anything else -- a directory,
/// device, socket or FIFO is never "content" (section 3: no answer is a valid
/// answer, and a blocking open on a planted FIFO is no answer; O_NONBLOCK
/// makes that a refusal rather than a hang).
[[nodiscard]] std::expected<OpenedFile, IoError> open_regular(const SandboxPath& path);

struct FileContent {
  std::string bytes;
  struct ::stat meta {};
};

/// Read one regular file completely, refusing files over `max_bytes` -- both
/// up front from the stat and again during the read, because st_size is a
/// claim about the past and the file can grow underneath the loop.
[[nodiscard]] std::expected<FileContent, IoError> read_file(
    const SandboxPath& path, std::uint64_t max_bytes = kDefaultMaxReadBytes);

/// Read the remainder of an already-open regular file, same cap semantics as
/// read_file. For callers that need the open fd for more than one thing --
/// the staleness guard stats it, R4 streams a backup from it, then the edit
/// reads it -- one open, one object, no re-resolution between steps.
[[nodiscard]] std::expected<std::string, IoError> read_all(const OpenedFile& file,
                                                           std::uint64_t max_bytes);

/// Write every byte or say why not. EINTR is retried; a short write continues.
[[nodiscard]] std::expected<void, IoError> write_all(int fd, std::string_view bytes);

/// Create an exclusive temp file inside `parent_fd` -- same directory as
/// `target_name` so the later linkat()/renameat2() publication is
/// same-filesystem and atomic -- fill it with `bytes`, and set `mode` exactly
/// (fchmod, not umask-masked). Returns the temp's bare filename, relative to
/// `parent_fd`; the caller publishes or unlinks it there. A crash in between
/// leaves a ".hermit-tmp." orphan beside the target, which is visible,
/// greppable, and harmless.
///
/// Durability posture, deliberate: no fsync anywhere on this path. R5's
/// read-back verifies content, R4 keeps the old bytes recoverable, and
/// crash-durability of the new bytes is a claim this codebase has not made;
/// if it ever does, that is a decision for DECISIONS.md, not a flag here.
[[nodiscard]] std::expected<std::string, IoError> write_temp_in_dir(
    int parent_fd, std::string_view target_name, std::string_view bytes, ::mode_t mode);

}  // namespace hermit
