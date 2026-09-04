#include <hermit/app/toolset.h>

#include <utility>

#include <hermit/core/tools/delete.h>
#include <hermit/core/tools/edit.h>
#include <hermit/core/tools/find.h>
#include <hermit/core/tools/grep.h>
#include <hermit/core/tools/hash.h>
#include <hermit/core/tools/list.h>
#include <hermit/core/tools/move.h>
#include <hermit/core/tools/read.h>
#include <hermit/core/tools/shell.h>
#include <hermit/core/tools/write.h>

namespace hermit::app {

std::string to_string(const BackupDirError& error) {
  return error.store.string() + " is inside " + error.root.string();
}

std::expected<std::filesystem::path, BackupDirError> resolve_backup_dir(
    const Sandbox& box, std::optional<std::filesystem::path> backup_dir) {
  if (!backup_dir) {
    backup_dir =
        box.root().parent_path() / (".hermit-backups-" + box.root().filename().string());
  }

  // Cheap containment check on the *lexical* paths, which is enough for an operator
  // typo. It is not a security boundary and is not claimed as one: both paths are
  // host-side operator configuration, not model input.
  //
  // Compared with a separator appended, not as a bare prefix. A bare prefix test
  // makes `/tmp/sandbox-backups` look "inside" `/tmp/sandbox`, which would refuse the
  // most natural place to put the store -- right beside the root, which is where this
  // command's own default puts it.
  constexpr char kSep = std::filesystem::path::preferred_separator;
  std::string root = box.root().lexically_normal().string();
  while (root.size() > 1 && root.back() == kSep) root.pop_back();
  // weakly_canonical, not absolute: Sandbox::open canonicalises the root, expanding
  // every symlink, and sandbox.cpp says outright that "the two must be expressed in the
  // same terms or containment comparisons silently fail open". absolute() resolves no
  // symlinks, so `--root ~/work --backups ~/work/undo` with ~/work a symlink would
  // compare a canonical root against an unexpanded store, call the store outside, and
  // put the undo data inside the sandbox where the model can list, read, edit and move
  // it -- R4 defeated by the check meant to enforce it.
  // weakly_canonical tolerates a store that does not exist yet, which is the normal
  // case: BackupStore creates it lazily on the first mutation.
  const std::string store =
      std::filesystem::weakly_canonical(*backup_dir).lexically_normal().string();

  // `--root /` needs its own arm, and getting it wrong is worse than it looks: the strip
  // loop leaves root as "/", so the general test degrades to `starts_with("//")`, which
  // no normalised path matches -- every location would have been accepted as "outside"
  // the one root that contains everything.
  const bool inside = (root == std::string{kSep}) ? store.starts_with(kSep)
                                                   : (store == root || store.starts_with(root + kSep));
  if (inside) {
    return std::unexpected(
        BackupDirError{.root = std::filesystem::path{root}, .store = std::filesystem::path{store}});
  }
  return *backup_dir;
}

ToolSet::ToolSet(std::unique_ptr<ObservedState> observed,
                 std::unique_ptr<BackupStore> backups,
                 std::unique_ptr<ToolRegistry> registry) noexcept
    : observed_(std::move(observed)),
      backups_(std::move(backups)),
      registry_(std::move(registry)) {}

std::expected<ToolSet, RegistryError> ToolSet::tier0(std::filesystem::path backup_dir,
                                                     std::optional<ShellOptions> shell,
                                                     std::uint64_t max_read_bytes,
                                                     bool with_delete) {
  auto observed = std::make_unique<ObservedState>();
  auto backups = std::make_unique<BackupStore>(std::move(backup_dir));
  auto registry = std::make_unique<ToolRegistry>();

  // Observe first, then mutate -- ROUTING.md section 4's own order.
  const auto add = [&](std::unique_ptr<Tool> tool) -> std::expected<void, RegistryError> {
    return registry->add(std::move(tool));
  };

  if (auto ok = add(std::make_unique<ReadTool>(*observed, max_read_bytes)); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = add(std::make_unique<HashTool>()); !ok) return std::unexpected(ok.error());
  if (auto ok = add(std::make_unique<ListTool>(*observed)); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = add(std::make_unique<FindTool>()); !ok) return std::unexpected(ok.error());
  if (auto ok = add(std::make_unique<GrepTool>(max_read_bytes)); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = add(std::make_unique<WriteTool>(*observed, *backups)); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = add(std::make_unique<EditTool>(*observed, *backups, max_read_bytes)); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = add(std::make_unique<MoveTool>(*observed)); !ok) {
    return std::unexpected(ok.error());
  }

  // Only when the caller asks, and appended rather than interleaved, so the first
  // eight's prompt bytes -- and with them the server's prompt-cache hit -- stay exactly
  // what they were before either parameter existed. `delete` first (D19): its gate is the
  // flag alone, its guard is the observation table and the store it shares with write and
  // edit. Then `shell`, last: the caller (main.cpp, mcp.cpp) is where its actual gate
  // lives -- an explicit config flag plus a live probe_confinement() reporting Enforced,
  // checked once at startup.
  if (with_delete) {
    if (auto ok = add(std::make_unique<DeleteTool>(*observed, *backups)); !ok) {
      return std::unexpected(ok.error());
    }
  }
  if (shell) {
    if (auto ok = add(std::make_unique<ShellTool>(shell->root, shell->timeout, max_read_bytes));
        !ok) {
      return std::unexpected(ok.error());
    }
  }

  return ToolSet{std::move(observed), std::move(backups), std::move(registry)};
}

}  // namespace hermit::app
