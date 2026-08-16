#pragma once

// `edit` -- ROUTING.md section 4: exact `old` -> `new`, read back, compare
// (R5), R4 backup, failing closed on a stale identity tuple. The gate is the
// settled table: unseen -> refuse (read first); absent -> refuse (not found);
// present at T -> compare tuples, refuse on mismatch.
//
// The occurrence rule, settled 2026-08-16: `old` must appear EXACTLY ONCE.
// Zero occurrences is "not found"; two or more is "ambiguous, give more
// context" (deliberately uncounted -- two is already ambiguous, and exact
// totals buy nothing). Replace-first would silently guess which occurrence
// was meant -- the adjacent success section 3 forbids -- and replace-all lets
// one confused call rewrite a whole file. Both refusals are actionable.
//
// Shares `read`'s cap and reasoning: the file must be loaded to be edited.

#include <cstdint>

#include <hermes/core/backup.h>
#include <hermes/core/fsio.h>
#include <hermes/core/observed.h>
#include <hermes/core/tool.h>

namespace hermes {

class EditTool final : public Tool {
 public:
  EditTool(ObservedState& observed, BackupStore& backups,
           std::uint64_t max_file_bytes = kDefaultMaxReadBytes) noexcept
      : observed_(observed), backups_(backups), max_file_bytes_(max_file_bytes) {}

  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;

  ObservedState& observed_;
  BackupStore& backups_;
  std::uint64_t max_file_bytes_;
};

}  // namespace hermes
