#include <hermit/core/confine.h>

#include "confine_internal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hermit {

std::string_view to_string(ConfineErrorKind e) noexcept {
  switch (e) {
    case ConfineErrorKind::Fork: return "fork";
    case ConfineErrorKind::InheritedFd: return "inherited-fd";
    case ConfineErrorKind::Wait: return "wait";
    case ConfineErrorKind::ChildKilled: return "child-killed";
    case ConfineErrorKind::ProbeSetup: return "probe-setup";
    case ConfineErrorKind::CaptureSetup: return "capture-setup";
  }
  return "unknown";
}

std::string to_string(const ConfineError& e) {
  return std::string(to_string(e.kind)) + ": " + e.detail;
}

std::string_view to_string(ConfinementProbeResult r) noexcept {
  switch (r) {
    case ConfinementProbeResult::Enforced: return "enforced";
    case ConfinementProbeResult::Unenforced: return "unenforced";
  }
  return "unknown";
}

namespace {

// Builds the argv landlock_run_cli_main expects. argv[0] is a placeholder program name --
// parse() inside the vendored file starts reading at index 1, matching real process argv -- and
// `finish()` appends the NUL sentinel at argv[argc] that a real process's argv gets from the OS
// for free but a hand-built array does not (confine_internal.h documents why this matters).
//
// `push` must not be interleaved with reading `.argv` (i.e. call finish() exactly once, after
// every push): std::string's SSO buffer lives inside the vector<string> element, and a
// reallocation of `owned` while it grows moves those elements, invalidating any .data() taken
// beforehand. finish() is the only place .data() is taken, and only after all pushes are done,
// so `owned` never reallocates again while `argv` is in use.
class CommandLine {
 public:
  void push(std::string_view s) { owned_.emplace_back(s); }

  void finish() {
    argv_.reserve(owned_.size() + 1);
    for (auto& s : owned_) argv_.push_back(s.data());
    argv_.push_back(nullptr);
  }

  [[nodiscard]] int argc() const noexcept { return static_cast<int>(owned_.size()); }
  // Non-const: landlock_run_cli_main's C signature is `char**`, not const-correct by nature
  // (it is the vendored file's own `main`), so the pointer this hands out cannot be `char* const*`.
  [[nodiscard]] char** argv() noexcept { return argv_.data(); }

 private:
  std::vector<std::string> owned_;
  std::vector<char*> argv_;
};

// D10's grant table, verbatim -- not tuned, each row measured by removing it. `root` is the
// only writable directory.
void push_system_grants(CommandLine& cl, const std::filesystem::path& root) {
  cl.push("landlock-run");  // argv[0]; parse() skips it, same as a real process's own name
  cl.push("--rw");
  cl.push(root.string());
  cl.push("--ro");
  cl.push("/usr");
  cl.push("--ro");
  cl.push("/etc");
  cl.push("--ro");
  cl.push("/proc");
  cl.push("--rw");
  cl.push("/dev/null");
  cl.push("--ro");
  cl.push("/dev/urandom");
}

// mkdtemp, not a getpid()-based name: /tmp is world-writable, and a predictable path there is
// exactly the classic local pre-plant race mkdtemp's random suffix exists to close -- a
// getpid()-named directory can be pre-created as a symlink by anything else with write access to
// /tmp before this call ever runs, which would hand the Landlock --rw grant this function relies
// on to wherever that symlink points instead of a fresh, exclusively-owned directory.
std::expected<std::filesystem::path, ConfineError> make_scratch_dir(std::string_view tag) {
  std::filesystem::path templ =
      std::filesystem::temp_directory_path() / (std::string("hermit-confine-") + std::string(tag) + "-XXXXXX");
  std::string buf = templ.string();
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) {
    return std::unexpected(
        ConfineError{ConfineErrorKind::ProbeSetup, std::string("mkdtemp: ") + std::strerror(errno)});
  }
  return std::filesystem::path(buf.c_str());
}

