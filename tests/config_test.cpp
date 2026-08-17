#include <hermit/app/config.h>

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using hermit::app::apply_env;
using hermit::app::apply_flags;
using hermit::app::apply_json;
using hermit::app::Config;
using hermit::app::ConfigError;
using hermit::app::ConfigSource;
using hermit::app::Field;
using hermit::app::find_config_flag;
using hermit::app::parse_whole;
using hermit::app::Requirements;
using hermit::app::select_config_file;
using hermit::app::validate;

namespace {

/// Every test here runs against the pure overlays rather than `load`, so none of them
/// touches a filesystem, an environment, or a daemon. That is the reason the pieces
/// are exposed separately: precedence and key names are the part worth pinning down,
/// and they are exactly the part that a test needing $HOME would test badly.

/// A fake environment. `apply_env` takes a lookup rather than calling getenv so a test
/// can state precisely which variables exist -- including the difference between unset
/// and set-to-empty, which the two are treated differently.
hermit::app::EnvLookup env_of(std::map<std::string, std::string, std::less<>> vars) {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    const auto found = vars.find(name);
    if (found == vars.end()) return std::nullopt;
    return found->second;
  };
}

std::vector<std::string_view> flags(std::initializer_list<std::string_view> args) {
  return std::vector<std::string_view>{args};
}

/// apply_flags with the positional vector a caller would otherwise have to declare.
hermit::app::ConfigResult<void> apply(Config& config,
                                      std::initializer_list<std::string_view> args) {
  const auto owned = flags(args);
  std::vector<std::string_view> positional;
  return apply_flags(config, owned, positional);
}

nlohmann::json parse(std::string_view text) {
  return nlohmann::json::parse(text, nullptr, false);
}

constexpr std::string_view kBase = "/etc/hermit";

}  // namespace

// --- defaults ----------------------------------------------------------------

TEST(Config, TheSandboxRootHasNoDefault) {
  // R1's whole argument: any default here would be an inferred root. Not the working
  // directory, not $HOME, not a git root -- nothing.
  const Config config;
  EXPECT_TRUE(config.sandbox_root.empty());
  EXPECT_EQ(config.origin(Field::SandboxRoot), ConfigSource::Default);
}

TEST(Config, TheModelHasNoDefault) {
  const Config config;
  EXPECT_TRUE(config.model.empty());
}

TEST(Config, TheClampDefaultsToTheValueD8Records) {
  // 65536 is a safety limit, not a tuning default. If this number ever changes,
  // DECISIONS.md D8 has to change with it.
  EXPECT_EQ(Config{}.client.max_num_ctx, 65536u);
}

TEST(Config, TheContextFloorDefaultsToR9s) {
  EXPECT_EQ(Config{}.preflight.minimum_context, hermit::ollama::kMinimumContext);
  EXPECT_TRUE(Config{}.preflight.require_tools);
  EXPECT_FALSE(Config{}.preflight.warmup);
}

// --- precedence --------------------------------------------------------------

TEST(Config, FlagsBeatEnvironmentBeatsFile) {
  Config config;
  ASSERT_TRUE(apply_json(config, parse(R"({"model": "from-file"})"), kBase));
  EXPECT_EQ(config.model, "from-file");
  EXPECT_EQ(config.origin(Field::Model), ConfigSource::File);

  ASSERT_TRUE(apply_env(config, env_of({{"HERMIT_MODEL", "from-env"}})));
  EXPECT_EQ(config.model, "from-env");
  EXPECT_EQ(config.origin(Field::Model), ConfigSource::Environment);

  ASSERT_TRUE(apply(config, {"--model", "from-flag"}));
  EXPECT_EQ(config.model, "from-flag");
  EXPECT_EQ(config.origin(Field::Model), ConfigSource::Flag);
}

TEST(Config, ASourceThatSaysNothingOverwritesNothing) {
  // The overlays are field-by-field, not whole-object replacement. A config file that
  // sets only the model must not reset the URL somebody exported.
  Config config;
  ASSERT_TRUE(apply_env(config, env_of({{"HERMIT_OLLAMA_URL", "http://127.0.0.1:9999"}})));
  ASSERT_TRUE(apply(config, {"--model", "qwen"}));

  EXPECT_EQ(config.client.base_url, "http://127.0.0.1:9999");
  EXPECT_EQ(config.origin(Field::BaseUrl), ConfigSource::Environment);
  EXPECT_EQ(config.origin(Field::Model), ConfigSource::Flag);
}

