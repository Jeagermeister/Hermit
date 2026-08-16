#pragma once

// The one open primitive -- ROUTING.md section 12 step 4, settled 2026-08-16:
// allow the semantics, funnel the spelling. Tools never spell ::open
// themselves; every open of a SandboxPath goes through open_in_root, so when
// D7's gate lands, the openat(O_NOFOLLOW) component walk replaces this one
// function body and no tool changes. Until then the body is a plain open with
// O_NOFOLLOW forced on the final component -- free, and it already refuses the
// final-component swap, the cheapest slice of the race D6 accepts today.

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

#include <sys/stat.h>

#include <hermes/core/sandbox.h>

namespace hermes {

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
  };
  Kind kind = Kind::Kernel;
  int code = 0;
  std::uint64_t size = 0;
  std::uint64_t cap = 0;
};

[[nodiscard]] std::string to_string(const IoError& e);

/// The read cap's stand-in default. The cap is *configuration* (ROUTING.md
/// section 9): when a composition layer exists it passes the configured value
/// into each tool's constructor; nothing reads this constant at run time
/// except those constructors' defaults.
inline constexpr std::uint64_t kDefaultMaxReadBytes = 16u * 1024 * 1024;

/// Open `path`. O_NOFOLLOW and O_CLOEXEC are OR'd in unconditionally: the
/// resolved path had its symlinks expanded at resolve time, so a symlink in
/// the final component now means the name was retargeted afterwards --
/// refused (ELOOP), never followed.
[[nodiscard]] std::expected<Fd, IoError> open_in_root(const SandboxPath& path, int flags);

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

/// Create an exclusive temp file in `target`'s directory -- same directory so
/// the later link()/rename() publication is same-filesystem and atomic --
/// fill it with `bytes`, and set `mode` exactly (fchmod, not umask-masked).
/// Returns the temp's absolute path; the caller publishes or unlinks it. A
/// crash in between leaves a ".hermes-tmp." orphan beside the target, which
/// is visible, greppable, and harmless.
[[nodiscard]] std::expected<std::filesystem::path, IoError> write_temp_beside(
    const SandboxPath& target, std::string_view bytes, ::mode_t mode);

}  // namespace hermes
