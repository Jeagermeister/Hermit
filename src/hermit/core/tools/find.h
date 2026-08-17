#pragma once

// `find` -- ROUTING.md section 4: paths whose *name* matches an fnmatch(3)
// glob, walking depth-first in sorted order from an explicit start directory.
// The dialect is settled 2026-08-16 as a starting point and marked for
// revisit in section 4 -- path-globs (src/**/*.c) and case-folding are the
// plausible future asks, and they belong in the doc first.
//
// Symlinks are never followed, neither for recursion nor for matching: a
// symlink is itself an entry (its name can match), but nothing behind it is
// walked, so no result can name a file outside the root and cycles cannot
// occur. Zero matches is a valid answer; an unreadable directory mid-walk
// refuses the whole call, never a silent skip (section 3).

#include <hermit/core/tool.h>

namespace hermit {

class FindTool final : public Tool {
 public:
  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;
};

}  // namespace hermit
