#pragma once

// Where stated post-conditions come from.
//
// [judge.h](../supervisor/judge.h) decides expectations against two snapshots; nothing
// produced them, so the judgment half was unreachable code. This is the source. It lives
// in `app` because stating a post-condition is a frontend act -- a CLI flag today, a
// config file beside it, MCP arguments later -- and because it needs the `Sandbox`, which
// `supervisor` does not hold.
//
// --- The path contract, which is the only hard part --------------------------
//
// `TreeSnapshot` is keyed on sandbox-relative paths, spelled `lexically_normal()` from the
// root (verify.cpp). An expectation that names a path any other way is not rejected at
// judge time -- it is silently `Unmet`. That is worse than an error, because an unmet
// finding is exactly what R7 restates to the model: a typo would be re-injected as a
// concrete failure and the model would be sent to fix something that was never wrong.
//
// So paths are checked here and refused loudly. Two rules, and the second is not obvious:
//
//   1. **`Sandbox::resolve` gates containment.** `..` escapes, symlinks pointing out of
//      the root, NULs, control characters and over-long paths are refused with the
//      caller's own spelling quoted back. Paths that do not exist yet pass -- verified,
//      not assumed: `sub/report.md` resolves cleanly with no `sub/`, which it must, since
//      "create this" is the common expectation.
//
//   2. **`..` is refused outright**, which is the rule that makes 1 and 3 agree. `resolve`
//      walks POSIX-order and expands each symlink as it is met; a key folded textually
//      does not. With `dirlink -> a/b`, "dirlink/../x" resolves to `a/x` and normalises to
//      `x`, so the gate would pass on one path while the key named another. Without `..`
//      the two cannot diverge, because the walker builds keys from literal component
//      names and never follows a link either.
//
//   3. **The key is the literal normalised spelling, never `resolve()`'s output.**
//      `resolve` expands symlinks; the snapshot walker uses `lstat` and never follows, and
//      judge.h commits to that outright -- a symlink to a directory does not satisfy
//      `Exists`. With `link.md -> notes.txt`, `resolve("link.md")` returns relative
//      `notes.txt`, so keying on it would answer about `notes.txt` while the operator
//      asked about `link.md`. The resolve is a gate, not a translation.
//
// Two consequences, named here rather than discovered later:
//
//   * A symlink whose target lies outside the root cannot be named at all, because rule 1
//     refuses it -- including by `Absent`, where refusing is arguably backwards. The
//     snapshot does hold a key for that link. Widening rule 1 needs a better reason than
//     symmetry.
//   * Absolute paths are refused even when they land inside the root. Rebasing one to a
//     relative key is a second spelling of the same path, and R1's original bug was an
//     inferred root. Expectations are stated relative to `--root`, and say so.

//
// --- Sets that no run could satisfy -----------------------------------------
//
// Checked across the whole set, not per spec, and refused. `exists:x` beside `absent:x`
// parses cleanly one line at a time and is unsatisfiable together: judged `Unmet` every
// turn, so R7 restates the same impossible failure until a bound stops the run. The
// operator then gets "unmet" with nothing pointing at the *set* as the broken thing. Which
// predicate requires which path to be present is read off judge.cpp rather than guessed --
// notably `preserved:a=b` constrains only `b`, so `absent:a` beside it is a *move* and
// perfectly satisfiable.

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <hermit/core/sandbox.h>
#include <hermit/supervisor/judge.h>

namespace hermit::app {

/// A refused expectation, carrying the caller's own text so the message names what they
/// typed rather than what it was parsed into.
struct ExpectError {
  std::string spec{};
  std::string message{};

  /// `expectation "exists:": empty path`
  [[nodiscard]] std::string render() const;
};

/// What a caller stated, and what it admits it could not state.
///
/// `unjudged` is carried alongside the set rather than derived from it, because it counts
/// requirements that have no expression here at all -- `08_write_and_run_script` needs
/// "output contains TXTCOUNT=3", which is a reply marker and not a tree fact. Without it a
/// verdict on that task reads as success while a real requirement goes unexamined.
struct Expectations {
  supervisor::ExpectationSet set{};
  std::size_t unjudged = 0;

  [[nodiscard]] bool empty() const noexcept { return set.empty() && unjudged == 0; }
};

/// Parse one `kind:path` or `kind:from=to` spec.
///
/// Grammar, which is the whole surface:
///
///     exists:PATH          the path is present afterwards
///     dir:PATH             present afterwards *and* a directory
///     absent:PATH          not present afterwards
///     preserved:FROM=TO    TO now holds the bytes FROM held at baseline
///     identical:A=B        A and B hold the same bytes now
///
/// A one-sided kind takes everything after the colon, so `exists:report=final.md` names a
/// file with an '=' in it. A two-sided kind needs exactly one '=' and refuses a second
/// rather than guessing which one splits the pair; the object form below has no such
/// limit and is the way to write those paths.
///
/// `preserved:p=p` is the "do not touch p" constraint, and judge.h excludes it from the
/// vacuity flag for that reason.
[[nodiscard]] std::expected<supervisor::Expectation, ExpectError> parse_expectation(
    std::string_view spec, const Sandbox& sandbox);

/// Parse the config-file form.
///
/// Elements may be either the compact string form above or an object:
///
///     {"kind": "exists",    "path": "index.md"}
///     {"kind": "exists",    "path": "archive", "as_directory": true}
///     {"kind": "preserved", "path": "notes.txt", "other": "notes.txt"}
///
/// `value` is the whole settings object, not the array: `unjudged` is a sibling key of
/// `expectations`, so reading one without the other would silently drop it.
///
/// The single entry point, and deliberately so. Repeated `--expect` flags are collected into
/// this same array by the config layer, so the command line and the config file are not two
/// parsers that can disagree -- and no test can exercise a path production never runs.
[[nodiscard]] std::expected<Expectations, ExpectError> parse_expectations_json(
    const nlohmann::json& value, const Sandbox& sandbox);

}  // namespace hermit::app
