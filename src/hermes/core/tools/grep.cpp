#include <hermes/core/tools/grep.h>

#include <array>
#include <string>
#include <string_view>

namespace hermes {
namespace {

constexpr std::array<ArgSpec, 2> kArgs{{
    {.name = "pattern",
     .type = ArgType::String,
     .required = true,
     .doc = "literal byte substring to match within lines; not a regex"},
    {.name = "paths",
     .type = ArgType::PathList,
     .required = true,
     .doc = "files to search, one or more, relative to the sandbox root"},
}};

const ToolSpec kSpec{
    "grep",
    "Lines containing a literal substring: path, 1-based line, exact line bytes per match.",
    kArgs};

}  // namespace

const ToolSpec& GrepTool::spec() const noexcept { return kSpec; }

std::expected<ToolOutput, ToolError> GrepTool::run(const ToolArgs& args) {
  const std::string& pattern = *args.string("pattern");
  if (pattern.empty()) {
    return std::unexpected{ToolError{
        "grep: empty pattern -- a match-everything search is declined, not answered"}};
  }

  ToolOutput out;
  for (const SandboxPath& p : args.paths("paths")) {
    auto file = read_file(p, max_file_bytes_);
    if (!file) {
      return std::unexpected{ToolError{
          "grep: " + p.relative().string() + ": " + to_string(file.error())}};
    }

    const std::string_view bytes{file->bytes};
    std::uint64_t line_no = 1;
    std::size_t pos = 0;
    while (pos <= bytes.size()) {
      const std::size_t nl = bytes.find('\n', pos);
      const std::string_view line = (nl == std::string_view::npos)
                                        ? bytes.substr(pos)
                                        : bytes.substr(pos, nl - pos);
      // The final segment after the last '\n' is a line only if non-empty --
      // a trailing newline does not manufacture a phantom empty line.
      if ((nl != std::string_view::npos || !line.empty()) &&
          line.find(pattern) != std::string_view::npos) {
        out.rows.push_back({{{"path", p.relative().string()},
                             {"line", line_no},
                             {"text", std::string{line}}}});
      }
      if (nl == std::string_view::npos) break;
      pos = nl + 1;
      ++line_no;
    }
  }
  return out;
}

}  // namespace hermes
