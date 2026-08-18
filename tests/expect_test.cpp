#include <hermit/app/expect.h>

#include <gtest/gtest.h>

#include <hermit/supervisor/verify.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using hermit::Sandbox;
using hermit::app::ExpectError;
using hermit::app::parse_expectation;
using hermit::app::parse_expectations_json;
using hermit::supervisor::Expectation;
using hermit::supervisor::ExpectationKind;

namespace {

namespace fs = std::filesystem;

void write_file(const fs::path& p, std::string_view bytes) {
  std::ofstream out(p, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// <tmp>/root/notes.txt
// <tmp>/root/..notes            a legal filename that begins with two dots
// <tmp>/root/report=final.md    a legal filename containing '='
// <tmp>/root/existing/          a directory
// <tmp>/root/link.md   -> notes.txt          stays inside
// <tmp>/root/escape.md -> <tmp>/outside/...  leaves the root
class ExpectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermit_exp_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));

    root_ = tmp_ / "root";
    fs::create_directories(root_ / "existing");
    fs::create_directories(tmp_ / "outside");

    write_file(root_ / "notes.txt", "notes");
    write_file(root_ / "..notes", "dotdot");
    write_file(root_ / "report=final.md", "eq");
    write_file(tmp_ / "outside" / "secret.txt", "secret");

    fs::create_directories(root_ / "a" / "b");
    fs::create_directory_symlink("a/b", root_ / "dirlink");
    fs::create_symlink("notes.txt", root_ / "link.md");
    fs::create_symlink(tmp_ / "outside" / "secret.txt", root_ / "escape.md");

    auto sb = Sandbox::open(root_);
    ASSERT_TRUE(sb.has_value());
    sandbox_ = std::make_unique<Sandbox>(std::move(*sb));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  /// The parsed expectation, or a gtest failure naming why it was refused.
  Expectation ok(std::string_view spec) {
    auto parsed = parse_expectation(spec, *sandbox_);
    EXPECT_TRUE(parsed.has_value())
        << spec << " was refused: " << (parsed ? "" : parsed.error().render());
    return parsed ? *parsed : Expectation{};
  }

  /// The production path: repeated `--expect` flags are collected into exactly this array
  /// by the config layer, so a test that went through a separate specs entry point would be
  /// exercising something no command actually runs.
  std::expected<hermit::app::Expectations, ExpectError> parse_set(
      std::vector<std::string> specs) {
    nlohmann::json settings = nlohmann::json::object();
    settings["expectations"] = std::move(specs);
    return parse_expectations_json(settings, *sandbox_);
  }

  std::string refusal(std::string_view spec) {
    auto parsed = parse_expectation(spec, *sandbox_);
    EXPECT_FALSE(parsed.has_value()) << spec << " was accepted and should not have been";
    return parsed ? std::string{} : parsed.error().message;
  }

  fs::path tmp_;
  fs::path root_;
  std::unique_ptr<Sandbox> sandbox_;
};

// --- the grammar ------------------------------------------------------------

TEST_F(ExpectTest, TheFiveKindsParse) {
  EXPECT_EQ(ok("exists:notes.txt"), Expectation::exists("notes.txt"));
  EXPECT_EQ(ok("dir:existing"), Expectation::exists("existing", true));
  EXPECT_EQ(ok("absent:notes.txt"), Expectation::absent("notes.txt"));
  EXPECT_EQ(ok("preserved:notes.txt=moved.txt"),
            Expectation::preserved("notes.txt", "moved.txt"));
  EXPECT_EQ(ok("identical:notes.txt=copy.txt"),
            Expectation::identical("notes.txt", "copy.txt"));
}

TEST_F(ExpectTest, DeclarationOrderIsPreserved) {
  // judge.h leans on this: R7 restates the *first* unmet finding, so a set written in
  // dependency order has to stay in that order all the way through.
  auto set = parse_set({"dir:out", "exists:out/index.md", "absent:notes.txt"});
  ASSERT_TRUE(set.has_value()) << set.error().render();
  ASSERT_EQ(set->set.size(), 3U);
  EXPECT_EQ(set->set[0].path, "out");
  EXPECT_EQ(set->set[1].path, "out/index.md");
  EXPECT_EQ(set->set[2].path, "notes.txt");
}