TEST(Config, TheLastOccurrenceOfAFlagWins) {
  Config config;
  ASSERT_TRUE(apply(config, {"--model", "first", "--model", "second"}));
  EXPECT_EQ(config.model, "second");
}

// --- the config file ---------------------------------------------------------

TEST(Config, EveryDocumentedKeyIsRead) {
  // Guards against a key that exists in the unknown-key list but is never actually
  // applied -- which would accept the setting and silently ignore it, the exact
  // failure the fail-closed rule exists to prevent.
  Config config;
  ASSERT_TRUE(apply_json(config, parse(R"({
    "sandbox_root": "/srv/work",
    "model": "qwen35-agent",
    "ollama": {
      "base_url": "http://127.0.0.1:11434",
      "connect_timeout_s": 5,
      "metadata_timeout_s": 45,
      "chat_timeout_s": 900,
      "max_num_ctx": 32768
    },
    "preflight": {"minimum_context": 131072, "require_tools": false, "warmup": true}
  })"),
                         kBase));

  EXPECT_EQ(config.sandbox_root, std::filesystem::path{"/srv/work"});
  EXPECT_EQ(config.model, "qwen35-agent");
  EXPECT_EQ(config.client.base_url, "http://127.0.0.1:11434");
  EXPECT_EQ(config.client.connect_timeout.count(), 5);
  EXPECT_EQ(config.client.metadata_timeout.count(), 45);
  EXPECT_EQ(config.client.chat_timeout.count(), 900);
  EXPECT_EQ(config.client.max_num_ctx, 32768u);
  EXPECT_EQ(config.preflight.minimum_context, 131072u);
  EXPECT_FALSE(config.preflight.require_tools);
  EXPECT_TRUE(config.preflight.warmup);

  for (const Field field : {Field::SandboxRoot, Field::Model, Field::BaseUrl,
                            Field::ConnectTimeout, Field::MetadataTimeout, Field::ChatTimeout,
                            Field::MaxNumCtx, Field::MinimumContext, Field::RequireTools,
                            Field::Warmup}) {
    EXPECT_EQ(config.origin(field), ConfigSource::File);
  }
}

TEST(Config, AnUnknownKeyIsRejectedRatherThanIgnored) {
  // A typo'd max_num_ctx that is silently dropped leaves the D8 clamp at its default
  // while the operator believes they raised it.
  Config config;
  const auto result = apply_json(config, parse(R"({"sandbox_roots": "/srv/work"})"), kBase);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::UnknownKey);
  // The message has to name the alternatives, or a typo costs a trip to the source.
  EXPECT_NE(result.error().detail.find("sandbox_root"), std::string::npos);
}

TEST(Config, AnUnknownKeyIsRejectedInsideNestedObjectsToo) {
  Config config;
  const auto result = apply_json(config, parse(R"({"ollama": {"max_ctx": 1}})"), kBase);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::UnknownKey);
  EXPECT_NE(result.error().detail.find("ollama.max_ctx"), std::string::npos);
}

TEST(Config, ARelativeRootInAFileAnchorsToTheFilesOwnDirectory) {
  // The only anchor that means anything to whoever is editing the file. Resolving
  // against the working directory would make one file mean different things depending
  // on where the binary was launched.
  Config config;
  ASSERT_TRUE(apply_json(config, parse(R"({"sandbox_root": "work"})"), "/etc/hermit"));
  EXPECT_EQ(config.sandbox_root, std::filesystem::path{"/etc/hermit/work"});
}

TEST(Config, AnAbsoluteRootInAFileIsLeftAlone) {
  Config config;
  ASSERT_TRUE(apply_json(config, parse(R"({"sandbox_root": "/srv/work"})"), "/etc/hermit"));
  EXPECT_EQ(config.sandbox_root, std::filesystem::path{"/srv/work"});
}

TEST(Config, TheTopLevelMustBeAnObject) {
  Config config;
  const auto result = apply_json(config, parse("[1, 2, 3]"), kBase);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::NotAnObject);
}

TEST(Config, AKeyOfTheWrongTypeIsRejected) {
  Config config;
  const auto result = apply_json(config, parse(R"({"model": 7})"), kBase);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::WrongType);
}

TEST(Config, ANestedObjectOfTheWrongTypeIsRejected) {
  Config config;
  const auto result = apply_json(config, parse(R"({"preflight": "yes please"})"), kBase);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::WrongType);
}

