#include <hermes/core/fsio.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <system_error>

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
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
    case IoError::Kind::Refused:
      return std::string{e.note};
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

std::expected<std::string, IoError> read_all(const OpenedFile& file,
                                             std::uint64_t max_bytes) {
  const auto claimed = static_cast<std::uint64_t>(file.meta.st_size);
  if (claimed > max_bytes) {
    return std::unexpected{IoError{
        .kind = IoError::Kind::TooLarge, .size = claimed, .cap = max_bytes}};
  }

  std::string bytes;
  // st_size is a reserve hint, not a bound: the file can grow or shrink
  // between the fstat and the reads. EOF decides -- and the cap is enforced
  // again below for exactly that reason.
  if (file.meta.st_size > 0) {
    bytes.reserve(static_cast<std::size_t>(file.meta.st_size));
  }
  char buf[65536];
  for (;;) {
    ssize_t n = ::read(file.fd.get(), buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected{IoError{.code = errno}};
    }
    if (n == 0) break;
    bytes.append(buf, static_cast<std::size_t>(n));
    if (bytes.size() > max_bytes) {
      return std::unexpected{IoError{
          .kind = IoError::Kind::TooLarge, .size = bytes.size(), .cap = max_bytes}};
    }
  }
  return bytes;
}

std::expected<FileContent, IoError> read_file(const SandboxPath& path,
                                              std::uint64_t max_bytes) {
  auto opened = open_regular(path);
  if (!opened) return std::unexpected{opened.error()};

  auto bytes = read_all(*opened, max_bytes);
  if (!bytes) return std::unexpected{bytes.error()};

  FileContent out;
  out.meta = opened->meta;
  out.bytes = std::move(*bytes);
  return out;
}

std::expected<void, IoError> write_all(int fd, std::string_view bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected{IoError{.code = errno}};
    }
    written += static_cast<std::size_t>(n);
  }
  return {};
}

std::expected<std::filesystem::path, IoError> write_temp_beside(
    const SandboxPath& target, std::string_view bytes, ::mode_t mode) {
  const std::filesystem::path parent = target.path().parent_path();
  const std::string base = "." + target.path().filename().string() + ".hermes-tmp.";

  for (int attempt = 0; attempt < 32; ++attempt) {
    // getrandom(2), not std::random_device: the latter can throw, and a
    // terminate is not an acceptable failure mode here any more than it was
    // for the JSON dump (D2) -- entropy trouble is an IoError like any other.
    std::array<unsigned char, 8> rnd{};
    std::size_t filled = 0;
    while (filled < rnd.size()) {
      const ssize_t got = ::getrandom(rnd.data() + filled, rnd.size() - filled, 0);
      if (got < 0) {
        if (errno == EINTR) continue;
        return std::unexpected{IoError{.code = errno}};
      }
      filled += static_cast<std::size_t>(got);
    }
    std::string suffix;
    suffix.reserve(16);
    static constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char byte : rnd) {
      suffix.push_back(kHex[byte >> 4]);
      suffix.push_back(kHex[byte & 0x0f]);
    }
    const std::filesystem::path candidate = parent / (base + suffix);

    const int fd = ::open(candidate.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
      if (errno == EEXIST) continue;
      return std::unexpected{IoError{.code = errno}};
    }
    Fd owned{fd};
    if (auto written = write_all(owned.get(), bytes); !written) {
      ::unlink(candidate.c_str());
      return std::unexpected{written.error()};
    }
    // fchmod, not the open() mode: open's mode is masked by umask, and the
    // replace path needs the original file's mode preserved exactly.
    if (::fchmod(owned.get(), mode) != 0) {
      const int e = errno;
      ::unlink(candidate.c_str());
      return std::unexpected{IoError{.code = e}};
    }
    return candidate;
  }
  return std::unexpected{IoError{.code = EEXIST}};
}

}  // namespace hermes
