#include <hermit/app/config.h>

#include <charconv>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hermit::app {
namespace {

// Every key this understands, in one place. The unknown-key check reads these, so a
// setting that is added here and nowhere else fails loudly rather than being accepted
// and ignored -- which is the direction of failure worth having.
constexpr std::string_view kTopLevelKeys[] = {"sandbox_root", "model",        "ollama",
                                              "preflight",    "expectations", "unjudged", "shell"};
constexpr std::string_view kOllamaKeys[] = {"base_url", "connect_timeout_s", "metadata_timeout_s",
                                            "chat_timeout_s", "max_num_ctx"};
constexpr std::string_view kPreflightKeys[] = {"minimum_context", "require_tools", "warmup"};
constexpr std::string_view kShellKeys[] = {"enabled", "timeout_s"};

/// Every flag that consumes the argument after it.
///
/// Shared with `find_config_flag`, which has to know what to *skip* -- otherwise a
/// `--config` sitting in a value position (`--model --config x.json`) is picked up as
/// a request to load `x.json` while `apply_flags` reads the same line as a model
/// literally named "--config". Two parsers over one command line have to agree about
/// which tokens are values, and this is the one place that says so.
///
/// `apply_flags` still dispatches through its own if/else rather than this table, so
/// the two *could* drift. Two things stop it. `take()` refuses to run for a flag that
/// is not listed here, so a value-taking flag added to the chain and forgotten here
/// fails loudly the first time it is used rather than silently mis-parsing a command
/// line. And the suite reads this table directly through `flags_taking_a_value()`,
/// which catches the other direction: a *boolean* flag wrongly listed here would make
/// `find_config_flag` skip a live `--config`, quietly not loading a named file.
constexpr std::string_view kFlagsTakingAValue[] = {
    "--config",      "--root",             "--model",        "--url",
    "--max-num-ctx", "--min-context",      "--connect-timeout", "--metadata-timeout",
    "--chat-timeout", "--expect",          "--unjudged",       "--shell-timeout",
};

bool takes_a_value(std::string_view flag) {
  for (const std::string_view candidate : kFlagsTakingAValue) {
    if (flag == candidate) return true;
  }
  return false;
}

/// A token that cannot be a flag's value.
///
/// `--model --root /srv` is somebody who forgot the model name, not somebody naming a
/// model "--root". Rejecting it turns a silent mis-parse -- where the next flag is
/// eaten as a value and the one after it becomes a stray positional -- into one line
/// saying which flag is missing its argument. No setting here has a legitimate value
/// beginning with two dashes; paths that do are reachable after `--`.
bool looks_like_a_flag(std::string_view token) { return token.starts_with("--"); }

/// Above this, a "timeout" has stopped bounding anything.
///
/// The upper bound is not cosmetic. `std::chrono::seconds::rep` is `int64_t`, so a
/// value above INT64_MAX wraps negative on the cast -- measured: 18446744073709551615
/// arrives as -1. cpp-httplib then computes `static_cast<int>(sec * 1000 + usec / 1000)`
/// and hands it to `poll(2)`, where **a negative timeout means block forever**. So an
/// unbounded timeout setting does not merely misconfigure the wait, it disables it, and
/// on the one setting whose job is to bound a wedged daemon. The `static_cast<int>`
/// overflows on its own past roughly 24.8 days; a day is comfortably clear of both and
/// is already far beyond any real generation.
constexpr std::uint64_t kMaxTimeoutSeconds = 86400;

std::string join(std::span<const std::string_view> keys) {
  std::string out;
  for (const std::string_view key : keys) {
    if (!out.empty()) out += ", ";
    out += key;
  }
  return out;
}

ConfigProblem unknown_key(std::string_view where, std::string_view key,
                          std::span<const std::string_view> known) {
  std::string detail = "unknown key ";
  detail += where.empty() ? "" : std::string{where} + ".";
  detail += key;
  detail += "; this file understands: " + join(known);
  return {ConfigError::UnknownKey, std::move(detail)};
}

ConfigProblem wrong_type(std::string_view key, std::string_view wanted) {
  return {ConfigError::WrongType, std::string{key} + " must be " + std::string{wanted}};
}

/// Reject any key in `object` that is not in `known`. Run before reading anything, so
/// a file with one typo reports the typo rather than a later missing-value symptom.
ConfigResult<void> reject_unknown(const nlohmann::json& object, std::string_view where,
                                  std::span<const std::string_view> known) {
  for (const auto& entry : object.items()) {
    const std::string& key = entry.key();
    bool found = false;
    for (const std::string_view candidate : known) {
      if (key == candidate) {
        found = true;
        break;
      }
    }
    if (!found) return std::unexpected(unknown_key(where, key, known));
  }
  return {};
}

/// Read one unsigned field, distinguishing "absent" from "present and wrong".
///
/// `is_number_unsigned` rather than `is_number_integer`: -1 parsed as a signed integer
/// and converted to uint64 becomes 18446744073709551615, which would sail past every
/// range check below as an enormous but positive value.
ConfigResult<bool> read_uint(const nlohmann::json& object, std::string_view key,
                             std::string_view qualified, std::uint64_t& out) {
  const auto it = object.find(key);
  if (it == object.end()) return false;
  if (!it->is_number_unsigned()) return std::unexpected(wrong_type(qualified, "a whole number"));
  out = it->get<std::uint64_t>();
  return true;
}

ConfigResult<bool> read_seconds(const nlohmann::json& object, std::string_view key,
                                std::string_view qualified, std::chrono::seconds& out) {
  std::uint64_t value = 0;
  auto present = read_uint(object, key, qualified, value);
  if (!present) return std::unexpected(present.error());
  if (!*present) return false;
  if (value == 0) {
    return std::unexpected(
        ConfigProblem{ConfigError::BadValue, std::string{qualified} + " must be at least 1 second"});
  }
  if (value > kMaxTimeoutSeconds) {
    return std::unexpected(ConfigProblem{
        ConfigError::BadValue, std::string{qualified} + " must be at most " +
                                   std::to_string(kMaxTimeoutSeconds) + " seconds (one day)"});
  }
  out = std::chrono::seconds{static_cast<std::chrono::seconds::rep>(value)};
  return true;
}

ConfigResult<bool> read_bool(const nlohmann::json& object, std::string_view key,
                             std::string_view qualified, bool& out) {
  const auto it = object.find(key);
  if (it == object.end()) return false;
  if (!it->is_boolean()) return std::unexpected(wrong_type(qualified, "true or false"));
  out = it->get<bool>();
  return true;
}

ConfigResult<bool> read_string(const nlohmann::json& object, std::string_view key,
                               std::string_view qualified, std::string& out) {
  const auto it = object.find(key);
  if (it == object.end()) return false;
  if (!it->is_string()) return std::unexpected(wrong_type(qualified, "a string"));
  out = it->get<std::string>();
  if (out.empty()) {
    return std::unexpected(
        ConfigProblem{ConfigError::BadValue, std::string{qualified} + " must not be empty"});
  }
  return true;
}

}  // namespace