TEST(Config, ANegativeNumberIsRejectedRatherThanWrappingToEnormous) {
  // -1 read as a signed integer and converted to uint64 is 18446744073709551615, which
  // passes every range check below as a very large positive clamp. Read as unsigned it
  // is simply the wrong type.
  Config config;
  const auto result = apply_json(config, parse(R"({"ollama": {"max_num_ctx": -1}})"), kBase);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::WrongType);
  EXPECT_EQ(config.client.max_num_ctx, 65536u);
}

TEST(Config, AZeroTimeoutIsRejected) {
  Config config;
  const auto result = apply_json(config, parse(R"({"ollama": {"chat_timeout_s": 0}})"), kBase);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
}

TEST(Config, AnEmptyStringIsRejected) {
  Config config;
  const auto result = apply_json(config, parse(R"({"model": ""})"), kBase);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
}

// --- the environment ---------------------------------------------------------

TEST(Config, TheEnvironmentSetsThePerMachineValues) {
  Config config;
  ASSERT_TRUE(apply_env(config, env_of({{"HERMIT_SANDBOX_ROOT", "/srv/work"},
                                        {"HERMIT_MODEL", "qwen35-agent"},
                                        {"HERMIT_OLLAMA_URL", "http://localhost:11500"},
                                        {"HERMIT_MAX_NUM_CTX", "131072"}})));
  EXPECT_EQ(config.sandbox_root, std::filesystem::path{"/srv/work"});
  EXPECT_EQ(config.model, "qwen35-agent");
  EXPECT_EQ(config.client.base_url, "http://localhost:11500");
  EXPECT_EQ(config.client.max_num_ctx, 131072u);
}

TEST(Config, ARelativeRootInTheEnvironmentIsRejected) {
  // Nothing anchors it. A flag is typed at launch and a file has its own directory;
  // an environment variable has neither, and picking an anchor silently is how an
  // inferred root gets back in.
  Config config;
  const auto result = apply_env(config, env_of({{"HERMIT_SANDBOX_ROOT", "work"}}));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
  EXPECT_NE(result.error().detail.find("--root"), std::string::npos);
}

TEST(Config, AVariableSetToEmptyIsAnErrorRatherThanUnset) {
  // `export HERMIT_MODEL=` is somebody trying to say something. Treating it as unset
  // silently falls back to a different source, which is a surprise either way.
  Config config;
  const auto result = apply_env(config, env_of({{"HERMIT_MODEL", ""}}));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
}

TEST(Config, AnUnparseableClampInTheEnvironmentIsRejected) {
  Config config;
  const auto result = apply_env(config, env_of({{"HERMIT_MAX_NUM_CTX", "64k"}}));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
}

TEST(Config, TimeoutsAndPolicyAreDeliberatelyNotInTheEnvironment) {
  // Not an accident, and worth a test so it stays deliberate: an environment variable
  // is the easiest thing to set once in a shell profile and forget, and a long tail of
  // them is a long tail of settings quietly in force on one machine and not another --
  // the confound D3 exists to keep out.
  Config config;
  ASSERT_TRUE(apply_env(config, env_of({{"HERMIT_CHAT_TIMEOUT", "1"},
                                        {"HERMIT_WARMUP", "1"},
                                        {"HERMIT_MIN_CONTEXT", "1"}})));
  EXPECT_EQ(config.client.chat_timeout.count(), 600);
  EXPECT_FALSE(config.preflight.warmup);
  EXPECT_EQ(config.preflight.minimum_context, hermit::ollama::kMinimumContext);
}

// --- flags -------------------------------------------------------------------

TEST(Config, EveryFlagSetsItsField) {
  Config config;
  ASSERT_TRUE(apply(config, {"--root", "/srv/work", "--model", "qwen", "--url",
                             "http://127.0.0.1:11434", "--max-num-ctx", "32768", "--min-context",
                             "131072", "--connect-timeout", "3", "--metadata-timeout", "40",
                             "--chat-timeout", "120", "--warmup", "--no-tools"}));
  EXPECT_EQ(config.sandbox_root, std::filesystem::path{"/srv/work"});
  EXPECT_EQ(config.model, "qwen");
  EXPECT_EQ(config.client.base_url, "http://127.0.0.1:11434");
  EXPECT_EQ(config.client.max_num_ctx, 32768u);
  EXPECT_EQ(config.preflight.minimum_context, 131072u);
  EXPECT_EQ(config.client.connect_timeout.count(), 3);
  EXPECT_EQ(config.client.metadata_timeout.count(), 40);
  EXPECT_EQ(config.client.chat_timeout.count(), 120);
  EXPECT_TRUE(config.preflight.warmup);
  EXPECT_FALSE(config.preflight.require_tools);
}

