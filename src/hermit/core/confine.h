#pragma once

// D10 --- Kernel confinement for shell: Landlock, vendored, one writable root.
//
// The mechanism half of D10 (DECISIONS.md). Shell is the one tool that cannot be R1-correct by
// construction -- a command string has no path argument for Sandbox::resolve to contain -- so
// containment moves from the type system to the kernel: a Landlock ruleset, installed in a
// forked child immediately before it execs the command, denies everything outside one writable
// root. The parent never restricts itself.
//
// Placed beside sandbox.h, same precedent D11 sets for exactly this kind of code: no model, no
// network, Tier 0, hermit_core.
//
// Why fork is safe here despite D10 rejecting self-restriction for the supervisor: D10 rejects
// restricting the supervisor process itself because landlock_restrict_self restricts the
// calling THREAD, not the process -- a threaded HTTP resolver would stay unconfined. That
// objection does not apply to a forked child: POSIX fork() clones only the calling thread, and
// D1 already commits Hermit to single-threaded, blocking execution, so the forked child is
// trivially single-threaded before it ever restricts itself. "Restricts the thread" and
// "restricts the child process" coincide exactly in that shape.
//
// The grant table (fixed, not tuned -- D10 measured each row by removing it):
//   --rw <root>       the work; the only writable directory
//   --ro /usr         ld.so + shared libs + locale; omit it and exec fails outright
//   --ro /etc         nsswitch.conf, passwd, gitconfig, and name resolution incl. localhost
//   --ro /proc        self-inspection
//   --rw /dev/null    denied unless granted explicitly
//   --ro /dev/urandom same

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace hermit {

enum class ConfineErrorKind {
  Fork,         // fork() itself failed
  InheritedFd,  // a pre-fork fd audit found a descriptor without O_CLOEXEC
  Wait,         // waitpid() failed unexpectedly
  ChildKilled,  // the confined child died by signal rather than exiting
  ProbeSetup,   // probe_confinement()'s own scaffolding failed before any fork -- creating its
                // scratch/outside directories, or the fd audit above surfacing under it
};

std::string_view to_string(ConfineErrorKind e) noexcept;

struct ConfineError {
  ConfineErrorKind kind;
  std::string detail;
};

std::string to_string(const ConfineError& e);

struct ConfineResult {
  /// The confined command's own exit status -- or, if the vendored launcher itself refused to
  /// start the command (an unopenable grant path, an unenforceable kernel), its fixed refusal
  /// code (125), indistinguishable at this layer from the command legitimately exiting 125.
  /// Callers that need to tell those apart use probe_confinement() first, which is exactly what
  /// ROUTING.md section 8 gates shell's MCP exposure on.
  int exit_code;
};

/// Run `command` confined to `root` under D10's fixed grant table. Forks; the child installs
/// the Landlock ruleset and execvp's `command`; the parent stays unrestricted and waits.
///
/// `root` is host/supervisor configuration -- like BackupStore's directory -- not a
/// model-supplied argument, so it is a plain std::filesystem::path rather than a SandboxPath:
/// SandboxPath's contract is specifically for per-call paths proven via Sandbox::resolve.
///
/// Audits /proc/self/fd for inheritable descriptors before forking (D10: "audit /proc/self/fd
/// before the first spawn rather than trusting that") and fails closed rather than forking at
/// all if one is found. `allowed` is forwarded to that audit verbatim -- see
/// assert_no_inheritable_fds. A caller launched under something that itself hands down an extra
/// fd Hermit did not open (a supervisor's log-capture pipe, a test harness) names it here rather
/// than this function guessing which inherited descriptors are its own responsibility.
[[nodiscard]] std::expected<ConfineResult, ConfineError> run_confined(
    const std::filesystem::path& root, std::span<const std::string> command,
    std::span<const int> allowed = {});

enum class ConfinementProbeResult {
  Enforced,   // a write attempted outside the grant was refused with EACCES
  Unenforced, // it was not -- Landlock is absent, disabled, or the setup itself failed
};

std::string_view to_string(ConfinementProbeResult r) noexcept;

/// D10's own probe, deliberately stronger than the vendored launcher's built-in `--probe`:
/// "Upstream's probe runs `true` and checks for exit 0, which establishes that the profile was
/// accepted -- not that a forbidden access is refused. Ours attempts a write outside the grant
/// and requires EACCES." This is what ROUTING.md section 8 gates shell's MCP exposure on.
///
/// Two states only, not three. fs_mask_for_abi in the vendored file includes the write-file bit
/// unconditionally at every Landlock ABI the kernel might negotiate down to, so once
/// landlock_restrict_self succeeds AT ALL, a denied write outside the grant always produces
/// EACCES -- there is no ABI level at which restriction "partially" succeeds but stops denying
/// writes. The only two things this test can honestly distinguish are: the child never got to
/// attempt the write (setup failed closed) vs. the write was correctly refused. Per D11's fail-
/// closed vocabulary, "could not determine it" reports as Unenforced, never as a silent pass.
///
/// Implementation note, settled while building this rather than assumed up front: the write
/// attempt runs as `/usr/bin/touch <path outside the grant>`, under the identical standard grant
/// table run_confined uses -- not a self-re-exec of the calling process. A self-exec design was
/// tried first and does not work: the calling process is not always the `hermit` binary (it is
/// `hermit_tests` under the test suite, and gtest's own stock main owns that process's argv
/// parsing), so "re-exec whatever is currently running with a hidden internal flag" is correct
/// in production and silently wrong under test. `touch` needs no extra grant beyond `--ro /usr`
/// (already fixed and unconditional -- D10: omit it and exec fails outright), behaves
/// identically regardless of which binary called this function, and its exit status is enough:
/// under the standard grant table the ONLY thing that can make it fail against a target this
/// function creates its parent directory for is Landlock, since the parent directory itself is
/// created with ordinary, writable-by-this-user permissions.
///
/// `allowed` is forwarded to the pre-fork fd audit, same as run_confined's.
[[nodiscard]] std::expected<ConfinementProbeResult, ConfineError> probe_confinement(
    std::span<const int> allowed = {});

/// D10: "Set O_CLOEXEC on everything the parent opens, and audit /proc/self/fd before the first
/// spawn rather than trusting that." Enumerates /proc/self/fd and fails closed on the first
/// descriptor found -- beyond stdin/stdout/stderr and whatever `allowed` names -- that lacks
/// FD_CLOEXEC: any fd the parent holds when it forks (the Ollama socket, a config file, a log)
/// is inherited by the confined child and is a hole no Landlock grant describes.
///
/// `allowed` lets a caller deliberately permit specific additional fds to cross the fork -- the
/// shell Tool's future R8 stdout/stderr capture pipes are the motivating case, and a process
/// launched under something that itself hands down an extra fd (a supervisor's log pipe, a test
/// harness) is another. It ADDS to {0, 1, 2}, which are always permitted regardless -- a caller
/// naming its own extra fd should never have to remember to re-list stdio to keep it covered.
[[nodiscard]] std::expected<void, ConfineError> assert_no_inheritable_fds(
    std::span<const int> allowed = {});

}  // namespace hermit
