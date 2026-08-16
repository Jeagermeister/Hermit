#pragma once

// `grep` -- ROUTING.md section 4: content matches as path, line, text sibling
// fields, one row per matching line. The pattern is a LITERAL byte substring,
// settled 2026-08-16 and recorded in section 4: models emit literal fragments,
// a literal miss is a visible zero-match rather than a silent regex surprise,
// and nothing is interpolated. Zero matches is a valid answer (zero rows); an
// empty pattern is a refusal, not a match-everything (section 3).
//
// Matching is line-granular: a pattern containing '\n' can never match, and
// that is visible in the zero-match result rather than special-cased.

#include <cstdint>

#include <hermes/core/fsio.h>
#include <hermes/core/tool.h>

namespace hermes {

class GrepTool final : public Tool {
 public:
  /// Same cap and same reasoning as `read`: grep loads bodies to scan them.
  explicit GrepTool(std::uint64_t max_file_bytes = kDefaultMaxReadBytes) noexcept
      : max_file_bytes_(max_file_bytes) {}

  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;

  std::uint64_t max_file_bytes_;
};

}  // namespace hermes