TEST(Config, BothSidesOfEveryBooleanFlagExist) {
  // A file or the environment can turn something on, so a flag has to be able to turn
  // it back off -- otherwise the highest-precedence source cannot express "no".
  Config config;
  ASSERT_TRUE(apply_json(config, parse(R"({"preflight": {"warmup": true, "require_tools": false}})"),
                         kBase));
  ASSERT_TRUE(apply(config, {"--no-warmup", "--tools"}));
  EXPECT_FALSE(config.preflight.warmup);
  EXPECT_TRUE(config.preflight.require_tools);
}

TEST(Config, PositionalArgumentsAreCollectedRatherThanRejected) {
  Config config;
  const auto owned = flags({"--root", "/srv/work", "a.txt", "sub/b.txt"});
  std::vector<std::string_view> positional;
  ASSERT_TRUE(apply_flags(config, owned, positional));
  EXPECT_EQ(positional, (std::vector<std::string_view>{"a.txt", "sub/b.txt"}));
}

TEST(Config, AnUnknownFlagIsRejected) {
  Config config;
  const auto result = apply(config, {"--sandbox", "/srv/work"});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::UnknownFlag);
}

TEST(Config, AFlagMissingItsValueIsRejectedRatherThanConsumingNothing) {
  Config config;
  const auto result = apply(config, {"--model"});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::MissingFlagValue);
}

TEST(Config, AFlagValueIsNotMistakenForAPositional) {
  Config config;
  const auto owned = flags({"--model", "qwen"});
  std::vector<std::string_view> positional;
  ASSERT_TRUE(apply_flags(config, owned, positional));
  EXPECT_TRUE(positional.empty());
}

TEST(Config, TheConfigFlagIsConsumedWithItsValue) {
  // It was applied before flag parsing ran, so here it must only be skipped -- and its
  // path must not fall through into the positional list, where `resolve` would try to
  // resolve it as a sandbox path.
  Config config;
  const auto owned = flags({"--config", "/etc/hermit/hermit.json", "a.txt"});
  std::vector<std::string_view> positional;
  ASSERT_TRUE(apply_flags(config, owned, positional));
  EXPECT_EQ(positional, (std::vector<std::string_view>{"a.txt"}));
}

// --- locating the config file ------------------------------------------------

TEST(Config, TheConfigFlagIsFoundBeforeAnythingIsApplied) {
  const auto owned = flags({"--model", "qwen", "--config", "/etc/hermit.json"});
  const auto found = find_config_flag(owned);
  ASSERT_TRUE(found);
  ASSERT_TRUE(*found);
  EXPECT_EQ(**found, "/etc/hermit.json");
}

TEST(Config, NoConfigFlagIsNotAnError) {
  const auto owned = flags({"--model", "qwen"});
  const auto found = find_config_flag(owned);
  ASSERT_TRUE(found);
  EXPECT_FALSE(*found);
}

TEST(Config, TheLastConfigFlagWins) {
  const auto owned = flags({"--config", "a.json", "--config", "b.json"});
  const auto found = find_config_flag(owned);
  ASSERT_TRUE(found);
  ASSERT_TRUE(*found);
  EXPECT_EQ(**found, "b.json");
}

TEST(Config, AConfigFlagWithNoPathIsRejected) {
  const auto owned = flags({"--config"});
  const auto found = find_config_flag(owned);
  ASSERT_FALSE(found);
  EXPECT_EQ(found.error().kind, ConfigError::MissingFlagValue);
}

// --- flag values that look like flags, and the -- terminator -----------------
//
// All of this came out of review. `find_config_flag` and `apply_flags` are two parsers
// over one command line, and they have to agree about which tokens are values.

TEST(Config, AFlagWhereAValueBelongsIsAForgottenArgument) {
  // `--model --root /srv` is a missing model name, not a model called "--root".
  // Eating it would shift every later token by one and produce a complaint about
  // something the operator did not type.
  Config config;
  const auto result = apply(config, {"--model", "--root", "/srv/work"});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::MissingFlagValue);
  EXPECT_NE(result.error().detail.find("--model"), std::string::npos);
}

TEST(Config, TheTwoParsersAgreeAboutAConfigInAValuePosition) {
  // Before the fix this loaded x.json -- find_config_flag saw `--config x.json` and
  // read the file, while apply_flags read the same line as a model named "--config".
  // A config file the operator never asked for was applied.
  const auto owned = flags({"--model", "--config", "x.json"});
  const auto found = find_config_flag(owned);
  ASSERT_TRUE(found);
  EXPECT_FALSE(*found) << "a --config in a value position is not a request to load a file";

  Config config;
  const auto result = apply(config, {"--model", "--config", "x.json"});
  EXPECT_FALSE(result) << "and the flag parser must reject the same line";
}

