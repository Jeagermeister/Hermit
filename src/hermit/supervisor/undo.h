#pragma once

// R4's other half -- enumeration, restore and retention over the backup store.
// backup.h says "this type only ever adds"; this module is the side that reads
// and removes, and it lives in the supervisor because that is who the store
// belongs to (DECISIONS.md D10: backups are never granted to the child).
//
// Three operations, three postures:
//
//   enumerate -- read-only. A store that does not exist is an empty store, not
//   an error: nothing was ever preserved, so there is nothing to list.
//
//   restore -- a mutation, and it obeys the same rule as every other mutation
//   in this codebase: back up before you overwrite. Restoring generation N
//   first preserves the target's current bytes as a new generation, so no
//   invocation of undo can ever lose data, and redo falls out for free. The
//   relative path recorded in the store is resolved through Sandbox::resolve
//   before anything is written -- the store refuses absolute and `..` paths at
//   preserve time, but a directory an operator can point `--backups` at is a
//   directory an operator can hand-edit, and a hand-edited store must not be
//   able to write outside the root.
//
//   prune -- destructive, and therefore the most guarded. It refuses to run on
//   any directory that does not carry the store marker (kStoreMarker, written
//   by BackupStore on first use): retention runs automatically at agent start
//   on an operator-supplied path, and `--backups` pointed at the wrong
//   directory must never delete things that merely have numeric names.
//   Retention itself was settled 2026-08-18: generations older than a short
//   window (default in main.cpp, 72 hours) are pruned at startup, because the
//   sandboxed trees this project supervises live under git -- the store covers
//   the gap between a bad mutation and the operator noticing, not the archive.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <hermit/core/backup.h>  // kStoreMarker -- written by core, honoured here
#include <hermit/core/sandbox.h>

namespace hermit::supervisor {

/// One preserved file: generation `name` holding the pre-mutation bytes of
/// `relative`. By construction a generation holds exactly one file; a
/// hand-edited store can hold more, which enumerate reports as one row per
/// file and restore refuses as ambiguous.
struct Generation {
  std::uint64_t seq = 0;
  std::string name;                      // zero-padded directory name on disk
  std::filesystem::path relative;        // sandbox-relative path of the preserved file
  std::uintmax_t bytes = 0;
  std::filesystem::file_time_type when;  // preservation time (backup file mtime)
};

/// Every generation in the store, ordered oldest first. A missing store is an
/// empty store; an existing directory without kStoreMarker is an error, stated
/// so list/restore/prune all agree on what counts as a store.
[[nodiscard]] std::expected<std::vector<Generation>, std::string> enumerate(
    const std::filesystem::path& store);

struct Restored {
  Generation generation;  // what was restored
  /// Where the target's pre-restore bytes went -- a new generation in the same
  /// store. Empty when the target did not exist, because absence has no bytes
  /// to preserve.
  std::optional<std::filesystem::path> preserved;
};

/// Write generation `seq`'s bytes back to its recorded path inside `box`,
/// atomically (temp beside the target, then rename). The target's current
/// content, if any, is preserved as a new generation first.
[[nodiscard]] std::expected<Restored, std::string> restore(
    const std::filesystem::path& store, Sandbox& box, std::uint64_t seq);

struct Pruned {
  std::size_t generations = 0;
  std::uintmax_t bytes = 0;
};

/// Remove every generation whose newest file is older than `now - keep`.
/// Touches only numeric-named children; refuses entirely without kStoreMarker.
/// A missing store prunes to nothing successfully.
[[nodiscard]] std::expected<Pruned, std::string> prune(
    const std::filesystem::path& store, std::chrono::hours keep,
    std::filesystem::file_time_type now);

}  // namespace hermit::supervisor
