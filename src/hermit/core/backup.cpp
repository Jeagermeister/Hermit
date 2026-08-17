#include <hermit/core/backup.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace hermit {
namespace {

std::string generation_name(std::uint64_t seq) {
  std::string n = std::to_string(seq);
  while (n.size() < 4) n.insert(n.begin(), '0');
  return n;
}

bool is_numeric(std::string_view name) {
  return !name.empty() &&
         std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; });
}

/// A failed backup must not leave a truncated file behind: a partial copy in
/// the archive is indistinguishable from a good generation, which is worse
/// than no generation at all. Unlink, then report the original error.
std::unexpected<IoError> discard(const std::filesystem::path& target, IoError e) {
  ::unlink(target.c_str());
  return std::unexpected{e};
}

}  // namespace

std::expected<std::filesystem::path, IoError> BackupStore::begin(
    const std::filesystem::path& relative) {
  if (relative.is_absolute()) {
    return std::unexpected{
        IoError{.kind = IoError::Kind::Refused,
                .note = "backup path must be sandbox-relative, not absolute"}};
  }
  for (const auto& component : relative) {
    if (component == "..") {
      return std::unexpected{
          IoError{.kind = IoError::Kind::Refused,
                  .note = "backup path must not contain '..'"}};
    }
  }

  if (!scanned_) {
    // A reused directory continues numbering after what is already there --
    // a fresh session must never collide with (or overwrite) an older
    // session's generations.
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) return std::unexpected{IoError{.code = ec.value()}};

    std::filesystem::directory_iterator it{dir_, ec};
    if (ec) return std::unexpected{IoError{.code = ec.value()}};
    const std::filesystem::directory_iterator end;
    while (it != end) {
      const std::string name = it->path().filename().string();
      if (is_numeric(name)) {
        std::uint64_t value = 0;
        std::from_chars(name.data(), name.data() + name.size(), value);
        seq_ = std::max(seq_, value + 1);
      }
      it.increment(ec);
      if (ec) return std::unexpected{IoError{.code = ec.value()}};
    }
    scanned_ = true;
  }

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

  const int fd = ::open(target->c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return std::unexpected{IoError{.code = errno}};
  Fd owned{fd};
  if (auto written = write_all(owned.get(), bytes); !written) {
    return discard(*target, written.error());
  }
  return target;
}

std::expected<std::filesystem::path, IoError> BackupStore::preserve_fd(
    const std::filesystem::path& relative, int src_fd) {
  auto target = begin(relative);
  if (!target) return target;

  const int fd = ::open(target->c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return std::unexpected{IoError{.code = errno}};
  Fd owned{fd};

  char buf[65536];
  off_t offset = 0;
  for (;;) {
    const ssize_t n = ::pread(src_fd, buf, sizeof buf, offset);
    if (n < 0) {
      if (errno == EINTR) continue;
      return discard(*target, IoError{.code = errno});
    }
    if (n == 0) break;
    if (auto written = write_all(owned.get(), {buf, static_cast<std::size_t>(n)});
        !written) {
      return discard(*target, written.error());
    }
    offset += n;
  }
  return target;
}

}  // namespace hermit