std::string_view to_string(ConfigError e) noexcept {
  switch (e) {
    case ConfigError::FileUnreadable:   return "config file could not be read";
    case ConfigError::MalformedJson:    return "config file is not valid JSON";
    case ConfigError::NotAnObject:      return "config file is not a JSON object";
    case ConfigError::UnknownKey:       return "unknown setting";
    case ConfigError::WrongType:        return "setting has the wrong type";
    case ConfigError::BadValue:         return "setting has an unusable value";
    case ConfigError::UnknownFlag:      return "unrecognised argument";
    case ConfigError::MissingFlagValue: return "flag is missing its value";
    case ConfigError::MissingRequired:  return "required setting was not supplied";
  }
  return "unknown configuration error";
}

std::string_view to_string(ConfigSource s) noexcept {
  switch (s) {
    case ConfigSource::Default:     return "default";
    case ConfigSource::File:        return "file";
    case ConfigSource::Environment: return "env";
    case ConfigSource::Flag:        return "flag";
  }
  return "?";
}

std::string ConfigProblem::message() const {
  return std::string{to_string(kind)} + ": " + detail;
}

ConfigResult<std::uint64_t> parse_whole(std::string_view text, std::string_view what) {
  std::uint64_t value = 0;
  const char* const last = text.data() + text.size();
  const auto [stop, ec] = std::from_chars(text.data(), last, value);
  // Out of range is reported separately: "expects a whole number" is a lie about a
  // string that is entirely digits, and sends the reader looking for a typo that is
  // not there.
  if (ec == std::errc::result_out_of_range) {
    return std::unexpected(ConfigProblem{
        ConfigError::BadValue,
        std::string{what} + " is too large to represent, got: " + std::string{text}});
  }
  // `stop != last` carries as much weight as the error code. Without it "64k" parses
  // as 64 and a safety limit silently drops by three orders of magnitude.
  if (ec != std::errc{} || stop != last) {
    return std::unexpected(ConfigProblem{
        ConfigError::BadValue,
        std::string{what} + " expects a whole number, got: " + std::string{text}});
  }
  return value;
}

// --- config file ------------------------------------------------------------