// Runs `cl` in a forked child that installs the ruleset and execvp's, per D10: "installed in a
// forked child immediately before execvp; the parent stays unrestricted." The child never
// returns to the caller -- landlock_run_cli_main either execvp's (image replaced) or returns its
// fixed refusal code (125), and confine_internal.h's contract requires _exit() immediately
// either way, since the vendored parser's calloc'd buffers are never freed on the assumption the
// process is about to end.
//
// `root` sets the child's working directory before any of that: the grant table makes `root` the
// one writable directory, and a command built from relative paths -- the natural way ShellTool's
// caller writes one, matching how every other tool resolves relative paths against the sandbox
// root -- needs a matching cwd or `write file.txt` lands wherever this process happened to be
// launched from instead, which is not even guaranteed to be inside the grant at all. A chdir
// failure here is as fail-closed as it gets: exit 126, never falling through to run the command
// from an unconfined-relative cwd. (126, not 125, so it stays distinguishable from the vendored
// launcher's own refusal code at the one layer that can tell them apart -- a human reading the
// number, since ConfineResult's exit_code cannot.)
std::expected<int, ConfineError> fork_exec_wait(const std::filesystem::path& root,
                                                CommandLine& cl) {
  pid_t pid = ::fork();
  if (pid < 0) {
    return std::unexpected(ConfineError{ConfineErrorKind::Fork, std::strerror(errno)});
  }
  if (pid == 0) {
    if (::chdir(root.c_str()) != 0) _exit(126);
    _exit(landlock_run_cli_main(cl.argc(), cl.argv()));
  }

  int status = 0;
  int wait_rc = 0;
  // EINTR is retried, not treated as failure: a stray signal delivered while blocked here must
  // not be mistaken for the child's own outcome. Without the retry, EINTR would both misreport a
  // healthy child as a Wait error AND leak it as a zombie, since it was never actually reaped.
  //
  // This function stays unbounded and signal-agnostic on purpose -- probe_confinement() calls it
  // directly and must keep working exactly as before. R8's wall-clock bound
  // (fork_exec_wait_bounded, below) turned out not to need a signal-based interrupt of a blocking
  // waitpid at all: it polls WNOHANG against a deadline instead, which composes with capturing
  // stdout/stderr in the same loop without a signal handler anywhere in this file.
  do {
    wait_rc = ::waitpid(pid, &status, 0);
  } while (wait_rc < 0 && errno == EINTR);
  if (wait_rc < 0) {
    return std::unexpected(ConfineError{ConfineErrorKind::Wait, std::strerror(errno)});
  }
  if (WIFSIGNALED(status)) {
    return std::unexpected(ConfineError{
        ConfineErrorKind::ChildKilled,
        "confined child killed by signal " + std::to_string(WTERMSIG(status))});
  }
  return WEXITSTATUS(status);
}

// --- R8: the bounded, capturing sibling -----------------------------------------------------
//
// Used only when a ConfineLimits actually requests a timeout or a capture; run_confined() takes
// the plain fork_exec_wait() path above otherwise, so probe_confinement() needed no changes here.

constexpr auto kPollSlice = std::chrono::milliseconds{200};
constexpr auto kKillGrace = std::chrono::milliseconds{2000};
using Clock = std::chrono::steady_clock;

struct CaptureFd {
  int read_fd = -1;
  int write_fd = -1;
};

std::expected<CaptureFd, ConfineError> make_capture_pipe() {
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    return std::unexpected(
        ConfineError{ConfineErrorKind::CaptureSetup, std::string("pipe2: ") + std::strerror(errno)});
  }
  return CaptureFd{.read_fd = fds[0], .write_fd = fds[1]};
}

