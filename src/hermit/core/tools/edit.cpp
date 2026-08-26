#include <hermit/core/tools/edit.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hermit/core/fsio.h>
#include <hermit/core/sha256.h>

namespace hermit {
namespace {

constexpr std::array<ArgSpec, 3> kArgs{{
    {.name = "path",
     .type = ArgType::Path,
     .required = true,
     .doc = "file to edit; must have been read or listed this session"},
    {.name = "old",
     .type = ArgType::String,
     .required = true,
     .doc = "exact bytes to replace; must occur exactly once in the file"},
    {.name = "new",
     .type = ArgType::String,
     .required = true,
     .doc = "exact replacement bytes; may be empty to delete `old`"},
}};

const ToolSpec kSpec{
    "edit",
    "Replace one exact occurrence of `old` with `new` in a file observed this session.",
    kArgs};

std::unexpected<ToolError> refuse(const SandboxPath& p, std::string_view why) {
  return std::unexpected{
      ToolError{"edit: " + p.relative().string() + ": " + std::string{why}}};
}

}  // namespace

const ToolSpec& EditTool::spec() const noexcept { return kSpec; }

std::expected<ToolOutput, ToolError> EditTool::run(const ToolArgs& args) {
  const SandboxPath& target = *args.path("path");
  const std::string& old_bytes = *args.string("old");
  const std::string& new_bytes = *args.string("new");

  if (old_bytes.empty()) {
    return refuse(target, "`old` is empty; there is nothing to find");
  }

  const auto view = observed_.lookup(target.relative());
  switch (view.status) {
    case ObservedState::Status::Unseen:
      return refuse(target, "never read or listed this session; read it first");
    case ObservedState::Status::Absent:
      return refuse(target, "observed absent this session; not found");
    case ObservedState::Status::Present:
      break;
  }

  // One open carries everything: the staleness stat, the bytes to edit, the
  // R4 backup source, and the mode to preserve.
  auto current = open_regular(target);
  if (!current) {
    if (current.error().kind == IoError::Kind::Kernel &&
        current.error().code == ENOENT) {
      observed_.record_absent(target.relative());
      return refuse(target, "was present when last observed, now missing");
    }
    return refuse(target, to_string(current.error()));
  }
  if (tuple_from(current->meta) != view.tuple) {
    // Not recorded: a recorded fresh tuple would let a retry pass the guard
    // without re-reading content -- the read-first rule minus the reading.
    return refuse(target, "changed since last observed; read it again first");
  }

  auto bytes = read_all(*current, max_file_bytes_);
  if (!bytes) return refuse(target, to_string(bytes.error()));

  // One scan settles both questions: where `old` is, and whether it is unique.
  const std::size_t pos = bytes->find(old_bytes);
  if (pos == std::string::npos) {
    return refuse(target, "`old` not found (0 occurrences); re-read the file "
                          "and match its bytes exactly");
  }
  if (bytes->find(old_bytes, pos + 1) != std::string::npos) {
    return refuse(target, "`old` is ambiguous (2 or more occurrences); "
                          "include more surrounding context so it is unique");
  }

  if (auto kept = backups_.preserve(target.relative(), *bytes); !kept) {
    return refuse(target, "backup failed: " + to_string(kept.error()) +
                              "; nothing was written");
  }

  // Walked from sandbox_root(), not target.path()'s string -- an interior symlink
  // swapped in after resolve() is refused here, never followed (ROUTING.md section 12
  // step 5). `false`: edit never creates parent directories.
  auto parent = open_parent_in_root(target, /*create_missing=*/false);
  if (!parent) return refuse(target, "cannot reach parent directory: " + to_string(parent.error()));

  std::string edited;
  edited.reserve(bytes->size() - old_bytes.size() + new_bytes.size());
  edited.append(*bytes, 0, pos);
  edited += new_bytes;
  edited.append(*bytes, pos + old_bytes.size(), std::string::npos);

  auto temp = write_temp_in_dir(parent->dir.get(), parent->name, edited,
                                current->meta.st_mode & 07777);
  if (!temp) return refuse(target, to_string(temp.error()));
  if (::renameat2(parent->dir.get(), temp->c_str(), parent->dir.get(),
                  parent->name.c_str(), 0) != 0) {
    const int e = errno;
    ::unlinkat(parent->dir.get(), temp->c_str(), 0);
    return refuse(target, to_string(IoError{.code = e}));
  }

  // R5: read back, compare, and let the read-back's stat be the observation.
  auto back = read_file(target, std::max<std::uint64_t>(edited.size(), 1));
  if (!back) return refuse(target, "read-back failed: " + to_string(back.error()));
  if (back->bytes != edited) {
    return refuse(target, "read-back does not match the edited content (R5); "
                          "the file is in an unexpected state");
  }
  observed_.record_present(target.relative(), tuple_from(back->meta));

  // The fresh identity tuple is recorded above, supervisor-side; the rendered
  // row carries only what the model can use.
  ToolOutput out;
  out.rows.push_back({{
      {"path", target.relative().string()},
      {"hash", sha256_hex(edited)},
      {"size", tuple_from(back->meta).size},
  }});
  return out;
}

}  // namespace hermit