namespace {

/// The body of `apply_json`, which mutates as it goes. Called on a copy so a document
/// that fails on its ninth key does not leave eight settings applied -- see below.
ConfigResult<void> overlay_json(Config& config, const nlohmann::json& doc,
                                const std::filesystem::path& base) {
  if (!doc.is_object()) {
    return std::unexpected(ConfigProblem{ConfigError::NotAnObject,
                                         "the top level must be a JSON object, e.g. {\"model\": …}"});
  }
  if (auto ok = reject_unknown(doc, "", kTopLevelKeys); !ok) return ok;

  std::string root;
  if (auto present = read_string(doc, "sandbox_root", "sandbox_root", root); !present) {
    return std::unexpected(present.error());
  } else if (*present) {
    // A relative path in a file is anchored to the file's own directory. That is the
    // only anchor with a meaning to whoever is editing it -- resolving against the
    // working directory would make the same file mean different things depending on
    // where the binary was launched, which is R1's failure wearing a different hat.
    const std::filesystem::path written{root};
    config.sandbox_root = written.is_absolute() ? written : base / written;
    config.set_origin(Field::SandboxRoot, ConfigSource::File);
  }

  if (auto present = read_string(doc, "model", "model", config.model); !present) {
    return std::unexpected(present.error());
  } else if (*present) {
    config.set_origin(Field::Model, ConfigSource::File);
  }

  if (const auto it = doc.find("ollama"); it != doc.end()) {
    if (!it->is_object()) return std::unexpected(wrong_type("ollama", "an object"));
    if (auto ok = reject_unknown(*it, "ollama", kOllamaKeys); !ok) return ok;

    if (auto p = read_string(*it, "base_url", "ollama.base_url", config.client.base_url); !p) {
      return std::unexpected(p.error());
    } else if (*p) {
      config.set_origin(Field::BaseUrl, ConfigSource::File);
    }
    if (auto p = read_seconds(*it, "connect_timeout_s", "ollama.connect_timeout_s",
                              config.client.connect_timeout);
        !p) {
      return std::unexpected(p.error());
    } else if (*p) {
      config.set_origin(Field::ConnectTimeout, ConfigSource::File);
    }
    if (auto p = read_seconds(*it, "metadata_timeout_s", "ollama.metadata_timeout_s",
                              config.client.metadata_timeout);
        !p) {
      return std::unexpected(p.error());
    } else if (*p) {
      config.set_origin(Field::MetadataTimeout, ConfigSource::File);
    }
    if (auto p =
            read_seconds(*it, "chat_timeout_s", "ollama.chat_timeout_s", config.client.chat_timeout);
        !p) {
      return std::unexpected(p.error());
    } else if (*p) {
      config.set_origin(Field::ChatTimeout, ConfigSource::File);
    }
    if (auto p = read_uint(*it, "max_num_ctx", "ollama.max_num_ctx", config.client.max_num_ctx);
        !p) {
      return std::unexpected(p.error());
    } else if (*p) {
      config.set_origin(Field::MaxNumCtx, ConfigSource::File);
    }
  }

  if (const auto it = doc.find("preflight"); it != doc.end()) {
    if (!it->is_object()) return std::unexpected(wrong_type("preflight", "an object"));
    if (auto ok = reject_unknown(*it, "preflight", kPreflightKeys); !ok) return ok;

    if (auto p = read_uint(*it, "minimum_context", "preflight.minimum_context",
                           config.preflight.minimum_context);
        !p) {
      return std::unexpected(p.error());
    } else if (*p) {
      config.set_origin(Field::MinimumContext, ConfigSource::File);
    }
    if (auto p =
            read_bool(*it, "require_tools", "preflight.require_tools", config.preflight.require_tools);
        !p) {
      return std::unexpected(p.error());
    } else if (*p) {
      config.set_origin(Field::RequireTools, ConfigSource::File);
    }
    if (auto p = read_bool(*it, "warmup", "preflight.warmup", config.preflight.warmup); !p) {
      return std::unexpected(p.error());
    } else if (*p) {
      config.set_origin(Field::Warmup, ConfigSource::File);
    }
  }

  if (const auto it = doc.find("shell"); it != doc.end()) {
    if (!it->is_object()) return std::unexpected(wrong_type("shell", "an object"));
    if (auto ok = reject_unknown(*it, "shell", kShellKeys); !ok) return ok;

    if (auto p = read_bool(*it, "enabled", "shell.enabled", config.shell.enabled); !p) {
      return std::unexpected(p.error());
    } else if (*p) {
      config.set_origin(Field::ShellEnabled, ConfigSource::File);
    }
    if (auto p = read_seconds(*it, "timeout_s", "shell.timeout_s", config.shell.timeout); !p) {
      return std::unexpected(p.error());
    } else if (*p) {
      config.set_origin(Field::ShellTimeout, ConfigSource::File);
    }
  }

  // Shape only. Whether "exists:../x" names something reachable is a question for a
  // sandbox, which this layer deliberately does not have -- see Config::expectations.
  if (const auto it = doc.find("expectations"); it != doc.end()) {
    if (!it->is_array()) return std::unexpected(wrong_type("expectations", "an array"));
    config.expectations = *it;
    config.set_origin(Field::Expectations, ConfigSource::File);
  }
  if (const auto it = doc.find("unjudged"); it != doc.end()) {
    if (!it->is_number_unsigned()) {
      return std::unexpected(wrong_type("unjudged", "a non-negative whole number"));
    }
    config.unjudged_requirements = it->get<std::size_t>();
    config.set_origin(Field::Unjudged, ConfigSource::File);
  }

  return {};
}

}  // namespace

ConfigResult<void> apply_json(Config& config, const nlohmann::json& doc,
                              const std::filesystem::path& base) {
  // Commit or nothing. A file whose ninth key is a typo must not leave the first eight
  // applied: the caller is told the overlay failed, and a half-applied config would
  // make that report false. `load` happens to discard the whole thing on error, so
  // this is for every other caller -- and for the day `load` stops doing that.
  Config staged = config;
  if (auto ok = overlay_json(staged, doc, base); !ok) return ok;
  config = std::move(staged);
  return {};
}

// --- environment ------------------------------------------------------------

namespace {

/// The body of `apply_env`. Mutates as it goes; called on a copy, as `overlay_json` is.
ConfigResult<void> overlay_env(Config& config, const EnvLookup& lookup) {
  if (const auto value = lookup("HERMIT_SANDBOX_ROOT")) {
    if (value->empty()) {
      return std::unexpected(
          ConfigProblem{ConfigError::BadValue, "HERMIT_SANDBOX_ROOT is set but empty"});
    }
    // No anchor exists for a relative value here. A flag is typed at launch, where the
    // working directory is whatever the operator is looking at; a file has its own
    // directory. An environment variable has neither, and picking one silently is how
    // an inferred root gets back in.
    const std::filesystem::path root{*value};
    if (!root.is_absolute()) {
      return std::unexpected(ConfigProblem{
          ConfigError::BadValue,
          "HERMIT_SANDBOX_ROOT must be an absolute path (got: " + *value +
              "). Nothing anchors a relative one -- use --root if you mean it relative to here."});
    }
    config.sandbox_root = root;
    config.set_origin(Field::SandboxRoot, ConfigSource::Environment);
  }

  if (const auto value = lookup("HERMIT_MODEL")) {
    if (value->empty()) {
      return std::unexpected(ConfigProblem{ConfigError::BadValue, "HERMIT_MODEL is set but empty"});
    }
    config.model = *value;
    config.set_origin(Field::Model, ConfigSource::Environment);
  }

  if (const auto value = lookup("HERMIT_OLLAMA_URL")) {
    if (value->empty()) {
      return std::unexpected(
          ConfigProblem{ConfigError::BadValue, "HERMIT_OLLAMA_URL is set but empty"});
    }
    config.client.base_url = *value;
    config.set_origin(Field::BaseUrl, ConfigSource::Environment);
  }

  if (const auto value = lookup("HERMIT_MAX_NUM_CTX")) {
    const auto parsed = parse_whole(*value, "HERMIT_MAX_NUM_CTX");
    if (!parsed) return std::unexpected(parsed.error());
    config.client.max_num_ctx = *parsed;
    config.set_origin(Field::MaxNumCtx, ConfigSource::Environment);
  }

  return {};
}

}  // namespace

