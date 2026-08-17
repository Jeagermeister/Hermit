#pragma once

// Configuration: one resolved set of settings, and a record of where each came from.
//
// This is the *frontend* layer of D7's table, not core. It names a model and a base
// URL, and core is defined as the layer that knows about neither. The CLI and the
// eventual MCP server both compose themselves out of this, which is the whole reason
// it is a library rather than argument parsing inlined into main().
//
// --- Two rules inherited from R1, applied to configuration ------------------
//
// R1's failure was not a bad check. It was a *root nobody stated* -- Hermes' terminal
// tool followed the process working directory, its file tools resolved somewhere else,
// and neither had been told anything. The requirement's own words are "never inherit a
// working directory, never infer a project or git root."
//
// Configuration is where that gets decided, so the same discipline applies to it:
//
//  1. **The sandbox root has no default.** Not the working directory, not the git root,
//     not $HOME. Absent is an error. Every other setting here has a defensible default;
//     this one does not, because any default would be an inferred root.
//
//  2. **No implicit config-file search.** A file is read when it is named -- `--config`
//     or `HERMES_CONFIG` -- and never otherwise. Walking up from the working directory
//     looking for `.hermes.json` is the same act as inferring a project root, and it
//     would make the settings depend on where the binary was launched.
//
// --- Precedence -------------------------------------------------------------
//
//   defaults  <  config file  <  environment  <  command-line flags
//
// Later sources overwrite earlier ones field by field, and each field remembers which
// source last wrote it (`origin`). That record is not decoration. Two of the values
// here -- `max_num_ctx` and `minimum_context` -- are safety limits whose numbers this
// project has already had to correct in public once, and "which of the four places set
// this?" is a question that should never require reading the code to answer.
//
// --- Fail closed ------------------------------------------------------------
//
// An unknown key in a config file is an error, not a shrug. A typo'd `max_num_ctx`
// that is silently ignored leaves the clamp at its default while the operator believes
// they raised it -- and D8 records what an unbounded `num_ctx` did to this machine.
// Silently discarding a setting somebody wrote down is the failure mode this codebase
// keeps finding in other people's tools.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermes/ollama/client.h>
#include <hermes/ollama/preflight.h>

namespace hermes::app {

enum class ConfigError {
  FileUnreadable,   // named a config file that could not be opened
  MalformedJson,    // it opened, but did not parse
  NotAnObject,      // it parsed, but the top level is not a JSON object
  UnknownKey,       // a key nothing reads -- almost always a typo, never ignored
  WrongType,        // right key, wrong JSON type
  BadValue,         // right type, but not a value this accepts
  UnknownFlag,      // an argument no command takes
  MissingFlagValue, // a flag that takes a value was last on the line
  MissingRequired,  // the command needs a setting nothing supplied
};

std::string_view to_string(ConfigError e) noexcept;

/// A configuration failure, phrased for one line on stderr at startup.
struct ConfigProblem {
  ConfigError kind;
  std::string detail;

  [[nodiscard]] std::string message() const;
};

template <typename T>
using ConfigResult = std::expected<T, ConfigProblem>;

/// Which of the four sources last wrote a field.
enum class ConfigSource { Default, File, Environment, Flag };

std::string_view to_string(ConfigSource s) noexcept;

/// Every field whose origin is tracked. Kept as an enum with a trailing count rather
/// than ten named members so `render()` is a loop and adding a setting cannot forget
/// to report one.
enum class Field {
  ConfigFile,
  SandboxRoot,
  Model,
  BaseUrl,
  ConnectTimeout,
  MetadataTimeout,
  ChatTimeout,
  MaxNumCtx,
  MinimumContext,
  RequireTools,
  Warmup,
  kCount,
};

/// What a given command actually needs. Validation is per-command on purpose:
/// `resolve` has no model and `preflight` has no sandbox, and demanding both would
/// force operators to invent values that are then never used -- which is how a
/// meaningless sandbox root ends up in somebody's shell history.
struct Requirements {
  bool sandbox_root = false;
  bool model = false;

  /// Whether this command will actually open a Client.
  ///
  /// Gates the D7 loopback check, and that gating is the point: `resolve` never opens a
  /// socket, so an `HERMES_OLLAMA_URL` exported for some other tool must not break pure
  /// path resolution — and must not stop `hermes-cpp config` from printing, since that
  /// is the one command whose job is to show you which value is wrong. A URL that is
  /// never used cannot leak anything; `render()` marks a non-loopback one instead, and
  /// the commands that dial it still refuse.
  bool ollama = false;
};

struct Config {
  /// R1: never inferred, and absolute by the time `load` returns.
  ///
  /// Each source anchors a relative path differently, and `load` settles all three
  /// before anyone sees this: a file anchors to its own directory, the environment
  /// refuses a relative value outright, and a flag anchors to the working directory --
  /// legitimately, since it was typed at launch where that directory is whatever the
  /// operator is looking at. Resolving it in `load` rather than leaving it to
  /// `Sandbox::open` is what lets `render()` show the directory that will be used
  /// instead of the two characters somebody typed.
  ///
  /// `apply_flags` on its own may leave this relative; it is one overlay, not the
  /// finished article.
  std::filesystem::path sandbox_root;

  /// The Ollama tag to drive. No default: guessing which of the fourteen installed
  /// models was meant is not a service to anybody.
  std::string model;

  ollama::ClientOptions client;
  ollama::Policy preflight;

  /// The file that was read, if one was named. `nullopt` means none was, which is the
  /// normal case rather than a degraded one.
  std::optional<std::filesystem::path> config_file;

  [[nodiscard]] ConfigSource origin(Field f) const noexcept {
    return origins_[static_cast<std::size_t>(f)];
  }
  void set_origin(Field f, ConfigSource s) noexcept {
    origins_[static_cast<std::size_t>(f)] = s;
  }

