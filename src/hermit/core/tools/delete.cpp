#include <hermit/core/tools/delete.h>

#include <array>
#include <cerrno>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hermit/core/fsio.h>

namespace hermit {
namespace {

constexpr std::array<ArgSpec, 1> kArgs{{
    {.name = "path",
     .type = ArgType::Path,
     .required = true,
     .doc = "file to delete, relative to the sandbox root; must have been read or listed "
            "this session"},
}};

const ToolSpec kSpec{
    "delete",
    "Delete one regular file you have already read or listed this session.",
    kArgs};

std::unexpected<ToolError> refuse(const SandboxPath& p, std::string_view why) {
  return std::unexpected{
      ToolError{"delete: " + p.relative().string() + ": " + std::string{why}}};
}

}  // namespace

const ToolSpec& DeleteTool::spec() const noexcept { return kSpec; }

std::expected<ToolOutput, ToolError> DeleteTool::run(const ToolArgs& args) {
  const SandboxPath& target = *args.path("path");
  const auto view = observed_.lookup(target.relative());

  // The gate, same vocabulary as edit's. Unlike write there is no create-if-absent arm:
  // absence is the one state this tool has nothing to do with.
  switch (view.status) {
    case ObservedState::Status::Unseen:
      return refuse(target, "never read or listed this session; read or list it first");
    case ObservedState::Status::Absent:
      return refuse(target, "observed absent this session; not found");
    case ObservedState::Status::Present:
      break;
  }

  // One open carries the staleness check, the hash for the result row, and the R4 backup
  // -- all statements about the same object. A directory, or a link swapped in between
  // resolve() and this open, fails here as not-regular and is refused, not followed. A
  // link that was already there when the model named the path never reaches this open:
  // resolve() expanded it, and `target` is its target (delete.h).
  auto current = open_regular(target);
  if (!current) {
    if (current.error().kind == IoError::Kind::Kernel && current.error().code == ENOENT) {
      observed_.record_absent(target.relative());
      return refuse(target, "was present when last observed, now missing; absence is now "
                            "recorded");
    }
    return refuse(target, to_string(current.error()));
  }
  if (tuple_from(current->meta) != view.tuple) {
    // Deliberately NOT recorded as the new observation, for edit's reason: recording here
    // would let an immediate retry pass the guard without re-reading.
    return refuse(target, "changed since last observed; read it again first");
  }
  auto hash = sha256_of_fd(current->fd.get());
  if (!hash) return refuse(target, to_string(hash.error()));

  // R4 strictly before the mutation: the bytes are a generation in the store before the
  // name is touched, and a failed backup deletes nothing.
  if (auto kept = backups_.preserve_fd(target.relative(), current->fd.get()); !kept) {
    return refuse(target, "backup failed: " + to_string(kept.error()) +
                              "; nothing was deleted");
  }

  // Walk from sandbox_root(), not target.path()'s string, so an interior symlink swapped
  // in after resolve() is refused rather than followed (ROUTING.md section 12 step 5).
  // Never creates parents: a missing parent means a missing file.
  auto parent = open_parent_in_root(target, /*create_missing=*/false);
  if (!parent) {
    return refuse(target, "cannot reach parent directory: " + to_string(parent.error()));
  }

  // unlink acts on the NAME, and the gate above checked the OBJECT. Re-check that the name
  // still carries the identity the gate accepted before removing it, so a swap between
  // the two is refused rather than deleting whatever now sits under the name. The window
  // between this check and the unlink is the same one write's rename has.
  // Every refusal from here on happens AFTER the backup was taken: the store holds a spare
  // generation for a file that is still on disk (or already gone). Harmless -- retention is
  // age-based and restore preserves current bytes first -- and the message says so, because
  // "permission denied" alone reads as "nothing happened" and something did.
  struct ::stat by_name {};
  if (::fstatat(parent->dir.get(), parent->name.c_str(), &by_name, AT_SYMLINK_NOFOLLOW) != 0) {
    const int e = errno;
    if (e == ENOENT) {
      observed_.record_absent(target.relative());
      return refuse(target, "was present when last observed, now missing; absence is now "
                            "recorded, and the backup taken a moment ago was kept");
    }
    return refuse(target, to_string(IoError{.code = e}) + "; the backup was kept and nothing "
                                                          "was deleted");
  }
  if (!S_ISREG(by_name.st_mode) || tuple_from(by_name) != tuple_from(current->meta)) {
    return refuse(target, "the name no longer refers to the file that was observed; read it "
                          "again first (the backup was kept and nothing was deleted)");
  }
  if (::unlinkat(parent->dir.get(), parent->name.c_str(), 0) != 0) {
    return refuse(target, to_string(IoError{.code = errno}) + "; the backup was kept and "
                                                              "nothing was deleted");
  }

  // The name is gone whatever the check below concludes, so that observation records
  // first, on every branch.
  observed_.record_absent(target.relative());

  // R6 in miniature: the tool's one job was making the name not exist. Confirm it.
  struct ::stat after {};
  if (::fstatat(parent->dir.get(), parent->name.c_str(), &after, AT_SYMLINK_NOFOLLOW) == 0) {
    return refuse(target, "unlinked, but the name is still present; the directory is in an "
                          "unexpected state");
  }
  if (const int e = errno; e != ENOENT) {
    return refuse(target, "unlinked, but confirming the name is gone failed: " +
                              to_string(IoError{.code = e}));
  }

  // What was removed, in the terms the model can use: the hash and size of the bytes now
  // held in the store. The store's own path is host layout and never rendered.
  ToolOutput out;
  out.rows.push_back({{
      {"path", target.relative().string()},
      {"hash", *hash},
      {"size", tuple_from(current->meta).size},
  }});
  return out;
}

}  // namespace hermit