ConfigResult<void> apply_env(Config& config, const EnvLookup& lookup) {
  // Commit or nothing, as `apply_json` is: `HERMIT_SANDBOX_ROOT=/srv` with a broken
  // `HERMIT_MODEL` must not return an error while leaving the root applied -- a caller
  // that reported the failure and carried on would be carrying a root it was just
  // told not to trust.
  Config staged = config;
  if (auto ok = overlay_env(staged, lookup); !ok) return ok;
  config = std::move(staged);
  return {};
}

// --- flags ------------------------------------------------------------------

ConfigResult<std::optional<std::string_view>> find_config_flag(
    std::span<const std::string_view> args) {
  std::optional<std::string_view> found;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view token = args[i];
    if (token == "--") break;  // everything after is a path, including one spelled --config

    if (token != "--config") {
      // Step over another flag's value, so a `--config` sitting in a value position is
      // not mistaken for a request to load a file. Without this, `--model --config
      // x.json` reads x.json while apply_flags reads the same line as a model named
      // "--config" -- one command line, two parsers, two answers.
      //
      // Unconditionally, including over a flag-shaped token. Skipping only
      // *well-formed* values was tried and is wrong: on `--model --config x.json` it
      // declines to step over `--config`, then reads it as a request to load x.json --
      // which is the divergence this skip exists to prevent.
      //
      // The apparent cost is that a malformed line can still be read differently here
      // than by `apply_flags`. `load` removes it by parsing the flags once before this
      // runs, so a bad command line is reported before any file is opened.
      if (takes_a_value(token)) ++i;
      continue;
    }

    if (i + 1 >= args.size()) {
      return std::unexpected(
          ConfigProblem{ConfigError::MissingFlagValue, "--config expects a path"});
    }
    if (looks_like_a_flag(args[i + 1])) {
      return std::unexpected(ConfigProblem{
          ConfigError::MissingFlagValue,
          "--config expects a path, got: " + std::string{args[i + 1]}});
    }
    if (args[i + 1].empty()) {
      return std::unexpected(
          ConfigProblem{ConfigError::BadValue, "--config must not be empty"});
    }
    // Last one wins, matching every other flag here.
    found = args[++i];
  }
  return found;
}

std::span<const std::string_view> flags_taking_a_value() noexcept {
  return kFlagsTakingAValue;
}

