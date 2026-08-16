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
// to four digits and keep growing past 9999. Enumeration and restore are the
// supervisor's side (Phase 3); this type only ever adds.
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
#include <string_view>

#include <hermes/core/fsio.h>

namespace hermes {

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

}  // namespace hermes
