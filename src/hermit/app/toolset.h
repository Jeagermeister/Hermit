#pragma once

// Composition: the eight Tier 0 tools, and the state they share.
//
// ROUTING.md section 8 makes tool exposure a per-frontend policy rather than a fixed
// set, and section 9 makes the read cap and the backup directory configuration. Both
// decisions land here, in the layer that knows about machines and settings -- which is
// why this is in `app` and not in `core` beside the tools themselves.
//
// It exists as a type rather than a free function for one reason: lifetime. Tools hold
// `ObservedState&` and `BackupStore&` for their whole life (backup.h says so outright),
// so something has to own those for at least as long as the registry. Bundling them
// makes the ownership visible instead of leaving it as a rule a caller must remember.
//
// The two members are held behind `unique_ptr` so their addresses survive a move of the
// ToolSet itself. Holding them by value would compile and then dangle: moving the
// ToolSet would relocate the state while the tools inside the registry went on
// referring to where it used to be.
//
// `shell` is conditional, not unconditional like the other eight: pass `shell` to
// tier0() and it is registered ninth. The gate this was once described as waiting on
// was broader than it turned out to be -- DECISIONS.md's own D7 text says the
// `openat(O_NOFOLLOW)` component walk (ROUTING.md section 12 step 5's second condition)
// gates *the programmatic frontend* specifically (`mcp.cpp`, step 6, still unbuilt),
// not this registry. D10's kernel confinement (step 5's first condition) is the half
// that actually applies here, and it is done. What still gates registration is narrower
// and lives at the call site, not in this file: an explicit config flag, plus a live
// probe_confinement() reporting Enforced, checked once at startup (ROUTING.md section 8:
// "gate on the probe, never on the platform").

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>

#include <hermit/core/backup.h>
#include <hermit/core/fsio.h>
#include <hermit/core/observed.h>
#include <hermit/core/tool.h>

namespace hermit::app {

/// What `shell` needs beyond what the other eight tools share. `root` is
/// confine::run_confined's writable grant -- the sandbox root itself, not a per-call
/// resolved path, since shell has no Path argument to resolve one from. A plain path
/// rather than a `const Sandbox&`: unlike ObservedState/BackupStore this value never
/// changes after composition, so storing a reference would add a lifetime coupling nothing
/// else here needs.
struct ShellOptions {
  std::filesystem::path root;
  std::chrono::milliseconds timeout;
};

class ToolSet {
 public:
  /// The eight Tier 0 tools: `read`, `hash`, `list`, `find`, `grep`, `write`, `edit`,
  /// `move`, registered in that order -- observe before mutate, which is also the order
  /// ROUTING.md section 4's table reads in. `shell`, ninth and last when `shell` is
  /// supplied, so the first eight's prompt bytes stay exactly what they were before this
  /// parameter existed.
  ///
  /// Order is part of the contract, not an accident: it fixes the byte order of the tool
  /// definitions in every prompt, and a moving tool list moves the prompt bytes and with
  /// them the server's prompt-cache hit.
  ///
  /// `backup_dir` must be **outside** the sandbox root (R4, settled 2026-08-16): the
  /// model may never list, read, edit or move its own undo data. That is this layer's
  /// contract to honour, because `BackupStore` cannot know where the root is.
  ///
  /// Fails only if registration does -- a duplicate name or a malformed spec, both of
  /// which are wiring bugs and are meant to surface at composition time rather than
  /// mid-session.
  [[nodiscard]] static std::expected<ToolSet, RegistryError> tier0(
      std::filesystem::path backup_dir, std::optional<ShellOptions> shell = std::nullopt,
      std::uint64_t max_read_bytes = kDefaultMaxReadBytes);

  [[nodiscard]] ToolRegistry& registry() noexcept { return *registry_; }
  [[nodiscard]] const ToolRegistry& registry() const noexcept { return *registry_; }

  /// The observed-state map the tools share.
  ///
  /// Shared on purpose: `edit`'s staleness gate refuses a target whose identity tuple
  /// has changed since it was observed, and the observation it checks against is
  /// whatever `read` recorded. Two maps would make that gate always-stale and therefore
  /// useless.
  [[nodiscard]] ObservedState& observed() noexcept { return *observed_; }

  /// Where undo data goes. Exposed for the supervisor's side of R4 (enumeration and
  /// restore are Phase 3); nothing model-facing may ever see one of these paths.
  [[nodiscard]] const BackupStore& backups() const noexcept { return *backups_; }

 private:
  ToolSet(std::unique_ptr<ObservedState> observed, std::unique_ptr<BackupStore> backups,
          std::unique_ptr<ToolRegistry> registry) noexcept;

  std::unique_ptr<ObservedState> observed_;
  std::unique_ptr<BackupStore> backups_;
  std::unique_ptr<ToolRegistry> registry_;
};

}  // namespace hermit::app
