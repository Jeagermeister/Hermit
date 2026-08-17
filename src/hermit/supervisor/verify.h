#pragma once

// R6 --- what the filesystem shows, against what the model claims.
//
// ROUTING.md section 6 splits verification in two and is explicit that only the second
// half holds unconditionally:
//
//   Per-tool   only the tools the model actually chose   R5 read-back on write / edit
//   Per-turn   everything, including shell and tools     R4 snapshot, R3 hash-diff,
//              we did not write                          R6 poll state
//
// The per-tool half exists and has since 2026-08-16. This is the per-turn half, and it is
// a supervisor concern because it must not care which tool ran -- or whether one did.
//
// --- It verifies facts, not claims, and that distinction is the whole design ---
//
// The natural reading of R6 ("never trust a completion claim") is: parse what the model
// said it did, and check each statement. That is a trap. It reintroduces exactly the
// dependency R6 exists to remove -- the reply becomes load-bearing again, just later in
// the pipeline -- and prose does not parse reliably. The measured failure is not that
// models lie in a detectable grammar; it is that they are confidently wrong in fluent
// English. `llama3.2-3b` was handed a tool result whose `content` field was the four
// characters `aaaa` and reported "a.txt is 1 character long". There is no parser for that.
//
// So nothing here reads the model's output. A snapshot before, a snapshot after, and the
// difference between them is ground truth about what happened, computable whether the
// model spoke prose, called ten tools, or crashed mid-turn.
//
// **What that does and does not settle.** It answers "what changed" exactly. It does not
// answer "is the work done", because that needs a post-condition and a free-text
// instruction does not have one -- see the note at the end of this header, which is the
// open half of Phase 3 rather than something quietly assumed here.
//
// --- Hashing incrementally, because R3 forbids the cheap check ----------------
//
// R3 is blunt: verify by content hash, never by existence. Its evidence is `05_copy`,
// where two models overwrote `config.ini` with invented content and the assertion
// "config.ini still exists" passed for both. So a stat-only diff is not enough -- and it
// is not enough in the subtler direction too, since an identity tuple can move without
// the content changing at all (a touch, a chmod, a rewrite of identical bytes). Reporting
// those as modifications would train a reader to ignore the report.
//
// But hashing every file every turn costs O(tree bytes) per turn, on a loop whose whole
// justification is that a native binary is cheap to launch. The resolution is that a
// snapshot carries its hashes forward:
//
//   * every turn walks and stats the tree -- cheap, one syscall per entry;
//   * an entry whose identity tuple is unchanged keeps the hash already computed for it;
//   * only entries that are new, or whose tuple moved, are read and hashed.
//
// The first snapshot pays for a full baseline, which is unavoidable -- there is nothing to
// carry forward from. Every snapshot after it costs the walk plus the bytes that actually
// changed.
//
// The tuple is trustworthy for this because it is already section 4's currency for the
// staleness guard: `dev:ino` catches unlink-and-recreate, and `ctime` is the field a
// confined process cannot forge while backdating `mtime` (D10). A file rewritten in place
// with identical size and mtime still moves its ctime.
//
// --- Whole tree, not the paths the tools touched ------------------------------
//
// Restricting the walk to paths the tools reported would be far cheaper and is wrong for
// the one reason section 6 was written: per-turn verification exists precisely to cover
// "shell and tools we did not write". A changeset built from tool output inherits the
// blind spot of the tool layer, which is the failure being defended against. Both
// destructive `05_copy` incidents and every escape into the repository root came through
// file tools, and a supervisor that trusted those tools' own account of what they touched
// would have seen nothing wrong.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <hermit/core/observed.h>
#include <hermit/core/sandbox.h>

namespace hermit::supervisor {

/// What one entry looked like at snapshot time.
struct FileState {
  IdentityTuple tuple{};

  /// SHA-256 of the content, hex. Empty for anything that is not a regular file -- a
  /// directory, a symlink, and also a FIFO, socket or device, none of which are opened --
  /// and empty for a regular file that could not be read; `readable` distinguishes "no
  /// hash because there is nothing to hash" from "no hash because we were refused".
  ///
  /// Known gap, recorded rather than hidden: `FileState` carries no `is_regular`, so
  /// `diff` cannot see a regular file becoming a FIFO (reported `Modified`, hash to
  /// nothing) or a FIFO becoming a socket (reported `TouchedOnly`, and so not
  /// substantive). Both surface; one is mislabelled and one is under-weighted.
  std::string sha256{};