TEST(Config, EveryValueTakingFlagIsInTheSharedTable) {
  // The drift guard, reading the *real* table rather than a third hand-kept copy of it
  // -- a copy could itself drift, which is the bug this test exists to prevent.
  //
  // This direction catches a boolean flag wrongly listed as value-taking, which would
  // make find_config_flag skip a live `--config` and quietly not load a named file.
  // The other direction -- a value-taking flag added to apply_flags but missing from
  // the table -- is caught inside take(), which refuses to run for an unlisted flag.
  for (const std::string_view flag : hermit::app::flags_taking_a_value()) {
    Config config;
    const auto alone = flags({flag});
    std::vector<std::string_view> unused;
    const auto result = apply_flags(config, alone, unused);
    ASSERT_FALSE(result) << flag << " should demand a value";
    EXPECT_EQ(result.error().kind, ConfigError::MissingFlagValue) << flag;

    // And find_config_flag must step over that value rather than reading it.
    const auto pair = flags({flag, "--config"});
    const auto found = find_config_flag(pair);
    if (flag == "--config") {
      EXPECT_FALSE(found) << "--config --config is a missing path";
    } else {
      ASSERT_TRUE(found) << flag;
      EXPECT_FALSE(*found) << flag << " must not have its value read as a config path";
    }
  }
}

TEST(Config, TheSharedTableHoldsExactlyTheNineValueTakingFlags) {
  // Pins the count so a flag silently dropped from the table is noticed even if no
  // other test happens to exercise it.
  EXPECT_EQ(hermit::app::flags_taking_a_value().size(), 9u);
}

TEST(Config, NoBooleanFlagIsListedAsValueTaking) {
  for (const std::string_view boolean : {"--warmup", "--no-warmup", "--tools", "--no-tools"}) {
    for (const std::string_view listed : hermit::app::flags_taking_a_value()) {
      EXPECT_NE(boolean, listed) << boolean << " must not consume the next argument";
    }
  }
}

TEST(Config, BooleanFlagsDoNotConsumeTheNextArgument) {
  Config config;
  const auto owned = flags({"--warmup", "a.txt"});
  std::vector<std::string_view> positional;
  ASSERT_TRUE(apply_flags(config, owned, positional));
  EXPECT_TRUE(config.preflight.warmup);
  EXPECT_EQ(positional, (std::vector<std::string_view>{"a.txt"}));
}

TEST(Config, DoubleDashEndsTheOptions) {
  // A path-resolution tool that cannot name a path beginning with a dash has a real
  // gap, and the model picks these names, not us.
  Config config;
  const auto owned = flags({"--root", "/srv/work", "--", "--not-a-flag", "-x", "--config"});
  std::vector<std::string_view> positional;
  ASSERT_TRUE(apply_flags(config, owned, positional));
  EXPECT_EQ(config.sandbox_root, std::filesystem::path{"/srv/work"});
  EXPECT_EQ(positional, (std::vector<std::string_view>{"--not-a-flag", "-x", "--config"}));
}

TEST(Config, DoubleDashHidesAConfigFlagFromBothParsers) {
  const auto owned = flags({"--", "--config", "x.json"});
  const auto found = find_config_flag(owned);
  ASSERT_TRUE(found);
  EXPECT_FALSE(*found);
}

TEST(Config, ABadCommandLineIsReportedBeforeAnyFileIsOpened) {
  // On a malformed line the two parsers can still read it differently -- find_config_flag
  // steps over `--model`'s value unconditionally, so it never sees the `--` here. `load`
  // settles it by parsing the flags once up front: the error names the flag the operator
  // actually got wrong, and no file is opened on the strength of a line already known to
  // be invalid.
  //
  // Goes through `load` rather than the overlays because the ordering *is* the
  // behaviour. It reads the real environment, which cannot change the outcome: the flag
  // error is raised before any source is consulted.
  const auto owned = flags({"--model", "--", "--config", "/nope/absent.json"});
  std::vector<std::string_view> positional;
  const auto result = hermit::app::load(owned, {}, positional);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::MissingFlagValue);
  EXPECT_NE(result.error().detail.find("--model"), std::string::npos);
  EXPECT_EQ(result.error().detail.find("absent.json"), std::string::npos)
      << "the message must not blame a file the line never asked for";
}