// --- rule 2: the key is the literal spelling --------------------------------

TEST_F(ExpectTest, ASymlinkIsKeyedByItsOwnNameNotItsTarget) {
  // The regression this file exists for. `resolve("link.md")` returns relative
  // "notes.txt", and keying on it would answer a question about notes.txt while the
  // operator asked about link.md -- silently, and with a hash to make it look verified.
  // The snapshot walker lstats, so "link.md" is its own key and must stay one.
  EXPECT_EQ(ok("exists:link.md").path, "link.md");
  EXPECT_EQ(ok("absent:link.md").path, "link.md");
}

TEST_F(ExpectTest, SpellingsThatNameTheSameFileNormaliseToTheSnapshotKey) {
  EXPECT_EQ(ok("exists:./notes.txt").path, "notes.txt");
  EXPECT_EQ(ok("exists:a/./b").path, "a/b");
}

TEST_F(ExpectTest, DotDotIsRefusedBecauseItResolvesPastASymlinkAndTheKeyDoesNot) {
  // The bug this rule closes, kept as a live demonstration rather than a claim. The
  // containment gate accepts "dirlink/../x" -- it resolves POSIX-order to a/x, which is
  // inside the root -- while folding ".." textually gives "x". Two different files, one
  // of them silently keyed. Neither half is wrong on its own; using them together is.
  auto resolved = sandbox_->resolve("dirlink/../x");
  ASSERT_TRUE(resolved.has_value()) << "the gate is expected to accept this";
  EXPECT_EQ(resolved->relative(), fs::path{"a/x"});
  EXPECT_EQ(fs::path{"dirlink/../x"}.lexically_normal(), fs::path{"x"});

  const auto expected = "contains \"..\"; expectations name paths as spelled from the root";
  EXPECT_EQ(refusal("exists:dirlink/../x"), expected);
  EXPECT_EQ(refusal("exists:existing/../notes.txt"), expected);
  EXPECT_EQ(refusal("exists:../outside/secret.txt"), expected);
  EXPECT_EQ(refusal("exists:existing/.."), expected);

  // The component test is whole-component, so a filename that merely starts with two
  // dots is untouched.
  EXPECT_EQ(ok("exists:..notes").path, "..notes");
}

TEST_F(ExpectTest, ATrailingSeparatorIsStripped) {
  // lexically_normal keeps "existing/" as "existing/", and no snapshot key has one.
  EXPECT_EQ(ok("dir:existing/").path, "existing");
}

TEST_F(ExpectTest, AFilenameBeginningWithTwoDotsIsNotAnEscape) {
  // A prefix test on ".." would refuse this; the check compares whole components.
  EXPECT_EQ(ok("exists:..notes").path, "..notes");
}

TEST_F(ExpectTest, AOneSidedKindTakesAnEqualsAsPartOfThePath) {
  EXPECT_EQ(ok("exists:report=final.md").path, "report=final.md");
}

// --- the key actually matches the walker's, checked against a real snapshot -------

TEST_F(ExpectTest, EveryParsedKeyIsOneTheWalkerActuallyEmits) {
  // The load-bearing claim of this whole file, and the only test that can fail if
  // expect.cpp and verify.cpp ever disagree about how a path is spelled. Asserting the
  // parser returns "notes.txt" proves nothing on its own -- the snapshot has to contain
  // it. A mismatch here is invisible at runtime: it surfaces as a permanent `Unmet`.
  hermit::supervisor::TreeVerifier verifier(*sandbox_);
  auto snapshot = verifier.snapshot();
  ASSERT_TRUE(snapshot.has_value());

  for (const std::string spec : {"exists:notes.txt", "exists:./notes.txt",
                                 "dir:existing", "dir:a/b",
                                 "dir:existing/", "exists:..notes",
                                 "exists:report=final.md", "exists:link.md",
                                 "exists:notes.txt/"}) {
    auto parsed = parse_expectation(spec, *sandbox_);
    ASSERT_TRUE(parsed.has_value()) << spec << ": " << parsed.error().render();
    EXPECT_TRUE(snapshot->contains(parsed->path))
        << spec << " parsed to \"" << parsed->path << "\", which the walker never emits";
  }
}

