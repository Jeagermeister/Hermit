#pragma once

// `list` -- ROUTING.md section 4: directory entries with type and the
// dev:ino:size:mtime:ctime identity tuple, at O(1) per entry. Identity, not
// content: an observation from `list` is directly usable as the expected
// value on a later `edit`, which is the one-currency property section 4
// argues for. Times travel as nanoseconds since the epoch in sibling fields.
//
// Entries are name-sorted so output is deterministic across runs and
// filesystems; zero entries is the correct answer for an empty directory,
// not a failure. A vanished or unstattable entry refuses the whole call --
// silently skipping it would be the adjacent-success section 3 forbids.

#include <hermit/core/observed.h>
#include <hermit/core/tool.h>

namespace hermit {

class ListTool final : public Tool {
 public:
  /// A successful listing records presence for its regular-file entries --
  /// section 4's one-currency property: an observation from `list` is
  /// directly usable as the expected value on a later `edit`. Only regular
  /// files: directories are not edit targets, and a symlink entry's tuple
  /// describes the link, not the file a resolved path would name.
  explicit ListTool(ObservedState& observed) noexcept : observed_(observed) {}

  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;

  ObservedState& observed_;
};

}  // namespace hermit
