#pragma once

// `delete` -- ROUTING.md section 11, admitted 2026-09-04 (DECISIONS.md D19): remove one
// regular file, its bytes preserved first. The one tool whose failure would be
// irreversible without R4, so it is the most gated mutation on the surface:
//
//   the target must have been observed PRESENT this session -- a read, or a listing that
//   saw it, or a write/move that landed it -- and its identity tuple must still match.
//   That is the read-first rule write and edit enforce, applied to the one operation
//   that leaves nothing behind to re-read. A model removes what it has looked at, never
//   what it guessed exists.
//
//   the bytes go to the backup store before the name goes; a failed backup deletes
//   nothing. `hermit undo` lists the generation like any other, and `--restore N`
//   recreates the file at its recorded path (undo.cpp already handles a missing target).
//
// Regular files only, one per call: a directory has no content hash and no single
// generation to restore from, and the surface has no mkdir either. The gate refuses
// anything else by construction -- neither `read` nor `list` records a directory or a
// symlink as Present.
//
// Registered only when the caller asks (toolset.cpp, `--delete`): ninth, after the eight
// structural tools and before `shell`, so the eight's prompt bytes stay what they were.

#include <hermit/core/backup.h>
#include <hermit/core/observed.h>
#include <hermit/core/tool.h>

namespace hermit {

class DeleteTool final : public Tool {
 public:
  DeleteTool(ObservedState& observed, BackupStore& backups) noexcept
      : observed_(observed), backups_(backups) {}

  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;

  ObservedState& observed_;
  BackupStore& backups_;
};

}  // namespace hermit
