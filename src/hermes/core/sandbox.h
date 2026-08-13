#pragma once

// R1 --- One sandbox root. Every path resolved against it, explicitly.
//
// Evidence (bench/fsops, 2026-08-12/13): Hermes Agent's `terminal` tool follows the
// process working directory and ignores --in entirely, while its file tools resolve
// somewhere else again. Agents wrote hello.txt, project/src/utils/, data/ and logs/
// into the repository root while their work tree sat four levels below. llama32-3b
// scored 0/3 on 01_create_file having produced the exactly-correct file with the
// exactly-correct content, in the wrong directory -- a tool-design bug measured as
// model inability.
//
// The fix is structural, not procedural: Sandbox::resolve is the only way to obtain a
// SandboxPath, and it rejects anything outside the root rather than silently rebasing
// it. Any function that accepts a SandboxPath is therefore R1-correct by construction
// and does not re-check.
//
// resolve() never consults the process working directory. Sandbox::open does, but only
// to canonicalise a relative root at startup -- see the note there.

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace hermes {

enum class SandboxError {
  RootNotFound,      // the root does not exist
  RootNotDirectory,  // the root exists but is not a directory
  FilesystemError,   // canonicalisation failed (permissions, I/O)
};

enum class PathError {
  Empty,            // empty string is not a path
  EmbeddedNul,      // NUL byte -- would truncate at the syscall boundary
  EscapesRoot,      // resolved outside the sandbox root; rejected, never rebased
  TooLong,          // longer than PATH_MAX; rejected before any allocation
  FilesystemError,  // symlink resolution failed, or a component could not be inspected
};

std::string_view to_string(SandboxError e) noexcept;
std::string_view to_string(PathError e) noexcept;

class Sandbox;

/// A path proven to lie inside a Sandbox root.
///
/// Only Sandbox::resolve constructs one. Take this type, rather than a
/// std::filesystem::path, anywhere a tool touches the filesystem: the R1 check then
/// cannot be forgotten, because there is no other way to name a file.
class SandboxPath {
 public:
  /// Absolute, normalised, symlinks resolved. Safe to hand to the OS.
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return abs_; }

  /// Path relative to the sandbox root -- for logs and model-facing messages, so
  /// neither ever leaks the absolute layout of the host.
  [[nodiscard]] const std::filesystem::path& relative() const noexcept { return rel_; }

  [[nodiscard]] std::string string() const { return abs_.string(); }

  friend bool operator==(const SandboxPath&, const SandboxPath&) = default;

 private:
  friend class Sandbox;
  SandboxPath(std::filesystem::path abs, std::filesystem::path rel)
      : abs_(std::move(abs)), rel_(std::move(rel)) {}

  std::filesystem::path abs_;
  std::filesystem::path rel_;
};

/// The single root every path is resolved against.
class Sandbox {
 public:
  /// Open `root` as a sandbox. The root must already exist and be a directory; it is
  /// canonicalised once here, so a root reached through a symlink still compares
  /// correctly against resolved paths later.
  ///
  /// A *relative* root is canonicalised against the process working directory -- the
  /// only place in this class that directory is read. Prefer passing an absolute root;
  /// a relative one makes startup depend on where the binary was launched, which is
  /// the ambiguity R1 exists to remove.
  [[nodiscard]] static std::expected<Sandbox, SandboxError> open(
      const std::filesystem::path& root);

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  /// Resolve a model-supplied path against the root.
  ///
  /// Relative paths resolve against the root -- never the process working directory.
  /// Absolute paths are permitted only if they land inside the root. The target need
  /// not exist: writes create files, so only containment of the resolved path matters.
  ///
  /// Paths are resolved one component at a time, expanding symlinks as they are met,
  /// matching POSIX resolution. So `..` after a symlink means what the OS means by it:
  /// with `link -> a/b`, "link/../x" is "a/x", not "x". A link pointing out of the
  /// root is rejected rather than collapsed back inside. Interior `..` is fine as long
  /// as the final path is inside, so a route that leaves and returns is accepted --
  /// the file it ends up naming is what is judged.
  ///
  /// No tilde expansion and no environment substitution: "~/x" names a directory
  /// literally called "~". Inferring a home directory would be exactly the kind of
  /// implicit root R1 exists to forbid.
  [[nodiscard]] std::expected<SandboxPath, PathError> resolve(std::string_view raw) const;

 private:
  explicit Sandbox(std::filesystem::path canonical_root)
      : root_(std::move(canonical_root)) {}

  /// True if `absolute_resolved` lies at or below the root, comparing whole path
  /// components so "/srv/work2" is not treated as inside "/srv/work".
  ///
  /// Deliberately private: it assumes an already-resolved absolute path and fails
  /// *open* on anything else, so it is not a safety check a caller may reach for.
  [[nodiscard]] bool contains(const std::filesystem::path& absolute_resolved) const;

  /// POSIX-order resolution: walk components, expanding symlinks as encountered.
  [[nodiscard]] std::expected<std::filesystem::path, PathError> walk(
      const std::filesystem::path& absolute_input) const;

  std::filesystem::path root_;
};

}  // namespace hermes
