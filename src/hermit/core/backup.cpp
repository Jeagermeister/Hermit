#include <hermit/core/backup.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <limits>
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

    // Mark the directory as ours before the first generation lands: retention
    // is keyed on this file (backup.h), and a store that cannot be marked is a
    // store that can never be pruned or listed, so failing the preservation is
    // the honest answer. EEXIST is the normal case for a reused directory.
    const std::filesystem::path marker = dir_ / kStoreMarker;
    const int mfd = ::open(marker.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (mfd < 0) {
      if (errno != EEXIST) return std::unexpected{IoError{.code = errno}};
    } else {
      Fd owned_marker{mfd};
      if (auto written = write_all(owned_marker.get(),
                                   std::string{kStoreBanner} + "\n");
          !written) {
        return std::unexpected{written.error()};
      }
    }

    std::filesystem::directory_iterator it{dir_, ec};
    if (ec) return std::unexpected{IoError{.code = ec.value()}};
    const std::filesystem::directory_iterator end;
    while (it != end) {
      const std::string name = it->path().filename().string();
      if (const auto value = parse_generation_name(name)) {
        seq_ = std::max(seq_, *value + 1);
      }
      it.increment(ec);
      if (ec) return std::unexpected{IoError{.code = ec.value()}};
    }
    // The floor outlives the generations that carried the scan: retention that
    // removed everything raised it first, and numbering must not rewind past it
    // (backup.h, D14).
    if (const auto floor = read_store_floor(dir_)) {
      seq_ = std::max(seq_, *floor);
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

std::optional<std::uint64_t> parse_generation_name(std::string_view name) {
  if (name.empty() ||
      !std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto [stopped, ec] = std::from_chars(name.data(), name.data() + name.size(), value);
  // Overflow: all digits, but nothing `generation_name` could ever have written.
  if (ec != std::errc{} || stopped != name.data() + name.size()) return std::nullopt;
  // Exactly 2^64-1 parses cleanly and is refused anyway: every consumer follows a
  // generation number with +1 (the scan's next-seq, prune's floor), which would wrap
  // to 0 and hand a planted directory the power to rewind numbering to 0000 -- the
  // D14 violation by the back door. `generation_name` can never emit it (the store
  // would have had to survive 2^64 preservations), so refusing loses nothing.
  if (value == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
  return value;
}

std::optional<std::uint64_t> read_store_floor(const std::filesystem::path& dir) {
  const std::filesystem::path marker = dir / kStoreMarker;
  const int fd = ::open(marker.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return std::nullopt;
  Fd owned{fd};
  // The whole file is two short lines; 256 bytes is generous and bounds a
  // hand-edited marker. EINTR is retried because a floor misread as absent lets
  // raise_store_floor rewrite a higher floor lower -- the one direction backup.h
  // forbids.
  char buf[256];
  ssize_t n = -1;
  do {
    n = ::read(owned.get(), buf, sizeof buf);
  } while (n < 0 && errno == EINTR);
  if (n <= 0) return std::nullopt;
  const std::string_view text{buf, static_cast<std::size_t>(n)};
  const auto at = text.find("next ");
  if (at == std::string_view::npos) return std::nullopt;
  const std::string_view rest = text.substr(at + 5);
  // The digits must END inside the read window: a hand-padded marker whose number
  // straddles the cap would otherwise parse its visible prefix as a smaller floor --
  // silently UNDERSTATING it, the unsafe direction. Our own writer always terminates
  // with a newline, so requiring one costs nothing and turns truncation into
  // "absent", which the header promises is the failure mode.
  const auto end_of_line = rest.find('\n');
  if (end_of_line == std::string_view::npos) return std::nullopt;
  const std::string_view digits = rest.substr(0, end_of_line);
  std::uint64_t value = 0;
  const auto [stopped, ec] =
      std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (ec != std::errc{} || stopped != digits.data() + digits.size()) return std::nullopt;
  return value;
}

std::expected<void, IoError> raise_store_floor(const std::filesystem::path& dir,
                                               std::uint64_t next) {
  if (const auto existing = read_store_floor(dir); existing && *existing >= next) {
    return {};  // never lowered
  }
  const std::filesystem::path marker = dir / kStoreMarker;
  const int fd = ::open(marker.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return std::unexpected{IoError{.code = errno}};
  Fd owned{fd};
  const std::string content =
      std::string{kStoreBanner} + "\nnext " + std::to_string(next) + "\n";
  return write_all(owned.get(), content);
}

}  // namespace hermit