namespace {

/// The body of `apply_flags`. Mutates as it goes; called on a copy.
///
/// `positional` is *not* staged. It is an out-parameter the caller is told to ignore
/// on error, and staging it would mean copying a vector on every successful parse to
/// tidy up a case nobody reads.
ConfigResult<void> overlay_flags(Config& config, std::span<const std::string_view> args,
                                 std::vector<std::string_view>& positional) {
  bool everything_is_positional = false;
  // `--expect` is repeatable, so the flag layer accumulates within one call and then
  // replaces the file's set as a unit. Appending instead would leave a stale expectation
  // in a config file unremovable from the command line, which is the opposite of what
  // every other flag here does.
  bool expectations_replaced = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view flag = args[i];

    // `--` ends the options. Without it a path beginning with a dash is unnameable,
    // which is a real gap in a tool whose whole job is resolving paths somebody else
    // chose the names of.
    if (!everything_is_positional && flag == "--") {
      everything_is_positional = true;
      continue;
    }
    if (everything_is_positional || !flag.starts_with("-")) {
      positional.push_back(flag);
      continue;
    }

    const bool has_value = i + 1 < args.size();
    const auto take = [&]() -> ConfigResult<std::string_view> {
      // The drift guard, and the reason `kFlagsTakingAValue` can be trusted. A new
      // value-taking flag wired into the chain below but not added to the table would
      // otherwise leave `find_config_flag` treating its value as a separate token --
      // re-creating exactly the `--model --config x.json` divergence. Here that is a
      // loud failure the first time the flag is used.
      if (!takes_a_value(flag)) {
        return std::unexpected(ConfigProblem{
            ConfigError::UnknownFlag,
            std::string{flag} + " consumes a value but is missing from kFlagsTakingAValue, "
                                "so the config-file scan would mis-read this command line"});
      }
      if (!has_value) {
        return std::unexpected(ConfigProblem{ConfigError::MissingFlagValue,
                                             std::string{flag} + " expects a value"});
      }
      // A flag where a value belongs is a forgotten argument, not a value that happens
      // to start with two dashes. Silently eating it shifts every later token by one
      // and turns a typo into a confusing complaint about something else.
      if (looks_like_a_flag(args[i + 1])) {
        return std::unexpected(ConfigProblem{
            ConfigError::MissingFlagValue,
            std::string{flag} + " expects a value, got: " + std::string{args[i + 1]}});
      }
      return args[++i];
    };
    const auto take_whole = [&]() -> ConfigResult<std::uint64_t> {
      const auto raw = take();
      if (!raw) return std::unexpected(raw.error());
      return parse_whole(*raw, flag);
    };
    const auto take_seconds = [&]() -> ConfigResult<std::chrono::seconds> {
      const auto value = take_whole();
      if (!value) return std::unexpected(value.error());
      if (*value == 0) {
        return std::unexpected(ConfigProblem{ConfigError::BadValue,
                                             std::string{flag} + " must be at least 1 second"});
      }
      if (*value > kMaxTimeoutSeconds) {
        return std::unexpected(ConfigProblem{
            ConfigError::BadValue, std::string{flag} + " must be at most " +
                                       std::to_string(kMaxTimeoutSeconds) + " seconds (one day)"});
      }
      return std::chrono::seconds{static_cast<std::chrono::seconds::rep>(*value)};
    };

    if (flag == "--config") {
      // Already applied -- it selects the file the other sources sit on top of. Still
      // parsed here so its path is consumed rather than falling through as a
      // positional, and so it fails exactly the way find_config_flag does.
      const auto value = take();
      if (!value) return std::unexpected(value.error());
      if (value->empty()) {
        return std::unexpected(ConfigProblem{ConfigError::BadValue, "--config must not be empty"});
      }
    } else if (flag == "--root") {
      const auto value = take();
      if (!value) return std::unexpected(value.error());
      if (value->empty()) {
        return std::unexpected(ConfigProblem{ConfigError::BadValue, "--root must not be empty"});
      }
      config.sandbox_root = std::filesystem::path{*value};
      config.set_origin(Field::SandboxRoot, ConfigSource::Flag);
    } else if (flag == "--model") {
      const auto value = take();
      if (!value) return std::unexpected(value.error());
      if (value->empty()) {
        return std::unexpected(ConfigProblem{ConfigError::BadValue, "--model must not be empty"});
      }
      config.model = std::string{*value};
      config.set_origin(Field::Model, ConfigSource::Flag);
    } else if (flag == "--url") {
      const auto value = take();
      if (!value) return std::unexpected(value.error());
      if (value->empty()) {
        return std::unexpected(ConfigProblem{ConfigError::BadValue, "--url must not be empty"});
      }
      config.client.base_url = std::string{*value};
      config.set_origin(Field::BaseUrl, ConfigSource::Flag);
    } else if (flag == "--max-num-ctx") {
      const auto value = take_whole();
      if (!value) return std::unexpected(value.error());
      config.client.max_num_ctx = *value;
      config.set_origin(Field::MaxNumCtx, ConfigSource::Flag);
    } else if (flag == "--min-context") {
      const auto value = take_whole();
      if (!value) return std::unexpected(value.error());
      config.preflight.minimum_context = *value;
      config.set_origin(Field::MinimumContext, ConfigSource::Flag);
    } else if (flag == "--connect-timeout") {
      const auto value = take_seconds();
      if (!value) return std::unexpected(value.error());
      config.client.connect_timeout = *value;
      config.set_origin(Field::ConnectTimeout, ConfigSource::Flag);
    } else if (flag == "--metadata-timeout") {
      const auto value = take_seconds();
      if (!value) return std::unexpected(value.error());
      config.client.metadata_timeout = *value;
      config.set_origin(Field::MetadataTimeout, ConfigSource::Flag);
    } else if (flag == "--chat-timeout") {
      const auto value = take_seconds();
      if (!value) return std::unexpected(value.error());
      config.client.chat_timeout = *value;
      config.set_origin(Field::ChatTimeout, ConfigSource::Flag);
    } else if (flag == "--warmup") {
      config.preflight.warmup = true;
      config.set_origin(Field::Warmup, ConfigSource::Flag);
    } else if (flag == "--no-warmup") {
      config.preflight.warmup = false;
      config.set_origin(Field::Warmup, ConfigSource::Flag);
    } else if (flag == "--expect") {
      const auto value = take();
      if (!value) return std::unexpected(value.error());
      if (value->empty()) {
        return std::unexpected(ConfigProblem{ConfigError::BadValue, "--expect must not be empty"});
      }
      if (!expectations_replaced) {
        config.expectations = nlohmann::json::array();
        expectations_replaced = true;
      }
      config.expectations.push_back(std::string{*value});
      config.set_origin(Field::Expectations, ConfigSource::Flag);
    } else if (flag == "--unjudged") {
      const auto value = take_whole();
      if (!value) return std::unexpected(value.error());
      config.unjudged_requirements = *value;
      config.set_origin(Field::Unjudged, ConfigSource::Flag);
    } else if (flag == "--tools") {
      config.preflight.require_tools = true;
      config.set_origin(Field::RequireTools, ConfigSource::Flag);
    } else if (flag == "--no-tools") {
      config.preflight.require_tools = false;
      config.set_origin(Field::RequireTools, ConfigSource::Flag);
    } else if (flag == "--shell") {
      config.shell.enabled = true;
      config.set_origin(Field::ShellEnabled, ConfigSource::Flag);
    } else if (flag == "--no-shell") {
      config.shell.enabled = false;
      config.set_origin(Field::ShellEnabled, ConfigSource::Flag);
    } else if (flag == "--shell-timeout") {
      const auto value = take_seconds();
      if (!value) return std::unexpected(value.error());
      config.shell.timeout = *value;
      config.set_origin(Field::ShellTimeout, ConfigSource::Flag);
    } else {
      return std::unexpected(
          ConfigProblem{ConfigError::UnknownFlag, std::string{flag}});
    }
  }
  return {};
}

}  // namespace

