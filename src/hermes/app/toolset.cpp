#include <hermes/app/toolset.h>

#include <utility>

#include <hermes/core/tools/edit.h>
#include <hermes/core/tools/find.h>
#include <hermes/core/tools/grep.h>
#include <hermes/core/tools/hash.h>
#include <hermes/core/tools/list.h>
#include <hermes/core/tools/move.h>
#include <hermes/core/tools/read.h>
#include <hermes/core/tools/write.h>

namespace hermes::app {

ToolSet::ToolSet(std::unique_ptr<ObservedState> observed,
                 std::unique_ptr<BackupStore> backups,
                 std::unique_ptr<ToolRegistry> registry) noexcept
    : observed_(std::move(observed)),
      backups_(std::move(backups)),
      registry_(std::move(registry)) {}

std::expected<ToolSet, RegistryError> ToolSet::tier0(std::filesystem::path backup_dir,
                                                     std::uint64_t max_read_bytes) {
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

  return ToolSet{std::move(observed), std::move(backups), std::move(registry)};
}

}  // namespace hermes::app
