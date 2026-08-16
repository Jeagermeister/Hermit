#include <hermes/core/backup.h>

#include <cerrno>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace hermes {
namespace {

std::string generation_name(std::uint64_t seq) {
  std::string n = std::to_string(seq);
  while (n.size() < 4) n.insert(n.begin(), '0');
  return n;
}

}  // namespace

std::expected<std::filesystem::path, IoError> BackupStore::begin(
    const std::filesystem::path& relative) {
  const std::filesystem::path target = dir_ / generation_name(seq_) / relative;
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  if (ec) return std::unexpected{IoError{.code = ec.value()}};
  ++seq_;
  return target;
}

std::expected<std::filesystem::path, IoError> BackupStore::preserve(
    const std::filesystem::path& relative, std::string_view bytes) {
  auto target = begin(relative);
  if (!target) return target;

  const int fd = ::open(target->c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return std::unexpected{IoError{.code = errno}};
  Fd owned{fd};
  if (auto written = write_all(owned.get(), bytes); !written) {
    return std::unexpected{written.error()};
  }
  return target;
}

std::expected<std::filesystem::path, IoError> BackupStore::preserve_fd(
    const std::filesystem::path& relative, int src_fd) {
  auto target = begin(relative);
  if (!target) return target;

  const int fd = ::open(target->c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return std::unexpected{IoError{.code = errno}};
  Fd owned{fd};

  char buf[65536];
  off_t offset = 0;
  for (;;) {
    const ssize_t n = ::pread(src_fd, buf, sizeof buf, offset);
    if (n < 0) {
      if (errno == EINTR) continue;
      return std::unexpected{IoError{.code = errno}};
    }
    if (n == 0) break;
    if (auto written = write_all(owned.get(), {buf, static_cast<std::size_t>(n)});
        !written) {
      return std::unexpected{written.error()};
    }
    offset += n;
  }
  return target;
}

}  // namespace hermes
