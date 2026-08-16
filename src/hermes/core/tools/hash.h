#pragma once

// `hash` -- ROUTING.md section 4: content hashes for a path set. The result
// *is* the verification (R3): confirming twenty files landed correctly costs
// a hash set, not twenty file bodies. Same one-whole-job rule as `read`: any
// unreadable file refuses the whole call, naming the file.

#include <hermes/core/tool.h>

namespace hermes {

class HashTool final : public Tool {
 public:
  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;
};

}  // namespace hermes