  /// Multi-line, one setting per line with its origin, ending in a newline.
  ///
  /// Also the place where a value that is *in force but unusual* gets said out loud:
  /// a raised `max_num_ctx` (D8 asks for that to be deliberate, against a specific
  /// card) and a disabled R9 context floor both earn a marked line. Nothing here is
  /// rejected for being unusual -- it is reported, so it cannot be in force silently.
  [[nodiscard]] std::string render() const;

 private:
  std::array<ConfigSource, static_cast<std::size_t>(Field::kCount)> origins_{};
};

// --- the pieces, each pure over its inputs ----------------------------------
// Split this way so the precedence rules, the key names and the failure modes are all
// testable without a filesystem, an environment, or a daemon. `load` below is the only
// function that touches any of the three.

/// Overlay a parsed config-file document. Unknown keys fail (see the header comment).
///
/// `base` anchors a relative `sandbox_root`: a path in a file is taken relative to the
/// file's own directory, which is the only anchor that means anything to someone
/// editing that file. Pass the directory containing the document.
[[nodiscard]] ConfigResult<void> apply_json(Config& config, const nlohmann::json& doc,
                                            const std::filesystem::path& base);

using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

/// Overlay the environment. Deliberately a short list -- `HERMES_SANDBOX_ROOT`,
/// `HERMES_MODEL`, `HERMES_OLLAMA_URL`, `HERMES_MAX_NUM_CTX` -- covering the values
/// that vary per *machine*. Timeouts and preflight policy are not exposed here: an
/// environment variable is the easiest thing in the world to set once in a shell
/// profile and forget, and a long tail of them is a long tail of settings quietly in
/// force on one machine and not another. That is the confound D3 exists to avoid.
///
/// `HERMES_SANDBOX_ROOT` must be absolute. Unlike a flag it is not typed at launch and
/// unlike a file it has no directory to be relative to, so there is no honest anchor.
[[nodiscard]] ConfigResult<void> apply_env(Config& config, const EnvLookup& lookup);

/// Overlay command-line flags. `args` excludes the program name and the subcommand.
///
/// Anything not starting with `-` is left in `positional` for the command to read;
/// `resolve` takes its paths that way, and `--` ends the options so a path may begin
/// with a dash. `--config` is recognised and ignored here -- it selects the file, which
/// was applied before this runs.
///
/// `config` is left untouched when this returns an error. `positional` is not: it may
/// hold whatever was collected before the failing argument, which callers should
/// ignore on error rather than rely on.
[[nodiscard]] ConfigResult<void> apply_flags(Config& config, std::span<const std::string_view> args,
                                             std::vector<std::string_view>& positional);

/// The flags that consume the argument after them.
///
/// Exposed so the drift test reads the *real* table rather than a third hand-kept copy
/// of it. `apply_flags` dispatches through its own if/else, so the two could disagree;
/// `take()` refuses to run for a flag missing from this list, which turns that
/// disagreement into a loud failure the first time the flag is used instead of a
/// silent mis-parse of somebody's command line.
[[nodiscard]] std::span<const std::string_view> flags_taking_a_value() noexcept;

/// The checks that need the whole config rather than one field.
///
/// Includes D7's loopback rule, by calling `ollama::validate_base_url` rather than
/// restating it -- there must be exactly one implementation of "this URL is local",
/// since it is what keeps sandbox file contents on the machine.
[[nodiscard]] ConfigResult<void> validate(const Config& config, const Requirements& required);

/// Find `--config PATH` in `args` without applying anything else. The file has to be
/// located before the other sources are overlaid, because it sits *under* them.
///
/// Steps over other flags' values, so a `--config` written where a value belongs is
/// not read as a request to load a file. Two parsers over one command line have to
/// agree about which tokens are values.
[[nodiscard]] ConfigResult<std::optional<std::string_view>> find_config_flag(
    std::span<const std::string_view> args);

/// Which config file to read, if any: `--config` if present, otherwise `HERMES_CONFIG`.
///
/// Takes the environment as a parameter for the same reason `apply_env` does -- this
/// is the setting that selects all the other settings, and it was the one place a
/// `getenv` call made the decision untestable. `HERMES_CONFIG=` set-but-empty is an
/// error here, matching every other variable rather than silently meaning "no file".
[[nodiscard]] ConfigResult<std::optional<std::string>> select_config_file(
    std::span<const std::string_view> args, const EnvLookup& lookup);

/// Parse a whole non-negative integer. Rejects trailing junk, so `--max-num-ctx 64k`
/// is an error rather than 64 -- a silent partial parse of a safety limit is exactly
/// the failure this codebase keeps writing requirements about.
[[nodiscard]] ConfigResult<std::uint64_t> parse_whole(std::string_view text,
                                                      std::string_view what);

// --- the composed thing -----------------------------------------------------

/// Read the file (if one is named), overlay environment and flags, and validate.
///
/// The only function here that touches the filesystem or the environment.
///
/// ⚠ **`args` must outlive `positional`.** Nothing here copies an argument: the views
/// handed in are the views handed back. From `main` that is free, since they point
/// into `argv`, which lives as long as the process. The MCP frontend is where this
/// becomes a trap -- a request whose arguments are built into `std::string`s that go
/// out of scope leaves every element of `positional` dangling, and the returned
/// `Config` (which owns its strings) will look perfectly healthy next to it.
///
/// `positional` is cleared on entry, so reusing one vector across two calls does not
/// accumulate.
[[nodiscard]] ConfigResult<Config> load(std::span<const std::string_view> args,
                                        const Requirements& required,
                                        std::vector<std::string_view>& positional);

}  // namespace hermes::app
