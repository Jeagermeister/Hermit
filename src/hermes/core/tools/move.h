#pragma once

// `move` -- ROUTING.md section 4: hash source, move, verify destination, R3
// at both ends. Regular files only: hashing is the verification, and a
// directory has no content hash.
//
// Two decisions settled 2026-08-16:
//
// UNGATED by observed state. Unlike write and edit, a move preserves content
// byte-for-byte and R3 proves it did; only the *name* is at risk, and undo is
// moving it back. Gating would force full-body reads just to rename, which
// the read cap makes hostile for exactly the files most worth not loading.
//
// NEVER onto an existing destination -- renameat2(RENAME_NOREPLACE), so the
// silent-destruction shape (05_copy) is structurally impossible. The refusal
// carries the file-manager affordance as a SUGGESTION: it names a free
// "name (N)"-style destination, and the caller moves there explicitly if a
// renamed copy is what it wants. The destination a caller names is the
// destination it gets (section 3); the tool never picks a different one.

#include <hermes/core/observed.h>
#include <hermes/core/tool.h>

namespace hermes {

class MoveTool final : public Tool {
 public:
  explicit MoveTool(ObservedState& observed) noexcept : observed_(observed) {}

  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;

  ObservedState& observed_;
};

}  // namespace hermes