void close_if_open(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

// One requested stream's read side, tracked through the poll loop: where captured bytes land,
// the cap they truncate at, and whether this fd has stopped producing data. `done` defaults true
// so a stream nobody asked to capture is inert everywhere below without an extra branch at every
// call site.
struct StreamState {
  int fd = -1;
  CapturedStream* out = nullptr;
  std::uint64_t cap = 0;
  bool done = true;
};

// Reads everything currently available on `s` without blocking, appending up to `s.cap` bytes
// into `s.out->bytes` and setting `s.out->truncated` the instant a read carries the buffer past
// the cap. Deliberately keeps draining past the cap rather than stopping there: once this side
// stops reading, a full kernel pipe buffer (~64 KiB) blocks the child's own write() forever,
// which would turn "output capped" into "process hung" -- the exact failure R8 exists to
// prevent, self-inflicted this time. Marks `s.done` and closes the fd on a genuine EOF or an
// unexpected read() error; leaves it open and returns on EAGAIN, meaning "nothing more right
// now, try again next slice."
void drain_available(StreamState& s) {
  if (s.done) return;
  std::array<char, 4096> buf{};
  for (;;) {
    const ssize_t n = ::read(s.fd, buf.data(), buf.size());
    if (n > 0) {
      auto& bytes = s.out->bytes;
      const auto len = static_cast<std::size_t>(n);
      const std::size_t room = bytes.size() < s.cap ? s.cap - bytes.size() : 0;
      const std::size_t take = len < room ? len : room;
      if (take > 0) bytes.append(buf.data(), take);
      if (take < len) s.out->truncated = true;
      continue;
    }
    if (n == 0) {
      close_if_open(s.fd);
      s.done = true;
      return;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    close_if_open(s.fd);  // an unexpected read() error: stop trying this fd, keep what we have
    s.done = true;
    return;
  }
}

// The vendored launcher writes this exact line to the CHILD's stderr, unconditionally, on every
// successful call where the kernel negotiates a Landlock ABI below 5 (main.c: `restrict_self`
// sets `partial` whenever `abi < MAX_ABI`, true for every kernel from 5.13 through 6.11 -- most
// currently-deployed LTS/enterprise kernels sit in that range). It is written before execvp, on
// the same fd this function has already dup2'd onto the capture pipe, so without this it lands
// in `stderr_capture` indistinguishable from the confined command's own output -- not a rare
// edge case but the common one, and a direct violation of ROUTING.md section 5's "content
// travels exact" on a codebase whose read/grep/write tools all keep that promise.
//
// Matching this exact string is safe rather than fragile specifically because D3 pins the
// vendored file by sha256 at configure time (CMakeLists: FATAL_ERROR on any hand-edit) -- the
// string cannot silently drift out from under this check the way matching a third party's
// unpinned CLI output normally would.
constexpr std::string_view kPartialEnforcementNotice =
    "landlock-run: partial enforcement (older Landlock ABI)\n";

void strip_partial_enforcement_notice(CapturedStream& stderr_capture) {
  if (stderr_capture.bytes.starts_with(kPartialEnforcementNotice)) {
    stderr_capture.bytes.erase(0, kPartialEnforcementNotice.size());
  }
  // A cap smaller than the notice itself (default 16 MiB; this is a 53-byte string) could leave
  // a truncated prefix that fails this exact match and leaks a few bytes of launcher noise
  // through. Not handled -- named, matching this file's habit of stating what a mechanism does
  // not close rather than silently overclaiming coverage.
}

std::expected<ConfineResult, ConfineError> fork_exec_wait_bounded(const std::filesystem::path& root,
                                                                   CommandLine& cl,
                                                                   const ConfineLimits& limits) {
  std::optional<CaptureFd> out_pipe, err_pipe;
  if (limits.stdout_cap) {
    auto p = make_capture_pipe();
    if (!p) return std::unexpected(p.error());
    out_pipe = *p;
  }
  if (limits.stderr_cap) {
    auto p = make_capture_pipe();
    if (!p) {
      if (out_pipe) {
        close_if_open(out_pipe->read_fd);
        close_if_open(out_pipe->write_fd);
      }
      return std::unexpected(p.error());
    }
    err_pipe = *p;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    ConfineError err{ConfineErrorKind::Fork, std::strerror(errno)};
    if (out_pipe) {
      close_if_open(out_pipe->read_fd);
      close_if_open(out_pipe->write_fd);
    }
    if (err_pipe) {
      close_if_open(err_pipe->read_fd);
      close_if_open(err_pipe->write_fd);
    }
    return std::unexpected(err);
  }
  if (pid == 0) {
    // Its own process group, joined before anything else: a timeout's killpg (parent side, below)
    // must reach every process this command backgrounds, not just this immediate pid -- the
    // vendored launcher does not setsid/setpgid itself (verified by reading main.c), so nothing
    // else will.
    ::setpgid(0, 0);
    // Same fail-closed chdir as the plain fork_exec_wait, and the same reason: `root` is the
    // grant's one writable directory, and a relative path in the command needs a matching cwd.
    // Exit 126 here, same accepted ambiguity fork_exec_wait's own chdir failure documents.
    if (::chdir(root.c_str()) != 0) _exit(126);
    // dup2 duplicates onto a fresh descriptor with no CLOEXEC of its own, so the redirection
    // survives the execvp landlock_run_cli_main performs internally regardless of the original
    // pipe fd's own O_CLOEXEC; Landlock hooks opens, not dup, so doing this before the ruleset
    // install (inside landlock_run_cli_main, below) is not a correctness question either way.
    //
    // Checked and fail-closed, same as chdir above: an unchecked dup2 that happened to fail
    // would fall through to exec with that stream still pointed at whatever fd 1/2 this process
    // inherited, and the caller would see an empty, unmarked capture -- "the command produced
    // nothing" -- instead of a setup failure. Same 126, same accepted ambiguity against a
    // legitimate exit 126 from the command itself.
    if (out_pipe) {
      if (::dup2(out_pipe->write_fd, STDOUT_FILENO) < 0) _exit(126);
      ::close(out_pipe->write_fd);
      ::close(out_pipe->read_fd);
    }
    if (err_pipe) {
      if (::dup2(err_pipe->write_fd, STDERR_FILENO) < 0) _exit(126);
      ::close(err_pipe->write_fd);
      ::close(err_pipe->read_fd);
    }
    _exit(landlock_run_cli_main(cl.argc(), cl.argv()));
  }

  // Parent, from here on. setpgid on both sides of the fork, same idiom Stevens' APUE recommends
  // for exactly this race: whichever side loses it, the group is set before this function can
  // possibly reach its own killpg below.
  ::setpgid(pid, pid);

  ConfineResult result{};
  StreamState out_state, err_state;
  if (out_pipe) {
    // Load-bearing, not cleanup: a pipe reaches EOF only once EVERY open copy of its write end is
    // closed, including this one.
    close_if_open(out_pipe->write_fd);
    ::fcntl(out_pipe->read_fd, F_SETFL, O_NONBLOCK);
    out_state = StreamState{
        .fd = out_pipe->read_fd, .out = &result.stdout_capture, .cap = *limits.stdout_cap, .done = false};
  }
  if (err_pipe) {
    close_if_open(err_pipe->write_fd);
    ::fcntl(err_pipe->read_fd, F_SETFL, O_NONBLOCK);
    err_state = StreamState{
        .fd = err_pipe->read_fd, .out = &result.stderr_capture, .cap = *limits.stderr_cap, .done = false};
  }

  const std::optional<Clock::time_point> deadline =
      limits.timeout ? std::optional(Clock::now() + *limits.timeout) : std::nullopt;

  // One poll+drain slice: waits at most `slice` for either stream to have something ready (or,
  // once both are done, just sleeps the slice via poll(nullptr, 0, ...) so the outer loop below
  // does not spin), then drains whatever arrived. The slice, not just an overall deadline, is
  // what makes the loop correct: it is what lets the outer loop re-check waitpid(WNOHANG)
  // periodically instead of blocking on a pipe that a backgrounded grandchild might hold open
  // long after the command this call actually ran has exited.
  const auto poll_and_drain = [&](std::chrono::milliseconds slice) {
    std::vector<::pollfd> fds;
    if (!out_state.done) fds.push_back({.fd = out_state.fd, .events = POLLIN, .revents = 0});
    if (!err_state.done) fds.push_back({.fd = err_state.fd, .events = POLLIN, .revents = 0});
    if (!fds.empty()) {
      if (::poll(fds.data(), fds.size(), static_cast<int>(slice.count())) > 0) {
        drain_available(out_state);
        drain_available(err_state);
      }
    } else {
      ::poll(nullptr, 0, static_cast<int>(slice.count()));
    }
  };

  bool reaped = false;
  bool timed_out = false;
  int status = 0;

  for (;;) {
    poll_and_drain(kPollSlice);

    const pid_t w = ::waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      reaped = true;
      break;
    }
    if (w < 0 && errno != EINTR) {
      // Should not happen under D1's single-threaded model -- this process is the only waiter
      // on a pid it just forked -- but abandoning the child here without at least trying to kill
      // it would leave a live, still-confined-but-now-untracked process (and everything it
      // backgrounds) running with no supervisor watching it at all, which is a worse failure
      // than the Wait error being returned. Best-effort: the return value is not checked, since
      // there is no more useful action to take if even this fails.
      const int saved_errno = errno;
      ::killpg(pid, SIGKILL);
      pid_t reap;
      do {
        reap = ::waitpid(pid, &status, 0);
      } while (reap < 0 && errno == EINTR);
      close_if_open(out_state.fd);
      close_if_open(err_state.fd);
      return std::unexpected(ConfineError{ConfineErrorKind::Wait, std::strerror(saved_errno)});
    }
    if (deadline && Clock::now() >= *deadline) {
      timed_out = true;
      break;
    }
  }

  if (timed_out) {
    ::killpg(pid, SIGTERM);
    const Clock::time_point grace_deadline = Clock::now() + kKillGrace;
    while (!reaped) {
      poll_and_drain(kPollSlice);
      const pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        reaped = true;
        break;
      }
      if (Clock::now() >= grace_deadline) break;
    }
    if (!reaped) {
      ::killpg(pid, SIGKILL);  // cannot be caught, blocked or ignored -- bounded in every case
                               // except a process wedged in uninterruptible kernel sleep (D-state),
                               // named here rather than assumed away, matching D10's own habit of
                               // stating what a mechanism does not close
      pid_t w;
      do {
        w = ::waitpid(pid, &status, 0);
      } while (w < 0 && errno == EINTR);
      reaped = (w == pid);
    }
  }

  // One last non-blocking pass either way: a handful of bytes can sit in a pipe buffer, written
  // just before the process's own exit or just before SIGKILL landed, and never yet polled. A
  // lingering grandchild still holding a stream open past this point (the accepted gap -- see
  // confine.h's ConfineLimits doc) reads as EAGAIN here, not EOF, and is simply not waited on any
  // further: the immediate child's own reap is what this function treats as "the command is done."
  drain_available(out_state);
  drain_available(err_state);
  close_if_open(out_state.fd);
  close_if_open(err_state.fd);
  if (err_pipe) strip_partial_enforcement_notice(result.stderr_capture);

  if (!reaped) {
    return std::unexpected(ConfineError{
        ConfineErrorKind::Wait, "child did not become reapable even after SIGKILL"});
  }

  result.timed_out = timed_out;
  if (WIFSIGNALED(status)) {
    if (!timed_out) {
      return std::unexpected(ConfineError{
          ConfineErrorKind::ChildKilled,
          "confined child killed by signal " + std::to_string(WTERMSIG(status))});
    }
    // Killed by this call's own timeout: not ChildKilled (that kind means an external actor), and
    // not a real exit status either -- 128+signal is the shell convention for reporting one,
    // which is the closest thing to a meaningful number here. ConfineResult::timed_out is what
    // callers actually branch on; this is just what accompanies it.
    result.exit_code = 128 + WTERMSIG(status);
  } else {
    result.exit_code = WEXITSTATUS(status);
  }
  return result;
}

}  // namespace