TEST(Config, AnUnknownFlagIsReportedBeforeAConfigFileIsRead) {
  const auto owned = flags({"--frobnicate", "--config", "/nope/absent.json"});
  std::vector<std::string_view> positional;
  const auto result = hermit::app::load(owned, {}, positional);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::UnknownFlag);
}

// --- selecting the config file -----------------------------------------------

TEST(Config, TheFlagBeatsTheEnvironmentWhenChoosingAFile) {
  const auto owned = flags({"--config", "from-flag.json"});
  const auto chosen = select_config_file(owned, env_of({{"HERMIT_CONFIG", "/etc/from-env.json"}}));
  ASSERT_TRUE(chosen);
  ASSERT_TRUE(*chosen);
  EXPECT_EQ(**chosen, "from-flag.json");
}

TEST(Config, TheEnvironmentSelectsAFileWhenNoFlagDoes) {
  const auto owned = flags({});
  const auto chosen = select_config_file(owned, env_of({{"HERMIT_CONFIG", "/etc/from-env.json"}}));
  ASSERT_TRUE(chosen);
  ASSERT_TRUE(*chosen);
  EXPECT_EQ(**chosen, "/etc/from-env.json");
}

TEST(Config, ARelativeHermitConfigIsRejected) {
  // The same rule as HERMIT_SANDBOX_ROOT, and it was missed when this variable was
  // first routed through the seam. A relative value makes *which file is read* depend
  // on the working directory -- and that file may set its own relative sandbox_root,
  // anchored to whichever directory won. An inferred root by a longer route.
  const auto owned = flags({});
  const auto chosen = select_config_file(owned, env_of({{"HERMIT_CONFIG", "rel.json"}}));
  ASSERT_FALSE(chosen);
  EXPECT_EQ(chosen.error().kind, ConfigError::BadValue);
  EXPECT_NE(chosen.error().detail.find("--config"), std::string::npos);
}

TEST(Config, ARelativeConfigFlagIsStillFineBecauseItWasTypedAtLaunch) {
  const auto owned = flags({"--config", "rel.json"});
  const auto chosen = select_config_file(owned, env_of({}));
  ASSERT_TRUE(chosen);
  ASSERT_TRUE(*chosen);
  EXPECT_EQ(**chosen, "rel.json");
}

TEST(Config, AnEmptyConfigPathIsRejectedByBothParsers) {
  const auto owned = flags({"--config", ""});
  EXPECT_FALSE(find_config_flag(owned));

  Config config;
  const auto result = apply(config, {"--config", ""});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
}

TEST(Config, AnEmptyHermitConfigIsAnErrorRatherThanNoFile) {
  // This was the fail-closed rule broken for the one variable that selects all the
  // others: `export HERMIT_CONFIG=` silently read no file and said nothing.
  const auto owned = flags({});
  const auto chosen = select_config_file(owned, env_of({{"HERMIT_CONFIG", ""}}));
  ASSERT_FALSE(chosen);
  EXPECT_EQ(chosen.error().kind, ConfigError::BadValue);
}

TEST(Config, NoFlagAndNoVariableSelectsNoFile) {
  const auto owned = flags({"--model", "qwen"});
  const auto chosen = select_config_file(owned, env_of({}));
  ASSERT_TRUE(chosen);
  EXPECT_FALSE(*chosen);
}

// --- timeouts fail open, so they are bounded at both ends --------------------

TEST(Config, AnOversizedTimeoutIsRejectedRatherThanWrappingNegative) {
  // seconds::rep is int64_t: 18446744073709551615 casts to -1. cpp-httplib then hands
  // a negative millisecond count to poll(2), where it means *block forever* -- so an
  // unbounded timeout does not lengthen the wait, it removes it, on the one setting
  // whose job is to bound a wedged daemon.
  Config config;
  const auto result = apply(config, {"--chat-timeout", "18446744073709551615"});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
  EXPECT_EQ(config.client.chat_timeout.count(), 600);
}

TEST(Config, AnOversizedTimeoutInAFileIsRejectedToo) {
  Config config;
  const auto result =
      apply_json(config, parse(R"({"ollama": {"chat_timeout_s": 18446744073709551615}})"), kBase);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
}

TEST(Config, ADayIsTheMostATimeoutMayBe) {
  Config config;
  EXPECT_TRUE(apply(config, {"--chat-timeout", "86400"}));
  EXPECT_EQ(config.client.chat_timeout.count(), 86400);

  Config too_long;
  EXPECT_FALSE(apply(too_long, {"--chat-timeout", "86401"}));
}