  bool is_dir = false;
  bool is_symlink = false;

  /// False when a regular file's content could not be read. The entry is still recorded:
  /// it exists, and reporting it as absent would be a worse lie than reporting it
  /// unhashed. A file that stays unreadable across a turn is reported as unchanged; one
  /// that becomes unreadable is reported, because that is itself a change worth seeing.
  bool readable = true;

  /// Permission bits only -- `st_mode & 07777`, so setuid, setgid and the sticky bit are
  /// included and the file-type bits are not (those are already `is_dir`/`is_symlink`).
  ///
  /// Recorded because a `chmod` moves ctime and no content, so without it the single most
  /// security-relevant thing a model can do to a file it may not rewrite -- make it
  /// executable -- was indistinguishable from a touch, and `TouchedOnly` is the kind this
  /// header tells readers to skim. `08_write_and_run_script` asks for exactly that
  /// ("make count.sh executable and run it"), so it is ordinary behaviour and not an
  /// exotic case.
  ///
  /// Not added to `IdentityTuple`: that type is section 4's currency for the staleness
  /// guard and is shared with every tool, so widening it would ripple through the whole
  /// mutate surface to answer a question only this layer asks.
  std::uint32_t mode = 0;

  friend bool operator==(const FileState&, const FileState&) = default;
};

/// Sandbox-relative path -> state. Ordered, so a changeset reads in path order and two
/// runs over the same tree produce byte-identical reports.
using TreeSnapshot = std::map<std::string, FileState>;

enum class ChangeKind {
  Created,
  Deleted,

  /// Content differs, with a hash on both sides proving it -- the only kind that says so
  /// about a path that existed before *and* after, and the only one R3 would accept as
  /// evidence of an in-place mutation. `Created` and `Deleted` move bytes too; see
  /// `Changeset::substantive`, which counts all three plus `TypeChanged`.
  Modified,

  /// The identity tuple moved but the content did not: a touch, a chmod, a rewrite of
  /// identical bytes, or a file replaced by a copy of itself. Kept separate from
  /// `Modified` deliberately -- folding the two would make every report noisier than the
  /// signal in it, and a reader who learns to skim a noisy report is worse off than one
  /// with no report.
  TouchedOnly,

  /// A regular file became unreadable, or became readable again. Neither is a content
  /// change that can be proven, and both are worth surfacing rather than swallowing.
  ReadabilityChanged,

  /// The permission bits moved and the content did not -- a `chmod`.
  ///
  /// Counted by `substantive()` even though no bytes moved, which is the one place that
  /// method departs from "bytes". Gaining the executable bit is an act with consequences,
  /// not an artefact of one, and a supervisor that reported it in the same breath as a
  /// touched mtime would be training its reader to miss it. When content moved *as well*,
  /// the kind stays `Modified` -- the larger fact wins -- but `mode_before`/`mode_after`
  /// are still filled in, so a `chmod` is never hidden behind a rewrite.
  PermissionsChanged,

  /// A path changed what it *is* -- file to directory, directory to symlink. Rare, and
  /// never innocent enough to fold into Modified.
  TypeChanged,
};

std::string_view to_string(ChangeKind k) noexcept;

struct Change {
  std::string path;  // sandbox-relative
  ChangeKind kind = ChangeKind::Modified;

  /// Hex hashes where they exist and are meaningful. `before` is empty for Created,
  /// `after` for Deleted, and both for entries that are not readable regular files.
  /// Both carry an explicit `{}` so the designated initialisers that build a Change --
  /// most of which set only one side -- do not have to name the other.
  std::string before{};
  std::string after{};

  /// Permission bits either side, filled in whenever they differ -- on *any* kind, not
  /// only `PermissionsChanged`. Both zero when they did not move. A rewrite that also
  /// flipped the executable bit reports as `Modified` and still shows the flip here.
  std::uint32_t mode_before = 0;
  std::uint32_t mode_after = 0;
};

/// What a turn did to the tree, in path order.
struct Changeset {
  std::vector<Change> changes;

  [[nodiscard]] bool empty() const noexcept { return changes.empty(); }