std::expected<ConfineResult, ConfineError> run_confined(
    const std::filesystem::path& root, std::span<const std::string> command,
    std::span<const int> allowed, const ConfineLimits& limits) {
  if (auto fds = assert_no_inheritable_fds(allowed); !fds) {
    return std::unexpected(fds.error());
  }

  CommandLine cl;
  push_system_grants(cl, root);
  cl.push("--");
  for (const auto& arg : command) cl.push(arg);
  cl.finish();

  // A default-constructed ConfineLimits takes the original, unbounded path exactly as before
  // this parameter existed -- probe_confinement() relies on this below.
  if (limits.timeout || limits.stdout_cap || limits.stderr_cap) {
    return fork_exec_wait_bounded(root, cl, limits);
  }

  auto exit_code = fork_exec_wait(root, cl);
  if (!exit_code) return std::unexpected(exit_code.error());
  return ConfineResult{*exit_code};
}

std::expected<ConfinementProbeResult, ConfineError> probe_confinement(
    std::span<const int> allowed) {
  namespace fs = std::filesystem;

  // Two directories, both created by this (unrestricted) call before anything is confined:
  // `scratch` is granted --rw and is where a confined write is expected to succeed; `outside`
  // is NOT granted and is where a confined write is expected to be refused. `outside` must
  // exist and be ordinarily writable by this user *before* the child runs, or a plain ENOENT
  // (missing directory) would be indistinguishable from Landlock's EACCES -- the whole point is
  // to isolate Landlock as the only possible reason the write fails.
  auto scratch = make_scratch_dir("probe");
  if (!scratch) return std::unexpected(scratch.error());
  auto outside = make_scratch_dir("probe-outside");
  if (!outside) {
    std::error_code ec;
    fs::remove_all(*scratch, ec);
    return std::unexpected(outside.error());
  }
  fs::path outside_target = *outside / "attempt";

  if (auto fds = assert_no_inheritable_fds(allowed); !fds) {
    std::error_code ec;
    fs::remove_all(*scratch, ec);
    fs::remove_all(*outside, ec);
    return std::unexpected(fds.error());
  }

  // The write attempt is `/usr/bin/touch <outside_target>`, not a self-re-exec of the calling
  // process -- see confine.h's header comment on probe_confinement for why a self-exec design
  // does not work here (it is correct for `hermit` and silently wrong for `hermit_tests`).
  // `touch` needs no grant beyond the standard, already-unconditional `--ro /usr`.
  CommandLine cl;
  push_system_grants(cl, *scratch);
  cl.push("--");
  cl.push("/usr/bin/touch");
  cl.push(outside_target.string());
  cl.finish();

  auto exit_code = fork_exec_wait(*scratch, cl);
  // Checked BEFORE cleanup: touch returns the same generic exit code (1, on GNU coreutils) for
  // every failure reason, not specifically EACCES, so a non-0/125 exit alone cannot distinguish
  // "Landlock refused it" from "outside vanished out from under us for some other reason" (a
  // concurrent cleanup, another process sharing this /tmp). Confirming `outside` is still exactly
  // what this call created narrows that ambiguity about as far as an exit-code-only signal can.
  bool outside_survived = fs::is_directory(*outside);
  std::error_code ec;
  fs::remove_all(*scratch, ec);
  fs::remove_all(*outside, ec);
  if (!exit_code) return std::unexpected(exit_code.error());

  // exit 0: touch succeeded -- the write was NOT refused -- Unenforced.
  // exit 125: the vendored launcher's own fail-closed refusal (unopenable grant path, kernel
  //   reports Landlock unsupported) -- confinement was never established at all -- Unenforced,
  //   per D11's vocabulary: "I could not determine it" is a failure, not a pass.
  // anything else (touch's own EACCES failure; GNU coreutils touch exits 1): the write was
  //   refused, and per the header comment nothing but Landlock could have refused it here --
  //   Enforced, PROVIDED `outside` was still there to be refused against; if it was not, this is
  //   the same "could not determine it" case as an unenforceable kernel, not a measurement.
  if (*exit_code == 0 || *exit_code == 125 || !outside_survived) {
    return ConfinementProbeResult::Unenforced;
  }
  return ConfinementProbeResult::Enforced;
}