TEST(Config, ValidateBackstopsATimeoutSetAnotherWay) {
  // The overlays give the better messages, but a Config assembled directly must not
  // be able to carry a timeout that disables itself.
  Config config;
  config.client.chat_timeout = std::chrono::seconds{-1};
  const auto result = validate(config, {});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
}

// --- transactional overlay ---------------------------------------------------

TEST(Config, AFileThatFailsLateAppliesNothingAtAll) {
  // Commit or nothing: reporting "the overlay failed" while leaving half of it applied
  // would make the report false.
  Config config;
  const auto result = apply_json(config, parse(R"({
    "model": "qwen35-agent",
    "ollama": {"base_url": "http://127.0.0.1:9999"},
    "preflight": {"warmupp": true}
  })"),
                                 kBase);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::UnknownKey);
  EXPECT_TRUE(config.model.empty()) << "the earlier keys must not have survived the failure";
  EXPECT_EQ(config.client.base_url, "http://localhost:11434");
  EXPECT_EQ(config.origin(Field::Model), ConfigSource::Default);
}

TEST(Config, AFailedEnvironmentOverlayAppliesNothingAtAll) {
  // Same rule as the file overlay. This used to return an error and leave the sandbox
  // root applied -- a caller reporting the failure and carrying on would be carrying a
  // root it had just been told not to trust.
  Config config;
  const auto result = apply_env(
      config, env_of({{"HERMIT_SANDBOX_ROOT", "/srv/escaped"}, {"HERMIT_MODEL", ""}}));
  ASSERT_FALSE(result);
  EXPECT_TRUE(config.sandbox_root.empty());
  EXPECT_EQ(config.origin(Field::SandboxRoot), ConfigSource::Default);
}

TEST(Config, AFailedFlagOverlayAppliesNothingAtAll) {
  Config config;
  const auto result = apply(config, {"--root", "/srv/escaped", "--model"});
  ASSERT_FALSE(result);
  EXPECT_TRUE(config.sandbox_root.empty());
  EXPECT_EQ(config.origin(Field::SandboxRoot), ConfigSource::Default);
}

// --- validation --------------------------------------------------------------

TEST(Config, ACommandNeedingARootFailsWithoutOne) {
  Config config;
  const auto result = validate(config, {.sandbox_root = true, .model = false});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::MissingRequired);
  // The message has to say all three ways to supply it, and why there is no default.
  EXPECT_NE(result.error().detail.find("--root"), std::string::npos);
  EXPECT_NE(result.error().detail.find("HERMIT_SANDBOX_ROOT"), std::string::npos);
}

TEST(Config, ACommandNeedingNoRootDoesNotDemandOne) {
  // preflight has no sandbox. Demanding one would force an operator to invent a value
  // that is then never used.
  Config config;
  config.model = "qwen";
  EXPECT_TRUE(validate(config, {.sandbox_root = false, .model = true}));
}

TEST(Config, ACommandNeedingAModelFailsWithoutOne) {
  Config config;
  const auto result = validate(config, {.sandbox_root = false, .model = true});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::MissingRequired);
}

TEST(Config, ANonLoopbackUrlIsRejectedForACommandThatDialsIt) {
  // D7 is enforced by calling ollama::validate_base_url rather than restating it, so
  // there is exactly one answer to "is this URL local?" -- which is what keeps sandbox
  // file contents on this machine. This test is here to catch the wiring being dropped.
  Config config;
  config.client.base_url = "http://10.0.0.5:11434";
  config.model = "qwen";
  const auto result = validate(config, {.sandbox_root = false, .model = true, .ollama = true});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
}

TEST(Config, AUrlNothingOpensDoesNotBlockACommandThatNeverDialsIt) {
  // `resolve` is pure filesystem work. An HERMIT_OLLAMA_URL exported for some other
  // tool used to break it, and used to stop `config` printing the very value that was
  // wrong -- the one command whose job is to show you what is in force.
  Config config;
  config.client.base_url = "http://10.0.0.5:11434";
  config.sandbox_root = "/srv/work";
  EXPECT_TRUE(validate(config, {.sandbox_root = true, .model = false, .ollama = false}));
}

TEST(Config, ABadUrlIsCalledOutByRenderSinceValidateNoLongerBlocksIt) {
  // The other half of the bargain: not refusing to print means it must be impossible
  // to miss in what is printed.
  Config config;
  config.client.base_url = "http://10.0.0.5:11434";
  EXPECT_NE(config.render().find("⚠"), std::string::npos);
}