ConfigResult<void> apply_flags(Config& config, std::span<const std::string_view> args,
                               std::vector<std::string_view>& positional) {
  Config staged = config;
  if (auto ok = overlay_flags(staged, args, positional); !ok) return ok;
  config = std::move(staged);
  return {};
}

// --- validation -------------------------------------------------------------

ConfigResult<void> validate(const Config& config, const Requirements& required) {
  if (required.sandbox_root && config.sandbox_root.empty()) {
    return std::unexpected(ConfigProblem{
        ConfigError::MissingRequired,
        "no sandbox root. Pass --root DIR, set HERMIT_SANDBOX_ROOT, or put \"sandbox_root\" in a "
        "config file. There is deliberately no default: an inferred root is R1's original bug."});
  }
  if (required.model && config.model.empty()) {
    return std::unexpected(ConfigProblem{
        ConfigError::MissingRequired,
        "no model. Pass --model NAME, set HERMIT_MODEL, or put \"model\" in a config file."});
  }

  // D7's loopback rule, asked of the one function that implements it. Restating the
  // check here would create a second answer to "is this URL local?", and that question
  // is what keeps sandbox file contents on this machine.
  //
  // Only for commands that will actually dial it, though. A URL nothing opens cannot
  // carry anything off the machine, so checking it unconditionally would break `resolve`
  // -- which has no network at all -- over an `HERMIT_OLLAMA_URL` exported for an
  // unrelated tool, and would stop `config` printing the very value that is wrong.
  // `render()` marks it instead, so it is still impossible to miss.
  if (required.ollama) {
    if (const auto ok = ollama::validate_base_url(config.client.base_url); !ok) {
      return std::unexpected(ConfigProblem{ConfigError::BadValue, ok.error().detail});
    }
  }

  if (config.client.max_num_ctx == 0) {
    return std::unexpected(ConfigProblem{
        ConfigError::BadValue,
        "max_num_ctx of 0 would clamp every request to a zero-length context window"});
  }

  // A backstop, not the primary check -- the overlays reject these with better
  // messages. It exists because a timeout is the one setting that fails *open*: a
  // negative or absurd value does not shorten the wait, it removes it, and this is the
  // last place a Config assembled some other way can be caught.
  const auto bounded = [](std::chrono::seconds value, std::string_view name)
      -> ConfigResult<void> {
    if (value <= std::chrono::seconds::zero()) {
      return std::unexpected(ConfigProblem{ConfigError::BadValue,
                                           std::string{name} + " must be at least 1 second"});
    }
    if (value > std::chrono::seconds{static_cast<std::chrono::seconds::rep>(kMaxTimeoutSeconds)}) {
      return std::unexpected(ConfigProblem{
          ConfigError::BadValue, std::string{name} + " must be at most " +
                                     std::to_string(kMaxTimeoutSeconds) + " seconds (one day)"});
    }
    return {};
  };
  if (auto ok = bounded(config.client.connect_timeout, "connect_timeout"); !ok) return ok;
  if (auto ok = bounded(config.client.metadata_timeout, "metadata_timeout"); !ok) return ok;
  if (auto ok = bounded(config.client.chat_timeout, "chat_timeout"); !ok) return ok;
  if (auto ok = bounded(config.shell.timeout, "shell.timeout_s"); !ok) return ok;

  return {};
}

// --- rendering --------------------------------------------------------------