std::expected<void, ConfineError> assert_no_inheritable_fds(std::span<const int> allowed) {
  // {0, 1, 2} are always permitted, on top of whatever `allowed` adds -- never replaced by it.
  // A caller naming one extra fd (a capture pipe, a launcher's own inherited descriptor) should
  // never have to remember to re-list stdio to keep it covered; that footgun previously made a
  // caller who forgot silently lose the default protection instead of only widening it.
  std::vector<int> effective(allowed.begin(), allowed.end());
  effective.push_back(0);
  effective.push_back(1);
  effective.push_back(2);

  DIR* dir = ::opendir("/proc/self/fd");
  if (dir == nullptr) {
    return std::unexpected(ConfineError{ConfineErrorKind::ProbeSetup,
                                        std::string("opendir(/proc/self/fd): ") + std::strerror(errno)});
  }
  const int self_fd = ::dirfd(dir);

  errno = 0;
  while (::dirent* entry = ::readdir(dir)) {
    const std::string_view name{entry->d_name};
    if (name == "." || name == "..") continue;

    int fd = -1;
    auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), fd);
    if (ec != std::errc{} || ptr != name.data() + name.size()) continue;  // not a bare fd number

    if (fd == self_fd) continue;  // our own directory handle, closed below regardless
    if (std::find(effective.begin(), effective.end(), fd) != effective.end()) continue;

    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) continue;  // vanished between readdir and fcntl -- D1 is single-threaded, so
                               // this is transient /proc churn, not a race with another thread
    if ((flags & FD_CLOEXEC) == 0) {
      ::closedir(dir);
      return std::unexpected(ConfineError{
          ConfineErrorKind::InheritedFd, "fd " + std::to_string(fd) + " lacks O_CLOEXEC"});
    }
  }
  ::closedir(dir);
  return {};
}

}  // namespace hermit
