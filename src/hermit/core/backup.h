#pragma once

// R4 -- back up before mutating; undo is a first-class operation, not a
// debugging aid. The store lives OUTSIDE the sandbox root, deliberately
// (settled 2026-08-16): the model can never list, read, edit or move its own
// undo data, and the sandbox's `list`/`find` results stay free of archive
// noise. The cost, accepted: backups are copies, not links, because
// same-filesystem is not guaranteed across the root boundary. Placement is
// the composition layer's contract to honor -- this type cannot know where
// the sandbox root is.
//
// Every preservation keeps its own generation -- repeated mutations of one
// file never overwrite an earlier backup, and a directory reused across
// sessions continues numbering after the generations already in it. Order
// generations by NUMERIC value, not lexicographically: names are zero-padded
// to four digits and keep growing past 9999. Enumeration, restore and
// retention are the supervisor's side (supervisor/undo.h); this type only
// ever adds.
//
// Backup paths are host-absolute and must never reach a model-facing result:
// SandboxPath::relative() exists so the host layout does not leak, and the
// archive is part of the host layout.
//
// Tools hold a BackupStore& for their whole life; the composition layer must
// keep the store alive at least as long as the registry that holds them.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>

#include <hermit/core/fsio.h>

namespace hermit {

/// Dropped at the store root the first time anything is preserved. Its
/// existence is the assertion "hermit owns this directory's numeric children":
/// retention (supervisor/undo.h) is destructive and runs automatically on an
/// operator-supplied path, so it refuses to delete anything from a directory
/// that lacks this file -- `--backups` pointed at the wrong place must never
/// prune a directory that merely has numeric names in it.
///
/// The marker also carries the store's **numbering floor**: a line `next N`
/// (absent until retention first removes something) below the banner line.
/// Scanning the directory recovers the sequence only while the newest
/// generation is still on disk; retention that removes *everything* would let
/// the next preservation restart at 0000 and hand a fresh backup a pruned
/// one's identity in the listing -- exactly what D14 promises cannot happen.
/// So whoever deletes a generation raises the floor FIRST (undo.cpp does),
/// and `begin` numbers from `max(scan, floor)`. A floor that cannot be parsed
/// is treated as absent: the generations a crash left behind still carry the
/// scan, and a corrupt second line must not brick the store.
inline constexpr std::string_view kStoreMarker = ".hermit-store";

/// The marker's fixed first line.
inline constexpr std::string_view kStoreBanner = "hermit backup store";

/// The numbering floor recorded in `dir`'s marker, if one is set and parseable.
[[nodiscard]] std::optional<std::uint64_t> read_store_floor(
    const std::filesystem::path& dir);

/// Record `next` as the numbering floor, never lowering an existing one.
/// Plain rewrite, not atomic: single-threaded (D1), and a torn write parses
/// as "no floor", which the scan covers as long as the caller raises the
/// floor BEFORE deleting the generations that carry it.
[[nodiscard]] std::expected<void, IoError> raise_store_floor(
    const std::filesystem::path& dir, std::uint64_t next);

/// A directory name that is a generation number: all digits AND within range.
/// A 25-digit name is numeric but can never have been written by
/// `generation_name`, so it is not a generation -- parsing it modulo 2^64
/// would collide it with a real one in listings and scans.
[[nodiscard]] std::optional<std::uint64_t> parse_generation_name(
    std::string_view name);

class BackupStore {
 public:
  /// `dir` is supervisor-provided configuration (ROUTING.md section 9), like
  /// the read cap: passed at composition, created lazily on first use.
  explicit BackupStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

  /// Preserve `bytes` as the pre-mutation content of `relative`.
  /// Returns the backup's absolute path -- for the supervisor, never for a
  /// model-facing row.
  ///
  /// `relative` must be a clean sandbox-relative path -- what
  /// SandboxPath::relative() produces. Absolute paths and `..` components
  /// are refused rather than trusted: the store must not be steerable
  /// outside its own directory, and at tool #37 that is the check that gets
  /// forgotten (D6's argument, applied to the archive).
  std::expected<std::filesystem::path, IoError> preserve(
      const std::filesystem::path& relative, std::string_view bytes);

  /// Preserve by streaming from an open descriptor (pread from offset zero,
  /// so the caller's file position is untouched). Constant memory: R4 must
  /// not degrade with file size any more than `hash` does.
  std::expected<std::filesystem::path, IoError> preserve_fd(
      const std::filesystem::path& relative, int src_fd);

  [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }

 private:
  std::expected<std::filesystem::path, IoError> begin(
      const std::filesystem::path& relative);

  std::filesystem::path dir_;
  std::uint64_t seq_ = 0;
  bool scanned_ = false;
};

}  // namespace hermit