std::string Config::render() const {
  std::ostringstream out;

  const auto line = [&](std::string_view name, const std::string& value, Field field) {
    out << "  " << name;
    for (std::size_t pad = name.size(); pad < 26; ++pad) out << ' ';
    out << value;
    // At least one space, always. Padding alone runs a long value straight into its
    // origin tag -- an absolute sandbox root is routinely past the column, and
    // "/srv/some/deep/path[file]" reads as part of the path.
    out << ' ';
    for (std::size_t pad = value.size() + 1; pad < 34; ++pad) out << ' ';
    out << "[" << to_string(origin(field)) << "]\n";
  };

  out << "config:\n";
  // Tagged like everything else: --config and HERMIT_CONFIG are not interchangeable
  // when you are working out why a setting you cannot see is in force.
  line("config file", config_file ? config_file->string() : std::string{"(none named)"},
       Field::ConfigFile);
  line("sandbox_root", sandbox_root.empty() ? std::string{"(unset)"} : sandbox_root.string(),
       Field::SandboxRoot);
  line("model", model.empty() ? std::string{"(unset)"} : model, Field::Model);
  line("ollama.base_url", client.base_url, Field::BaseUrl);
  line("ollama.connect_timeout_s", std::to_string(client.connect_timeout.count()),
       Field::ConnectTimeout);
  line("ollama.metadata_timeout_s", std::to_string(client.metadata_timeout.count()),
       Field::MetadataTimeout);
  line("ollama.chat_timeout_s", std::to_string(client.chat_timeout.count()), Field::ChatTimeout);
  line("ollama.max_num_ctx", std::to_string(client.max_num_ctx), Field::MaxNumCtx);
  line("preflight.minimum_context", std::to_string(preflight.minimum_context),
       Field::MinimumContext);
  line("preflight.require_tools", preflight.require_tools ? "true" : "false", Field::RequireTools);
  line("preflight.warmup", preflight.warmup ? "true" : "false", Field::Warmup);
  line("expectations", expectations.empty() ? std::string{"(none stated)"}
                                            : std::to_string(expectations.size()) + " stated",
       Field::Expectations);
  line("unjudged", std::to_string(unjudged_requirements), Field::Unjudged);
  line("shell.enabled", shell.enabled ? "true" : "false", Field::ShellEnabled);
  line("shell.timeout_s", std::to_string(shell.timeout.count()), Field::ShellTimeout);

  // Printed as written rather than as parsed, which is the useful form here: this command
  // exists to show an operator which value is in force, and a spec that will be *rejected*
  // for naming an unreachable path is exactly the one worth seeing. Parsing would need a
  // sandbox this command does not require -- `hermit config` must still print with no
  // --root at all.
  for (const auto& expectation : expectations) {
    out << "      " << (expectation.is_string() ? expectation.get<std::string>()
                                                : expectation.dump())
        << '\n';
  }

  // Values that are legal, in force, and worth saying out loud. None of these is
  // rejected -- an operator who means them should be able to have them -- but none of
  // them should be able to be in force without appearing on screen.
  const ollama::ClientOptions defaults;
  if (client.max_num_ctx > defaults.max_num_ctx) {
    out << "\n  ⚠ max_num_ctx is above the " << defaults.max_num_ctx
        << " default. Ollama does no admission control on context size, and an\n"
           "    oversized request took this machine down on 2026-08-13 (DECISIONS.md, D8).\n"
           "    This number is only safe against a specific card with the VRAM to hold it.\n";
  }
  if (const auto local = ollama::validate_base_url(client.base_url); !local) {
    out << "\n  ⚠ " << local.error().detail
        << ".\n    Printed rather than refused so this command can still show you the value;\n"
           "    any command that opens a connection rejects it (DECISIONS.md, D7).\n";
  }
  if (preflight.minimum_context == 0) {
    out << "\n  ⚠ preflight.minimum_context is 0, so R9's context gate admits every model,\n"
           "    including the ones measured as unusable (llama3-groq-tool-use:8b).\n";
  }
  if (!preflight.require_tools) {
    out << "\n  ⚠ preflight.require_tools is off, so a model with no `tools` capability will\n"
           "    pass preflight and then fail on its first tool call.\n";
  }
  if (preflight.minimum_context > client.max_num_ctx) {
    out << "\n  note: models are gated at " << preflight.minimum_context
        << " context but requests are clamped to " << client.max_num_ctx
        << ".\n    Legal -- big model, small window, to fit the card -- but rarely intended.\n";
  }
  if (shell.enabled) {
    out << "\n  ⚠ shell is enabled: the model can run arbitrary commands, confined by the\n"
           "    kernel (D10) to the sandbox root, bounded to " << shell.timeout.count()
        << "s per call (R8).\n"
           "    This flag alone does not register the tool -- startup also requires a live\n"
           "    confinement probe to report Enforced, checked once, here, never assumed from\n"
           "    the platform (ROUTING.md section 8; DECISIONS.md, D11).\n"
           "    Landlock governs the filesystem only -- a confined command still reaches the\n"
           "    network and pathname unix sockets (D10). Containment bounds what shell can\n"
           "    touch; it says nothing about what shell can send.\n";
  }

  return out.str();
}

// --- the composed thing -----------------------------------------------------

namespace {

/// Read a config file, without any path reaching `std::terminate`.
///
/// **`std::ifstream` opens a directory successfully on Linux.** `!file` is false, and
/// libstdc++ then throws `std::ios_base::failure` out of `basic_filebuf::underflow` on
/// the first read -- from inside the streambuf, where the stream's exception mask does
/// not gate it. `istreambuf_iterator` propagates it, `main` has no handler, and
/// `hermit config --config /etc` aborts with a core dump. Pointing at a config
/// *directory* instead of the file inside it is an ordinary slip, and losing the
/// process over it contradicts the rule this file states about malformed input being
/// a message to print.
///
/// So: require a regular file up front, and keep the try/catch anyway. The check gives
/// the better message; the catch is what makes "no exception escapes" true rather than
/// enumerated, since a FIFO, a device node or an I/O error can each throw from a
/// different place.
ConfigResult<std::string> read_whole_file(const std::filesystem::path& path) {
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (ec) {
    return std::unexpected(
        ConfigProblem{ConfigError::FileUnreadable, path.string() + ": " + ec.message()});
  }
  if (status.type() == std::filesystem::file_type::directory) {
    return std::unexpected(ConfigProblem{
        ConfigError::FileUnreadable,
        path.string() + " is a directory, not a config file"});
  }
  if (status.type() != std::filesystem::file_type::regular) {
    return std::unexpected(ConfigProblem{ConfigError::FileUnreadable,
                                         path.string() + " is not a regular file"});
  }

  try {
    std::ifstream file{path};
    if (!file) {
      return std::unexpected(ConfigProblem{ConfigError::FileUnreadable, path.string()});
    }
    std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (file.bad()) {
      return std::unexpected(
          ConfigProblem{ConfigError::FileUnreadable, path.string() + ": read failed"});
    }
    return text;
  } catch (const std::exception& e) {
    return std::unexpected(
        ConfigProblem{ConfigError::FileUnreadable, path.string() + ": " + e.what()});
  }
}

}  // namespace

