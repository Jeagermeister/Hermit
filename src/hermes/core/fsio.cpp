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
  switch (e.kind) {
    case IoError::Kind::Kernel:
      return std::system_category().message(e.code);
    case IoError::Kind::NotRegular:
      return "not a regular file";
    case IoError::Kind::TooLarge:
      return "file is " + std::to_string(e.size) + " bytes, over the " +
             std::to_string(e.cap) + "-byte read cap";
  }
  return "unknown I/O error";
}

std::expected<Fd, IoError> open_in_root(const SandboxPath& path, int flags) {
  int fd = ::open(path.path().c_str(), flags | O_NOFOLLOW | O_CLOEXEC, 0666);
  if (fd < 0) return std::unexpected{IoError{.code = errno}};
  return Fd{fd};
}

std::expected<OpenedFile, IoError> open_regular(const SandboxPath& path) {
  auto fd = open_in_root(path, O_RDONLY | O_NONBLOCK);
  if (!fd) return std::unexpected{fd.error()};

  OpenedFile out;
  out.fd = std::move(*fd);
  if (::fstat(out.fd.get(), &out.meta) != 0) {
    return std::unexpected{IoError{.code = errno}};
  }
  if (!S_ISREG(out.meta.st_mode)) {
    return std::unexpected{IoError{.kind = IoError::Kind::NotRegular}};
  }
  return out;
}

std::expected<FileContent, IoError> read_file(const SandboxPath& path,
                                              std::uint64_t max_bytes) {
  auto opened = open_regular(path);
  if (!opened) return std::unexpected{opened.error()};

  const auto claimed = static_cast<std::uint64_t>(opened->meta.st_size);
  if (claimed > max_bytes) {
    return std::unexpected{IoError{
        .kind = IoError::Kind::TooLarge, .size = claimed, .cap = max_bytes}};
  }

  FileContent out;
  out.meta = opened->meta;
  // st_size is a reserve hint, not a bound: the file can grow or shrink
  // between the fstat and the reads. EOF decides -- and the cap is enforced
  // again below for exactly that reason.
  if (opened->meta.st_size > 0) {
    out.bytes.reserve(static_cast<std::size_t>(opened->meta.st_size));
  }
  char buf[65536];
  for (;;) {
    ssize_t n = ::read(opened->fd.get(), buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected{IoError{.code = errno}};
    }
    if (n == 0) break;
    out.bytes.append(buf, static_cast<std::size_t>(n));
    if (out.bytes.size() > max_bytes) {
      return std::unexpected{IoError{.kind = IoError::Kind::TooLarge,
                                     .size = out.bytes.size(),
                                     .cap = max_bytes}};
    }
  }
  return out;
}

}  // namespace hermes
