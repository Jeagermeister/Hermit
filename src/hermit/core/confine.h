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

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
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
  CaptureSetup, // ConfineLimits requested capture and pipe2() itself failed before any fork
};

std::string_view to_string(ConfineErrorKind e) noexcept;

struct ConfineError {
  ConfineErrorKind kind;
  std::string detail;
};

std::string to_string(const ConfineError& e);

/// One captured stream (R8/shell's stdout or stderr). `truncated` is set the instant more than
/// the cap arrived -- this is shell's own, deliberate exception to fsio.h's "never a truncated
/// read" rule (ROUTING.md section 3): by the time output exceeds the cap the command has already
/// run and had its side effects, so refusing outright (fsio.h's convention for an oversized file)
/// would hide from the model that a real command executed -- worse than the read/grep case,
/// where refusing costs nothing because the file was never touched.
struct CapturedStream {
  std::string bytes;
  bool truncated = false;
};

struct ConfineResult {
  /// The confined command's own exit status -- or, if the vendored launcher itself refused to
  /// start the command (an unopenable grant path, an unenforceable kernel), its fixed refusal
  /// code (125), indistinguishable at this layer from the command legitimately exiting 125.
  /// Callers that need to tell those apart use probe_confinement() first, which is exactly what
  /// ROUTING.md section 8 gates shell's MCP exposure on. Same ambiguity, same reason, at 126:
  /// run_confined's own chdir(root) or dup2 (when capture is requested) failing before the
  /// launcher ever runs -- should not happen in practice, since `root` is the grant's own
  /// writable directory and the pipe fds are freshly created moments earlier, but is reported
  /// this way rather than silently falling through to run the command unconfined or uncaptured.
  ///
  /// On a 125 or 126 exit specifically, stderr_capture (below) may also contain the vendored
  /// launcher's own diagnostic text rather than -- or in addition to, since it never ran --
  /// the command's: the launcher's fprintf(stderr, ...) calls that precede a refusal share the
  /// same fd this layer has already redirected to the capture pipe by the time any of them can
  /// run. Not fixable without hand-editing the vendored file, which D3 forbids by design (pinned
  /// and sha256-checked). The one case reachable on a SUCCESSFUL exit -- a fixed "partial
  /// enforcement" notice on pre-6.12 kernels -- is stripped before this is returned, precisely
  /// because it is not a refusal and this ambiguity was never meant to cover it.
  ///
  /// Meaningless when timed_out is true: describes the forced death (see run_confined), not the
  /// command's own logic.
  int exit_code;

  /// Set only when this call's own ConfineLimits::timeout fired and the confined process group
  /// was killed as a result. R8: "a timeout treated as a failure rather than as missing data" --
  /// callers check this before exit_code. Never set for a kill this call did not itself issue
  /// (an external signal, the OOM killer): that stays ConfineErrorKind::ChildKilled, unchanged.
  bool timed_out = false;

  /// Populated iff the matching ConfineLimits::stdout_cap / stderr_cap was set; empty and
  /// untruncated (meaningless, not "confirmed empty") otherwise -- a caller that asked for no
  /// capture must not read these as if it had.
  CapturedStream stdout_capture;
  CapturedStream stderr_capture;
};

/// R8's per-call wall-clock bound and the two streams' capture request, both optional so a
/// default-constructed instance is exactly today's unbounded, uncaptured behaviour.
struct ConfineLimits {
  /// nullopt = unbounded. ShellTool always supplies one; LoopOptions::budget
  /// (supervisor/loop.h) cannot substitute for this -- it bounds the session between turns, not
  /// a single blocking call already in flight.
  std::optional<std::chrono::milliseconds> timeout;

  /// nullopt = do not capture this stream; it stays inherited from the parent exactly as today.
  /// A value is the byte cap CapturedStream::truncated is measured against.
  std::optional<std::uint64_t> stdout_cap;
  std::optional<std::uint64_t> stderr_cap;
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
///
/// `limits` is checked once, up front: a default-constructed ConfineLimits{} takes the original,
/// unbounded code path exactly as before this parameter existed -- probe_confinement() relies on
/// this to stay untouched. A non-default `limits` runs the confined child under its own process
/// group (setpgid, both sides, race-free) so a timeout's killpg reaches everything the command
/// itself backgrounds, not just its own immediate pid -- the vendored launcher does not session-
/// lead or otherwise group itself, verified by reading third_party/landlock-run/main.c.
[[nodiscard]] std::expected<ConfineResult, ConfineError> run_confined(
    const std::filesystem::path& root, std::span<const std::string> command,
    std::span<const int> allowed = {}, const ConfineLimits& limits = {});

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
/// `allowed` lets a caller deliberately permit specific additional fds to cross the fork -- a
/// process launched under something that itself hands down an extra fd Hermit did not open (a
/// supervisor's log pipe, a test harness) is the motivating case. It ADDS to {0, 1, 2}, which are
/// always permitted regardless -- a caller naming its own extra fd should never have to remember
/// to re-list stdio to keep it covered.
///
/// NOT the mechanism for R8's stdout/stderr capture pipes: run_confined creates those itself,
/// after this audit has already run (see run_confined's ConfineLimits), so they never exist at
/// the point this function inspects /proc/self/fd and never need naming here.
[[nodiscard]] std::expected<void, ConfineError> assert_no_inheritable_fds(
    std::span<const int> allowed = {});

}  // namespace hermit
