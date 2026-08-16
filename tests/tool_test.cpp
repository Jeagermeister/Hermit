#include <hermes/core/tool.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using hermes::ArgErrorKind;
using hermes::ArgSpec;
using hermes::ArgType;
using hermes::parse_args;
using hermes::PathError;
using hermes::RawArg;
using hermes::RawArgs;
using hermes::RegistryErrorKind;
using hermes::Sandbox;
using hermes::SpecError;
using hermes::Tool;
using hermes::ToolArgs;
using hermes::ToolError;
using hermes::ToolOutput;
using hermes::ToolRegistry;
using hermes::ToolSpec;
using hermes::validate;

namespace {

// A minimal concrete tool: one required argument of each type, one optional
// String. run() echoes what it was handed, one row per resolved path, so the
// dispatch test can see exactly what crossed the interface.
constexpr std::array<ArgSpec, 4> kEchoArgs{{
    {.name = "pattern", .type = ArgType::String, .required = true, .doc = "free text"},
    {.name = "target", .type = ArgType::Path, .required = true, .doc = "one path"},
    {.name = "extras", .type = ArgType::PathList, .required = false, .doc = "more paths"},
    {.name = "note", .type = ArgType::String, .required = false, .doc = "optional text"},
}};
const ToolSpec kEchoSpec{"echo", "echoes its arguments back", kEchoArgs};

class EchoTool final : public Tool {
 public:
  [[nodiscard]] const ToolSpec& spec() const noexcept override { return kEchoSpec; }

  [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs& args) override {
    ToolOutput out;
    out.rows.push_back({{{"pattern", *args.string("pattern")},
                         {"target", args.path("target")->relative().string()}}});
    for (const auto& p : args.paths("extras")) {
      out.rows.push_back({{{"extra", p.relative().string()}}});
    }
    return out;
  }
};

// Layout built for every test:
//
//   <tmp>/root/            <- the sandbox root
//   <tmp>/root/inside.txt
//   <tmp>/root/sub/deep.txt
//   <tmp>/outside/         <- exists, so an escape fails on containment,
//                             not on a missing component
class ToolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermes_tool_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));

    fs::create_directories(tmp_ / "root" / "sub");
    fs::create_directories(tmp_ / "outside");
    write_file(tmp_ / "root" / "inside.txt", "in");
    write_file(tmp_ / "root" / "sub" / "deep.txt", "deep");

    auto sb = Sandbox::open(tmp_ / "root");
    ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
    box_ = std::make_unique<Sandbox>(std::move(*sb));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  static void write_file(const fs::path& p, std::string_view contents) {
    std::ofstream out(p);
    out << contents;
  }

  fs::path tmp_;
  std::unique_ptr<Sandbox> box_;
};

// --- spec validation ---------------------------------------------------------

TEST(ToolSpecValidate, AWellFormedSpecPasses) {
  EXPECT_TRUE(validate(kEchoSpec).has_value());
}

TEST(ToolSpecValidate, AnEmptyToolNameIsRefused) {
  const ToolSpec spec{"", "described", kEchoArgs};
  auto result = validate(spec);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SpecError::EmptyToolName);
}

TEST(ToolSpecValidate, AnEmptyArgumentNameIsRefused) {
  static constexpr std::array<ArgSpec, 1> args{{{.name = "", .doc = "unnamed"}}};
  const ToolSpec spec{"tool", "described", args};
  auto result = validate(spec);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SpecError::EmptyArgName);
}

TEST(ToolSpecValidate, TwoArgumentsSharingANameAreRefused) {
  static constexpr std::array<ArgSpec, 2> args{{
      {.name = "path", .doc = "first"},
      {.name = "path", .doc = "second"},
  }};
  const ToolSpec spec{"tool", "described", args};
  auto result = validate(spec);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SpecError::DuplicateArgName);
}

// --- parsing: the happy path -------------------------------------------------

