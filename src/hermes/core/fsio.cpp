#include <hermes/core/fsio.h>

#include <cerrno>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace hermes {

void Fd::reset() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

std::string to_string(const IoError& e) {
  if (!e.note.empty()) return std::string{e.note};
  return std::system_category().message(e.code);
}

std::expected<Fd, IoError> open_in_root(const SandboxPath& path, int flags) {
  int fd = ::open(path.path().c_str(), flags | O_NOFOLLOW | O_CLOEXEC, 0666);
  if (fd < 0) return std::unexpected{IoError{.code = errno}};
  return Fd{fd};
}

std::expected<FileContent, IoError> read_file(const SandboxPath& path) {
  // O_NONBLOCK so an open on a planted FIFO returns instead of blocking until
  // a writer appears; it has no effect on regular files, and the S_ISREG check
  // below refuses the FIFO once it is safely open.
  auto fd = open_in_root(path, O_RDONLY | O_NONBLOCK);
  if (!fd) return std::unexpected{fd.error()};

  FileContent out;
  if (::fstat(fd->get(), &out.meta) != 0) {
    return std::unexpected{IoError{.code = errno}};
  }
  if (!S_ISREG(out.meta.st_mode)) {
    return std::unexpected{IoError{.note = "not a regular file"}};
  }

  // st_size is a reserve hint, not a bound: the file can grow or shrink
  // between the fstat and the reads. EOF decides, not the hint.
  if (out.meta.st_size > 0) {
    out.bytes.reserve(static_cast<std::size_t>(out.meta.st_size));
  }
  char buf[65536];
  for (;;) {
    ssize_t n = ::read(fd->get(), buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected{IoError{.code = errno}};
    }
    if (n == 0) break;
    out.bytes.append(buf, static_cast<std::size_t>(n));
  }
  return out;
}

}  // namespace hermes
