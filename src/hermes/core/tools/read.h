#pragma once

// `read` -- ROUTING.md section 4: the exact bytes of one or more files, with a
// content hash alongside. Section 5 is the law here: no decoration, ever. The
// bytes travel exact in one field, hash and path as siblings, nothing
// interleaved -- the N| prefix incident is why that sentence exists.
//
// One whole job (section 3): if any requested file cannot be read, the call is
// refused naming that file, never a partial answer. That includes files over
// the read cap -- refused with guidance toward `hash` and `list`, which answer
// at any size, never truncated.

#include <cstdint>

#include <hermes/core/fsio.h>
#include <hermes/core/tool.h>

namespace hermes {

class ReadTool final : public Tool {
 public:
  /// The cap is configuration (ROUTING.md section 9); the composition layer
  /// passes the configured value once one exists. The default stands in
  /// until then.
  explicit ReadTool(std::uint64_t max_file_bytes = kDefaultMaxReadBytes) noexcept
      : max_file_bytes_(max_file_bytes) {}

  [[nodiscard]] const ToolSpec& spec() const noexcept override;

 private:
  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override;

  std::uint64_t max_file_bytes_;
};

}  // namespace hermes