TEST_F(ExpectTest, TheWalkerKeepsASymlinkAndItsTargetApart) {
  // What makes the retarget above a real bug rather than a stylistic one: both names are
  // separate entries, so resolving link.md to notes.txt would have picked the wrong one
  // of two keys that are simultaneously present.
  hermit::supervisor::TreeVerifier verifier(*sandbox_);
  auto snapshot = verifier.snapshot();
  ASSERT_TRUE(snapshot.has_value());

  ASSERT_TRUE(snapshot->contains("link.md"));
  ASSERT_TRUE(snapshot->contains("notes.txt"));
  EXPECT_NE(snapshot->at("link.md"), snapshot->at("notes.txt"));
}

TEST_F(ExpectTest, ALinkPointingOutOfTheRootIsInTheSnapshotButCannotBeNamed) {
  // The asymmetry expect.h admits to, pinned so it stays a decision. The walker records
  // the link (it lstats, it does not follow), but rule 1 refuses to name it -- so
  // "absent:escape.md" cannot be stated even though the fact is observable.
  hermit::supervisor::TreeVerifier verifier(*sandbox_);
  auto snapshot = verifier.snapshot();
  ASSERT_TRUE(snapshot.has_value());

  EXPECT_TRUE(snapshot->contains("escape.md"));
  EXPECT_FALSE(refusal("absent:escape.md").empty());
}

// --- rule 1: containment ----------------------------------------------------

TEST_F(ExpectTest, PathsThatDoNotExistYetAreAccepted) {
  // The common case: "create this". Refusing it would make the parser useless.
  EXPECT_EQ(ok("exists:missing.md").path, "missing.md");
  EXPECT_EQ(ok("exists:sub/report.md").path, "sub/report.md");
  EXPECT_EQ(ok("exists:deep/a/b/c.md").path, "deep/a/b/c.md");
}

TEST_F(ExpectTest, EscapesAreRefused) {
  // A symlink out of the root carries no ".." and normalises cleanly, so only the
  // containment gate catches it. This is the case rule 1 is load-bearing for.
  EXPECT_FALSE(refusal("exists:escape.md").empty());
}

TEST_F(ExpectTest, AnAbsolutePathIsRefusedEvenInsideTheRoot) {
  const std::string spec = "exists:" + (root_ / "notes.txt").string();
  EXPECT_EQ(refusal(spec), "absolute path; expectations are relative to --root");
}

TEST_F(ExpectTest, TheRootItselfIsRefused) {
  const auto expected = "names the sandbox root, which has no entry in a snapshot";
  EXPECT_EQ(refusal("exists:."), expected);
  EXPECT_EQ(refusal("dir:./"), expected);
}

// --- malformed input --------------------------------------------------------

TEST_F(ExpectTest, MalformedSpecsAreRefusedWithTheCallersOwnText) {
  EXPECT_EQ(refusal(""), "empty expectation");
  EXPECT_EQ(refusal("notes.txt"),
            "no kind; expected exists:, dir:, absent:, preserved: or identical:");
  EXPECT_EQ(refusal("exists:"), "empty path");
  EXPECT_EQ(refusal("preserved:notes.txt"), "preserved needs two paths, written FROM=TO");
  EXPECT_EQ(refusal("identical:notes.txt="), "second path: empty path");
  EXPECT_FALSE(refusal("exsits:notes.txt").empty());

  auto parsed = parse_expectation("exsits:notes.txt", *sandbox_);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().spec, "exsits:notes.txt");
  EXPECT_EQ(parsed.error().render(),
            "expectation \"exsits:notes.txt\": unknown kind \"exsits\"; expected exists, "
            "dir, absent, preserved or identical");
}

TEST_F(ExpectTest, TheFirstRefusalStopsTheWholeSet) {
  auto set = parse_set({"exists:notes.txt", "exists:../escape", "dir:existing"});
  ASSERT_FALSE(set.has_value());
  EXPECT_EQ(set.error().spec, "exists:../escape");
}

// --- the config-file form ---------------------------------------------------

