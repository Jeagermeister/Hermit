#pragma once

// `write` -- ROUTING.md section 4: write, read back, compare (R5), with an R4
// backup on replacement, gated by the staleness table:
//
//   unseen or absent  -> create-if-absent, and the create is HONEST: it
//                        publishes by link()-no-replace, so a file created
//                        after the check is still preserved and still
//                        rejected -- the refusal that stops a model
//                        overwriting a file it never read (the 05_copy shape).
//   present at T      -> replace only while the tuple still matches T; the
//                        old bytes go to the backup store first.
//
// Missing parent directories are created: they are means to the one whole
// job, not judgment (settled 2026-08-16 -- the surface has no mkdir tool, and
// refusing would force models through shell for a mechanical step). A replace
// preserves the file's mode; a create honors the umask over 0666.

#include <hermit/core/backup.h>
#include <hermit/core/observed.h>
#include <hermit/core/tool.h>

namespace hermit {

class WriteTool final : public Tool {
 public:
  WriteTool(ObservedState& observed, BackupStore& backups) noexcept
      : observed_(observed), backups_(backups) {}

  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;

  ObservedState& observed_;
  BackupStore& backups_;
};

}  // namespace hermit