ConfigResult<std::optional<std::string>> select_config_file(std::span<const std::string_view> args,
                                                            const EnvLookup& lookup) {
  const auto from_flag = find_config_flag(args);
  if (!from_flag) return std::unexpected(from_flag.error());
  if (*from_flag) return std::optional<std::string>{std::string{**from_flag}};

  if (const auto from_env = lookup("HERMIT_CONFIG")) {
    // Set-but-empty is an error, exactly as it is for HERMIT_MODEL and the rest.
    // `export HERMIT_CONFIG=` is somebody trying to say something, and this is the
    // variable that selects every other setting -- the worst one to guess about.
    if (from_env->empty()) {
      return std::unexpected(
          ConfigProblem{ConfigError::BadValue, "HERMIT_CONFIG is set but empty"});
    }
    // Absolute, for the same reason HERMIT_SANDBOX_ROOT must be. A relative value
    // resolved against the working directory makes *which file is read* depend on
    // where the binary was launched -- and that file may then set its own relative
    // `sandbox_root`, anchored to whichever directory happened to win. An inferred
    // root, arriving by a longer route, from the one source with no honest anchor.
    if (!std::filesystem::path{*from_env}.is_absolute()) {
      return std::unexpected(ConfigProblem{
          ConfigError::BadValue,
          "HERMIT_CONFIG must be an absolute path (got: " + *from_env +
              "). Nothing anchors a relative one -- use --config if you mean it relative to "
              "here."});
    }
    return std::optional<std::string>{*from_env};
  }

  return std::optional<std::string>{};
}

ConfigResult<Config> load(std::span<const std::string_view> args, const Requirements& required,
                          std::vector<std::string_view>& positional) {
  Config config;  // every origin starts at ConfigSource::Default, which is value 0
  positional.clear();  // append-only would accumulate across two calls on one vector

  const EnvLookup real_env = [](std::string_view name) -> std::optional<std::string> {
    const char* const value = std::getenv(std::string{name}.c_str());
    if (value == nullptr) return std::nullopt;
    return std::string{value};
  };

  // Parse the flags once against a throwaway config, purely to surface a bad command
  // line before any file is opened.
  //
  // Without this the errors come out in the wrong order and blame the wrong thing:
  // `--frobnicate --config y.json` read y.json and then reported the unknown flag,
  // and `--model -- --config /nope.json` reported a missing file for a `--config`
  // that is actually behind the `--` terminator. Every such line still failed, so no
  // wrong config was ever built -- but the message named something the operator had
  // not got wrong, and a file was read on the strength of a line already known to be
  // invalid.
  {
    Config scratch;
    std::vector<std::string_view> ignored;
    if (auto ok = apply_flags(scratch, args, ignored); !ok) return std::unexpected(ok.error());
  }

  // The file is located before anything is applied, because it sits underneath the
  // environment and the flags. A flag beats HERMIT_CONFIG for the same reason.
  const auto from_flag = find_config_flag(args);
  if (!from_flag) return std::unexpected(from_flag.error());
  const ConfigSource chosen_origin = *from_flag ? ConfigSource::Flag : ConfigSource::Environment;

  const auto chosen = select_config_file(args, real_env);
  if (!chosen) return std::unexpected(chosen.error());

  if (*chosen) {
    std::error_code ec;
    // Absolute *before* it is opened, because this path is what a relative
    // `sandbox_root` inside the file anchors to. Failing to make it absolute is
    // therefore an error rather than something to shrug at and carry on from: the
    // fallback would leave `base` empty and silently re-anchor the file's root to the
    // working directory -- an inferred root arriving from the one source that is
    // supposed to be incapable of producing one.
    const std::filesystem::path path = std::filesystem::absolute(**chosen, ec);
    if (ec) {
      return std::unexpected(ConfigProblem{
          ConfigError::FileUnreadable,
          **chosen + ": could not be resolved against the working directory: " + ec.message()});
    }

    const auto text = read_whole_file(path);
    if (!text) return std::unexpected(text.error());

    // allow_exceptions=false: a malformed config file is a message to print, not a
    // C++ exception escaping to the operator (D2 records the same choice for replies).
    const auto doc = nlohmann::json::parse(*text, nullptr, false);
    if (doc.is_discarded()) {
      return std::unexpected(
          ConfigProblem{ConfigError::MalformedJson, path.string()});
    }

    if (auto ok = apply_json(config, doc, path.parent_path()); !ok) {
      return std::unexpected(
          ConfigProblem{ok.error().kind, path.string() + ": " + ok.error().detail});
    }
    config.config_file = path;
    config.set_origin(Field::ConfigFile, chosen_origin);
  }

  if (auto ok = apply_env(config, real_env); !ok) return std::unexpected(ok.error());
  if (auto ok = apply_flags(config, args, positional); !ok) return std::unexpected(ok.error());

  // A relative root can only have arrived from a flag -- the file anchored its own and
  // the environment rejects one. Make it absolute here rather than leaving it for
  // Sandbox::open, so `render()` shows the directory that will actually be used
  // instead of the two characters somebody typed.
  if (!config.sandbox_root.empty() && !config.sandbox_root.is_absolute()) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(config.sandbox_root, ec);
    if (ec) {
      return std::unexpected(ConfigProblem{
          ConfigError::BadValue, "could not resolve the sandbox root " +
                                     config.sandbox_root.string() +
                                     " against the working directory: " + ec.message()});
    }
    config.sandbox_root = absolute;
  }

  if (auto ok = validate(config, required); !ok) return std::unexpected(ok.error());
  return config;
}

}  // namespace hermit::app