TEST_F(ExpectTest, JsonAcceptsBothTheCompactAndObjectForms) {
  const auto settings = nlohmann::json::parse(R"({
    "expectations": [
      "exists:index.md",
      {"kind": "exists", "path": "archive", "as_directory": true},
      {"kind": "preserved", "path": "notes.txt", "other": "notes.txt"}
    ]
  })");
  auto parsed = parse_expectations_json(settings, *sandbox_);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().render();
  ASSERT_EQ(parsed->set.size(), 3U);
  EXPECT_EQ(parsed->set[0], Expectation::exists("index.md"));
  EXPECT_EQ(parsed->set[1], Expectation::exists("archive", true));
  EXPECT_EQ(parsed->set[2], Expectation::preserved("notes.txt", "notes.txt"));
  EXPECT_EQ(parsed->unjudged, 0U);
}

TEST_F(ExpectTest, TheObjectFormIsTheEscapeHatchForPathsContainingEquals) {
  // The claim expect.h makes about the two-sided flag grammar, pinned. A second '=' is
  // refused rather than split at the first, because splitting silently yields a path
  // nobody named -- and an unmet finding on it is what R7 would restate to the model.
  EXPECT_EQ(refusal("identical:report=final.md=copy.md"),
            "identical has more than one '=', so which one splits the pair is a guess; "
            "state it in the object form instead");

  const auto settings = nlohmann::json::parse(R"({
    "expectations": [
      {"kind": "identical", "path": "report=final.md", "other": "notes.txt"}
    ]
  })");
  auto parsed = parse_expectations_json(settings, *sandbox_);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().render();
  EXPECT_EQ(parsed->set[0], Expectation::identical("report=final.md", "notes.txt"));
}

TEST_F(ExpectTest, UnjudgedIsReadFromBesideTheArray) {
  // It is a sibling key, so reading the array alone would drop it -- and a verdict with
  // an unjudged requirement silently missing reads as success.
  const auto settings = nlohmann::json::parse(R"({
    "expectations": ["exists:index.md"],
    "unjudged": 2
  })");
  auto parsed = parse_expectations_json(settings, *sandbox_);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().render();
  EXPECT_EQ(parsed->unjudged, 2U);
  EXPECT_EQ(parsed->set.size(), 1U);
}

TEST_F(ExpectTest, AbsentSettingsAreNotAnError) {
  auto parsed = parse_expectations_json(nlohmann::json::object(), *sandbox_);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().render();
  EXPECT_TRUE(parsed->empty());
}

TEST_F(ExpectTest, JsonShapeErrorsAreRefused) {
  const auto bad = std::vector<std::string>{
      R"({"expectations": "exists:x"})",
      R"({"expectations": [42]})",
      R"({"expectations": [{"path": "x"}]})",
      R"({"expectations": [{"kind": "exists"}]})",
      R"({"expectations": [{"kind": "exists", "path": "x", "other": "y"}]})",
      R"({"expectations": [{"kind": "preserved", "path": "x"}]})",
      R"({"expectations": [{"kind": "absent", "path": "x", "as_directory": true}]})",
      R"({"expectations": [{"kind": "exists", "path": "x", "as_directory": "yes"}]})",
      R"({"unjudged": -1})",
      R"({"unjudged": "two"})",
  };
  for (const auto& text : bad) {
    auto parsed = parse_expectations_json(nlohmann::json::parse(text), *sandbox_);
    EXPECT_FALSE(parsed.has_value()) << text << " was accepted and should not have been";
  }
}


// --- sets no run could satisfy ----------------------------------------------

TEST_F(ExpectTest, ASetThatContradictsItselfIsRefused) {
  // Each line parses. Together they cannot hold, and nothing downstream would say so:
  // the verdict is Unmet every turn, so R7 restates the same impossible failure until a
  // bound stops the run.
  auto set = parse_set({"exists:notes.txt", "absent:notes.txt"});
  ASSERT_FALSE(set.has_value());
  EXPECT_EQ(set.error().spec, "notes.txt");
  EXPECT_NE(set.error().message.find("no tree can satisfy both"), std::string::npos);
}

