#include <hermit/core/confine.h>

#include "confine_internal.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
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
std::expected<int, ConfineError> fork_exec_wait(CommandLine& cl) {
  pid_t pid = ::fork();
  if (pid < 0) {
    return std::unexpected(ConfineError{ConfineErrorKind::Fork, std::strerror(errno)});
  }
  if (pid == 0) {
    _exit(landlock_run_cli_main(cl.argc(), cl.argv()));
  }

  int status = 0;
  int wait_rc = 0;
  // EINTR is retried, not treated as failure: a stray signal delivered while blocked here (R8's
  // future wall-clock bound is the expected source, once it lands as a signal-based timeout)
  // must not be mistaken for the child's own outcome. Without the retry, EINTR would both
  // misreport a healthy child as a Wait error AND leak it as a zombie, since it was never
  // actually reaped.
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

}  // namespace

std::expected<ConfineResult, ConfineError> run_confined(
    const std::filesystem::path& root, std::span<const std::string> command,
    std::span<const int> allowed) {
  if (auto fds = assert_no_inheritable_fds(allowed); !fds) {
    return std::unexpected(fds.error());
  }

  CommandLine cl;
  push_system_grants(cl, root);
  cl.push("--");
  for (const auto& arg : command) cl.push(arg);
  cl.finish();

  auto exit_code = fork_exec_wait(cl);
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

  auto exit_code = fork_exec_wait(cl);
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
