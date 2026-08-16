#pragma once

// `read` -- ROUTING.md section 4: the exact bytes of one or more files, with a
// content hash alongside. Section 5 is the law here: no decoration, ever. The
// bytes travel exact in one field, hash and path as siblings, nothing
// interleaved -- the N| prefix incident is why that sentence exists.
//
// One whole job (section 3): if any requested file cannot be read, the call is
// refused naming that file, never a partial answer.

#include <hermes/core/tool.h>

namespace hermes {

class ReadTool final : public Tool {
 public:
  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;
};

}  // namespace hermes