  /// Changes a reader must not skim past: created, deleted, modified, type-changed, and
  /// permissions-changed. The number that answers "did anything actually happen", which is
  /// a different question from "did anything get touched".
  ///
  /// Four of the five moved bytes; `PermissionsChanged` did not, and is counted anyway
  /// because a `chmod +x` is an act rather than a side effect of one.
  [[nodiscard]] std::size_t substantive() const noexcept;

  /// One line per change, sorted, ending in a newline. For a report or a trace.
  [[nodiscard]] std::string render() const;
};

enum class VerifyErrorKind {
  /// A directory could not be read. Fatal to a snapshot, and deliberately so: an
  /// unreadable directory hides a whole subtree, and every file under it would otherwise
  /// appear deleted. A verification that cannot see the tree must say so rather than
  /// report a confident diff of what it managed to reach.
  DirectoryUnreadable,

  /// The root itself could not be opened.
  RootUnreadable,
};

std::string_view to_string(VerifyErrorKind e) noexcept;

struct VerifyError {
  VerifyErrorKind kind = VerifyErrorKind::DirectoryUnreadable;
  std::string path;  // sandbox-relative, empty for the root
  int code = 0;      // errno

  [[nodiscard]] std::string message() const;
};

/// Walks one sandbox root and reports what changed between walks.
///
/// Holds the Sandbox by reference for its lifetime, like everything else in this layer.
/// Does no I/O of its own beyond the walk: it never writes, and it never opens anything
/// outside the root, so a verifier can be pointed at a live tree a model is working in.
class TreeVerifier {
 public:
  explicit TreeVerifier(const Sandbox& sandbox) noexcept : sandbox_(&sandbox) {}

  /// Walk the tree and record every entry.
  ///
  /// `previous` is the optimisation and the reason this is not a free function: entries
  /// whose identity tuple matches `previous` keep its hash instead of being re-read. Pass
  /// nullptr for a cold baseline, which hashes everything.
  ///
  /// Symlinks are recorded, never followed -- the walk uses `O_NOFOLLOW` on every descent
  /// and `AT_SYMLINK_NOFOLLOW` on every stat, so a directory swapped for a symlink between
  /// the stat and the open is a refusal rather than a traversal out of the root. That is
  /// the same discipline `find` already uses.
  [[nodiscard]] std::expected<TreeSnapshot, VerifyError> snapshot(
      const TreeSnapshot* previous = nullptr) const;

  /// Bytes read and hashed by the last `snapshot()` call, and entries walked. Exposed
  /// because the incremental claim above is a performance claim, and a performance claim
  /// nobody can measure is a performance hope.
  [[nodiscard]] std::uint64_t last_hashed_bytes() const noexcept { return hashed_bytes_; }
  [[nodiscard]] std::size_t last_entries_walked() const noexcept { return entries_walked_; }

 private:
  const Sandbox* sandbox_;
  mutable std::uint64_t hashed_bytes_ = 0;
  mutable std::size_t entries_walked_ = 0;
};

/// What changed between two snapshots, in path order.
///
/// Pure over its inputs -- no filesystem access at all -- so the whole classification
/// table is testable without a tree.
[[nodiscard]] Changeset diff(const TreeSnapshot& before, const TreeSnapshot& after);

// --- The open half of Phase 3, stated rather than assumed --------------------
//
// This header answers "what changed". It does not answer "is the work done", and nothing
// here should be read as if it did.
//
// Deciding completion needs a **post-condition**: a statement of what the tree should look
// like afterwards, against which a changeset can be judged. `bench/fsops` has them, which
// is why its tasks can be scored at all. A person typing `hermit agent "tidy up the
// logs"` has not supplied one, and no amount of filesystem observation invents it.
//
// So there are two honest positions and the project has to pick one, which is a decision
// for DECISIONS.md and not for this file:
//
//   1. **The supervisor is given post-conditions** -- by a task definition, a caller, or a
//      Tier 1 model asked to write them before work starts. Then R7 re-invocation has
//      something concrete to re-state, which is what the tournament recommendation
//      actually requires: *one concrete remaining failure*, not "try again".
//   2. **The supervisor reports rather than judges** -- it hands back the changeset and
//      lets the caller decide. Weaker, and it makes `bench/delta`'s reliability arm
//      unmeasurable, because there is no verdict to compare against.
//
// Until that is settled, a changeset is evidence a caller can act on, and the loop reports
// it instead of ruling on it.

}  // namespace hermit::supervisor