TEST_F(ToolTest, AllThreeTypesParseAndResolve) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"inside.txt"}},
      {"extras", std::vector<std::string>{"sub/deep.txt", "inside.txt"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_TRUE(parsed.has_value()) << to_string(parsed.error());

  ASSERT_NE(parsed->string("pattern"), nullptr);
  EXPECT_EQ(*parsed->string("pattern"), "hello");

  ASSERT_NE(parsed->path("target"), nullptr);
  EXPECT_EQ(parsed->path("target")->path(), box_->root() / "inside.txt");
  EXPECT_EQ(parsed->path("target")->relative(), fs::path{"inside.txt"});

  auto extras = parsed->paths("extras");
  ASSERT_EQ(extras.size(), 2u);
  EXPECT_EQ(extras[0].path(), box_->root() / "sub" / "deep.txt");
  EXPECT_EQ(extras[1].path(), box_->root() / "inside.txt");
}

TEST_F(ToolTest, AnAbsentOptionalArgumentReadsAsNullAndEmpty) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"inside.txt"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_TRUE(parsed.has_value()) << to_string(parsed.error());

  EXPECT_EQ(parsed->string("note"), nullptr);
  EXPECT_TRUE(parsed->paths("extras").empty());
}

TEST_F(ToolTest, AccessorsForANameOutsideTheSpecReturnNull) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"inside.txt"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_TRUE(parsed.has_value());

  EXPECT_EQ(parsed->string("no_such_arg"), nullptr);
  EXPECT_EQ(parsed->path("no_such_arg"), nullptr);
  EXPECT_TRUE(parsed->paths("no_such_arg").empty());
}

TEST_F(ToolTest, AccessorsOfTheWrongTypeReturnNullRatherThanConverting) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"inside.txt"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_TRUE(parsed.has_value());

  // "target" parsed as a Path; asking for it as a string is a tool bug and
  // reads as absent, never as a silent stringification.
  EXPECT_EQ(parsed->string("target"), nullptr);
  EXPECT_EQ(parsed->path("pattern"), nullptr);
  EXPECT_TRUE(parsed->paths("target").empty());
}

// --- parsing: refusals -------------------------------------------------------

TEST_F(ToolTest, AMissingRequiredArgumentIsNamed) {
  const RawArgs raw{{"pattern", std::string{"hello"}}};
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().kind, ArgErrorKind::MissingRequired);
  EXPECT_EQ(parsed.error().arg, "target");
}

TEST_F(ToolTest, AnUnknownArgumentIsRefusedNotIgnored) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"inside.txt"}},
      {"tarrget", std::string{"typo"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().kind, ArgErrorKind::UnknownArg);
  EXPECT_EQ(parsed.error().arg, "tarrget");
}

TEST_F(ToolTest, ADuplicatedArgumentIsRefused) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"pattern", std::string{"again"}},
      {"target", std::string{"inside.txt"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().kind, ArgErrorKind::DuplicateArg);
  EXPECT_EQ(parsed.error().arg, "pattern");
}

TEST_F(ToolTest, AListWhereAScalarBelongsIsTheWrongShape) {
  const RawArgs raw{
      {"pattern", std::vector<std::string>{"hello"}},
      {"target", std::string{"inside.txt"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().kind, ArgErrorKind::WrongShape);
  EXPECT_EQ(parsed.error().arg, "pattern");
}

TEST_F(ToolTest, AScalarWhereAListBelongsIsTheWrongShape) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"inside.txt"}},
      {"extras", std::string{"sub/deep.txt"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().kind, ArgErrorKind::WrongShape);
  EXPECT_EQ(parsed.error().arg, "extras");
}

TEST_F(ToolTest, AnEmptyPathListIsARefusalNotAVacuousSuccess) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"inside.txt"}},
      {"extras", std::vector<std::string>{}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().kind, ArgErrorKind::EmptyPathList);
  EXPECT_EQ(parsed.error().arg, "extras");
}

TEST_F(ToolTest, AnEscapingPathIsRefusedWithTheSandboxsReason) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"../outside/secret.txt"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().kind, ArgErrorKind::BadPath);
  EXPECT_EQ(parsed.error().arg, "target");
  ASSERT_TRUE(parsed.error().path_error.has_value());
  EXPECT_EQ(*parsed.error().path_error, PathError::EscapesRoot);
  EXPECT_EQ(parsed.error().detail, "../outside/secret.txt");
}

TEST_F(ToolTest, AnEscapingEntryInAListNamesTheEntryThatEscaped) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"inside.txt"}},
      {"extras", std::vector<std::string>{"sub/deep.txt", "../outside/secret.txt"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().kind, ArgErrorKind::BadPath);
  EXPECT_EQ(parsed.error().arg, "extras");
  EXPECT_EQ(parsed.error().detail, "../outside/secret.txt");
}

TEST_F(ToolTest, ArgErrorRendersAsOneReadableLine) {
  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"../outside/secret.txt"}},
  };
  auto parsed = parse_args(kEchoSpec, raw, *box_);
  ASSERT_FALSE(parsed.has_value());
  const std::string line = to_string(parsed.error());
  EXPECT_NE(line.find("target"), std::string::npos);
  EXPECT_NE(line.find("../outside/secret.txt"), std::string::npos);
}