TEST_F(ExpectTest, EveryPredicateThatNeedsAPathPresentContradictsAbsent) {
  const auto refused = [&](std::vector<std::string> specs) {
    return !parse_set(std::move(specs)).has_value();
  };
  EXPECT_TRUE(refused({"absent:notes.txt", "exists:notes.txt"})) << "order does not matter";
  EXPECT_TRUE(refused({"dir:existing", "absent:existing"}));
  EXPECT_TRUE(refused({"preserved:notes.txt=copy.txt", "absent:copy.txt"}))
      << "preserved requires its destination";
  EXPECT_TRUE(refused({"preserved:notes.txt=notes.txt", "absent:notes.txt"}))
      << "the self form requires the path it protects";
  EXPECT_TRUE(refused({"identical:notes.txt=copy.txt", "absent:notes.txt"}))
      << "identical reads both sides from the current tree";
  EXPECT_TRUE(refused({"identical:notes.txt=copy.txt", "absent:copy.txt"}));
}

TEST_F(ExpectTest, APathCannotBeRequiredToBeADirectoryAndToHoldBytes) {
  // The second axis, and the reason the first alone was not enough. judge.cpp refuses a
  // destination that `holds_no_bytes`, so `dir:b` beside `preserved:a=b` is exactly as
  // unsatisfiable as `exists:x` beside `absent:x` -- it just fails on kind instead of
  // existence, which a rule written around presence alone sails straight past.
  const auto refused = [&](std::vector<std::string> specs) {
    return !parse_set(std::move(specs)).has_value();
  };
  EXPECT_TRUE(refused({"dir:archive", "preserved:notes.txt=archive"}));
  EXPECT_TRUE(refused({"preserved:notes.txt=archive", "dir:archive"})) << "order does not matter";
  EXPECT_TRUE(refused({"dir:existing", "identical:existing=notes.txt"}));
  EXPECT_TRUE(refused({"dir:existing", "identical:notes.txt=existing"}));
  EXPECT_TRUE(refused({"dir:notes.txt", "preserved:notes.txt=notes.txt"}))
      << "the self form still requires bytes at the path it protects";

  auto message = parse_set({"dir:archive", "preserved:notes.txt=archive"});
  ASSERT_FALSE(message.has_value());
  EXPECT_EQ(message.error().spec, "archive");
  EXPECT_NE(message.error().message.find("a directory holds no bytes"), std::string::npos);
}

TEST_F(ExpectTest, APlainExistsBesideABytesPredicateIsFine) {
  // `exists` without `as_directory` means "present", not "present and not a directory",
  // so it constrains nothing about kind and must not trip the check.
  auto set = parse_set({"exists:archive", "preserved:notes.txt=archive"});
  ASSERT_TRUE(set.has_value()) << set.error().render();
  EXPECT_EQ(set->set.size(), 2u);
}

TEST_F(ExpectTest, AMoveIsNotAContradiction) {
  // The case a careless rule would break, and the reason the check is built on what
  // judge.cpp actually reads rather than on which paths an expectation mentions.
  // `preserved:a=b` finds `a` in the BASELINE and constrains only `b` in the current
  // tree, so saying `a` is gone afterwards is the ordinary description of a move.
  auto set = parse_set({"preserved:notes.txt=archive.txt", "absent:notes.txt"});
  ASSERT_TRUE(set.has_value()) << set.error().render();
  EXPECT_EQ(set->set.size(), 2u);
}

TEST_F(ExpectTest, RedundancyIsNotContradiction) {
  // Stating a path twice, or once loosely and once as a directory, is satisfiable --
  // `exists` without `as_directory` means "present", not "present and not a directory".
  // Refusing these would reject sets that are merely verbose.
  auto set = parse_set({"exists:existing", "dir:existing", "exists:existing"});
  ASSERT_TRUE(set.has_value()) << set.error().render();
  EXPECT_EQ(set->set.size(), 3u);
}

TEST_F(ExpectTest, TheJsonFormIsCheckedForContradictionsToo) {
  // Both entry points, or a config file becomes the way to smuggle in a set the flags
  // would have refused.
  const auto settings = nlohmann::json::parse(R"({
    "expectations": ["exists:report.md", {"kind": "absent", "path": "report.md"}]
  })");
  auto parsed = parse_expectations_json(settings, *sandbox_);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().spec, "report.md");
}

}  // namespace