TEST(Config, AnHttpsUrlIsRejected) {
  Config config;
  config.client.base_url = "https://localhost:11434";
  config.model = "qwen";
  EXPECT_FALSE(validate(config, {.sandbox_root = false, .model = true, .ollama = true}));
}

TEST(Config, AZeroClampIsRejected) {
  Config config;
  config.client.max_num_ctx = 0;
  const auto result = validate(config, {});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
}

TEST(Config, ADefaultConfigWithNoRequirementsIsValid) {
  EXPECT_TRUE(validate(Config{}, {}));
}

// --- parse_whole -------------------------------------------------------------

TEST(Config, TrailingJunkIsNotAPartialParse) {
  // "64k" parsing as 64 would drop a safety limit by three orders of magnitude and
  // report success while doing it.
  EXPECT_FALSE(parse_whole("64k", "--max-num-ctx"));
  EXPECT_FALSE(parse_whole("1 2", "--max-num-ctx"));
  EXPECT_FALSE(parse_whole("", "--max-num-ctx"));
  EXPECT_FALSE(parse_whole("-1", "--max-num-ctx"));
  EXPECT_FALSE(parse_whole("0x10", "--max-num-ctx"));
}

TEST(Config, AValueTooLargeForUint64IsRejectedAndSaysSo) {
  // from_chars reports result_out_of_range and leaves the value untouched, so this was
  // never a wrong number -- but calling it "expects a whole number" sends the reader
  // hunting for a typo in a string that is entirely digits.
  const auto result = parse_whole("18446744073709551616", "--max-num-ctx");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, ConfigError::BadValue);
  EXPECT_NE(result.error().detail.find("too large"), std::string::npos);
}

TEST(Config, AWholeNumberParses) {
  const auto value = parse_whole("65536", "--max-num-ctx");
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 65536u);
}

TEST(Config, TheFlagNameAppearsInTheParseError) {
  const auto result = parse_whole("64k", "--max-num-ctx");
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().detail.find("--max-num-ctx"), std::string::npos);
}

// --- rendering ---------------------------------------------------------------
//
// render() is the surface that makes "nothing is silently in force" true, so what it
// does and does not say is behaviour rather than formatting.

TEST(Config, RenderNamesEverySettingAndItsOrigin) {
  Config config;
  ASSERT_TRUE(apply(config, {"--root", "/srv/work", "--model", "qwen"}));
  const std::string text = config.render();

  for (const std::string_view key :
       {"sandbox_root", "model", "ollama.base_url", "ollama.connect_timeout_s",
        "ollama.metadata_timeout_s", "ollama.chat_timeout_s", "ollama.max_num_ctx",
        "preflight.minimum_context", "preflight.require_tools", "preflight.warmup"}) {
    EXPECT_NE(text.find(key), std::string::npos) << key << " is missing from render()";
  }
  EXPECT_NE(text.find("[flag]"), std::string::npos);
  EXPECT_NE(text.find("[default]"), std::string::npos);
}

TEST(Config, RenderSaysWhenNoConfigFileWasNamed) {
  EXPECT_NE(Config{}.render().find("(none named)"), std::string::npos);
}

TEST(Config, ARaisedClampIsCalledOutRatherThanJustPrinted) {
  // D8 asks for a raised clamp to be deliberate, against a specific card. It is not
  // rejected -- an operator with the VRAM should be able to have it -- but it must not
  // be able to be in force without saying so.
  Config config;
  ASSERT_TRUE(apply(config, {"--max-num-ctx", "262144"}));
  EXPECT_NE(config.render().find("⚠"), std::string::npos);
}

TEST(Config, TheDefaultClampIsNotCalledOut) {
  EXPECT_EQ(Config{}.render().find("⚠"), std::string::npos);
}

TEST(Config, ADisabledContextGateIsCalledOut) {
  Config config;
  ASSERT_TRUE(apply(config, {"--min-context", "0"}));
  EXPECT_NE(config.render().find("⚠"), std::string::npos);
}

TEST(Config, ADisabledToolsGateIsCalledOut) {
  Config config;
  ASSERT_TRUE(apply(config, {"--no-tools"}));
  EXPECT_NE(config.render().find("⚠"), std::string::npos);
}

TEST(Config, GatingHigherThanTheClampIsNotedButAllowed) {
  // Legal and occasionally intended: a big model run at a small window to fit the
  // card. Noted rather than rejected, and validate() must still pass.
  Config config;
  ASSERT_TRUE(apply(config, {"--min-context", "131072", "--max-num-ctx", "32768"}));
  EXPECT_TRUE(validate(config, {}));
  EXPECT_NE(config.render().find("note:"), std::string::npos);
}