// --- the registry ------------------------------------------------------------

TEST(ToolRegistryTest, AddedToolsAreFoundByName) {
  ToolRegistry registry;
  ASSERT_TRUE(registry.add(std::make_unique<EchoTool>()).has_value());
  EXPECT_NE(registry.find("echo"), nullptr);
  EXPECT_EQ(registry.find("no_such_tool"), nullptr);
}

TEST(ToolRegistryTest, ANullToolIsRefused) {
  ToolRegistry registry;
  auto result = registry.add(nullptr);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, RegistryErrorKind::NullTool);
}

TEST(ToolRegistryTest, ASecondToolWithTheSameNameIsRefusedNotReplaced) {
  ToolRegistry registry;
  ASSERT_TRUE(registry.add(std::make_unique<EchoTool>()).has_value());
  Tool* first = registry.find("echo");

  auto result = registry.add(std::make_unique<EchoTool>());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, RegistryErrorKind::DuplicateName);
  EXPECT_EQ(registry.find("echo"), first) << "the original registration must survive";
  EXPECT_EQ(registry.tools().size(), 1u);
}

TEST(ToolRegistryTest, AToolWithAnInvalidSpecFailsAtCompositionTime) {
  class NamelessTool final : public Tool {
   public:
    [[nodiscard]] const ToolSpec& spec() const noexcept override {
      static const ToolSpec kSpec{"", "no name", {}};
      return kSpec;
    }
    [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs&) override {
      return ToolOutput{};
    }
  };

  ToolRegistry registry;
  auto result = registry.add(std::make_unique<NamelessTool>());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, RegistryErrorKind::BadSpec);
  ASSERT_TRUE(result.error().spec_error.has_value());
  EXPECT_EQ(*result.error().spec_error, SpecError::EmptyToolName);
  EXPECT_TRUE(registry.tools().empty());
}

TEST(ToolRegistryTest, ToolsIterateInRegistrationOrder) {
  class NamedTool final : public Tool {
   public:
    explicit NamedTool(const ToolSpec& spec) : spec_(&spec) {}
    [[nodiscard]] const ToolSpec& spec() const noexcept override { return *spec_; }
    [[nodiscard]] std::expected<ToolOutput, ToolError> run(const ToolArgs&) override {
      return ToolOutput{};
    }

   private:
    const ToolSpec* spec_;
  };
  static const ToolSpec kB{"b", "second", {}};
  static const ToolSpec kA{"a", "first", {}};

  ToolRegistry registry;
  ASSERT_TRUE(registry.add(std::make_unique<NamedTool>(kB)).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<NamedTool>(kA)).has_value());

  auto tools = registry.tools();
  ASSERT_EQ(tools.size(), 2u);
  EXPECT_EQ(tools[0]->spec().name, "b") << "registration order, not name order";
  EXPECT_EQ(tools[1]->spec().name, "a");
}

// --- dispatch, end to end ----------------------------------------------------

TEST_F(ToolTest, ParsedArgumentsCrossTheVirtualInterfaceIntact) {
  ToolRegistry registry;
  ASSERT_TRUE(registry.add(std::make_unique<EchoTool>()).has_value());

  const RawArgs raw{
      {"pattern", std::string{"hello"}},
      {"target", std::string{"inside.txt"}},
      {"extras", std::vector<std::string>{"sub/deep.txt"}},
  };
  Tool* tool = registry.find("echo");
  ASSERT_NE(tool, nullptr);

  auto parsed = parse_args(tool->spec(), raw, *box_);
  ASSERT_TRUE(parsed.has_value()) << to_string(parsed.error());

  auto output = tool->run(*parsed);
  ASSERT_TRUE(output.has_value()) << output.error().reason;
  ASSERT_EQ(output->rows.size(), 2u);

  const auto& first = output->rows[0].fields;
  ASSERT_EQ(first.size(), 2u);
  EXPECT_EQ(first[0].name, "pattern");
  EXPECT_EQ(std::get<std::string>(first[0].value), "hello");
  EXPECT_EQ(first[1].name, "target");
  EXPECT_EQ(std::get<std::string>(first[1].value), "inside.txt");

  const auto& second = output->rows[1].fields;
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0].name, "extra");
  EXPECT_EQ(std::get<std::string>(second[0].value), "sub/deep.txt");
}

}  // namespace
