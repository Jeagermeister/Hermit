#pragma once

// The one open primitive -- ROUTING.md section 12 step 4, settled 2026-08-16:
// allow the semantics, funnel the spelling. Tools never spell ::open
// themselves; every open of a SandboxPath goes through open_in_root, so when
// D7's gate lands, the openat(O_NOFOLLOW) component walk replaces this one
// function body and no tool changes. Until then the body is a plain open with
// O_NOFOLLOW forced on the final component -- free, and it already refuses the
// final-component swap, the cheapest slice of the race D6 accepts today.

#include <expected>
#include <string>
#include <string_view>
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

/// An I/O refusal, reported as data. `code` is errno when the kernel refused;
/// `note` is set instead when the refusal is ours ("not a regular file").
struct IoError {
  int code = 0;
  std::string_view note{};
};

[[nodiscard]] std::string to_string(const IoError& e);

/// Open `path`. O_NOFOLLOW and O_CLOEXEC are OR'd in unconditionally: the
/// resolved path had its symlinks expanded at resolve time, so a symlink in
/// the final component now means the name was retargeted afterwards --
/// refused (ELOOP), never followed.
[[nodiscard]] std::expected<Fd, IoError> open_in_root(const SandboxPath& path, int flags);

/// A regular file's exact bytes plus the stat taken from the open fd -- the
/// same object the bytes came from, even if the name was retargeted since.
struct FileContent {
  std::string bytes;
  struct ::stat meta {};
};

/// Read one regular file completely. Refuses anything else -- a directory,
/// device, socket or FIFO is never "content" (ROUTING.md section 3: no answer
/// is a valid answer, and a blocking open on a planted FIFO is no answer).
[[nodiscard]] std::expected<FileContent, IoError> read_file(const SandboxPath& path);

}  // namespace hermes
