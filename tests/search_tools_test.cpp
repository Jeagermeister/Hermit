#include <hermes/core/tools/find.h>
#include <hermes/core/tools/grep.h>

#include <gtest/gtest.h>

#include <cstdlib>  // mkdtemp
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <hermes/core/tool.h>
#include <hermes/core/tools/hash.h>
#include <hermes/core/tools/read.h>

namespace fs = std::filesystem;
using hermes::Field;
using hermes::FindTool;
using hermes::GrepTool;
using hermes::HashTool;
using hermes::parse_args;
using hermes::RawArgs;
using hermes::ReadTool;
using hermes::Sandbox;
using hermes::Tool;
using hermes::ToolOutput;
using hermes::ToolRow;

namespace {

// Layout built for every test:
//
//   <tmp>/root/notes.txt        three lines, "TODO" on lines 1 and 3
//   <tmp>/root/plain.txt        one line, no trailing newline
//   <tmp>/root/.hidden          dotfile
//   <tmp>/root/src/a.c
//   <tmp>/root/src/b.c
//   <tmp>/root/src/deep/c.c
//   <tmp>/root/link_dir -> src  (symlinked directory, must not be walked)
class SearchToolsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermes_search_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));

    fs::create_directories(tmp_ / "root" / "src" / "deep");
    write_file(tmp_ / "root" / "notes.txt",
               "TODO fix the loop\nnothing here\nanother TODO: tests\n");
    write_file(tmp_ / "root" / "plain.txt", "no trailing newline TODO");
    write_file(tmp_ / "root" / ".hidden", "dot\n");
    write_file(tmp_ / "root" / "src" / "a.c", "int a;\n");
    write_file(tmp_ / "root" / "src" / "b.c", "int b;\n");
    write_file(tmp_ / "root" / "src" / "deep" / "c.c", "int c;\n");
    fs::create_directory_symlink("src", tmp_ / "root" / "link_dir");

    auto sb = Sandbox::open(tmp_ / "root");
    ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
    box_ = std::make_unique<Sandbox>(std::move(*sb));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  static void write_file(const fs::path& p, std::string_view contents) {
    std::ofstream out(p, std::ios::binary);
    out << contents;
  }

  static const std::string* text(const ToolRow& row, std::string_view name) {
    for (const Field& f : row.fields) {
      if (f.name == name) return std::get_if<std::string>(&f.value);
    }
    return nullptr;
  }
  static const std::uint64_t* uint(const ToolRow& row, std::string_view name) {
    for (const Field& f : row.fields) {
      if (f.name == name) return std::get_if<std::uint64_t>(&f.value);
    }
    return nullptr;
  }

  std::expected<ToolOutput, hermes::ToolError> call(Tool& tool, const RawArgs& raw) {
    auto parsed = parse_args(tool.spec(), raw, *box_);
    if (!parsed) return std::unexpected{hermes::ToolError{to_string(parsed.error())}};
    return tool.invoke(*parsed);
  }

  fs::path tmp_;
  std::unique_ptr<Sandbox> box_;
};

// --- grep --------------------------------------------------------------------

TEST_F(SearchToolsTest, GrepFindsLiteralSubstringsLineGranular) {
  GrepTool grep;
  auto out = call(grep, {{"pattern", std::string{"TODO"}},
                         {"paths", std::vector<std::string>{"notes.txt"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 2u);

  EXPECT_EQ(*text(out->rows[0], "path"), "notes.txt");
  EXPECT_EQ(*uint(out->rows[0], "line"), 1u) << "line numbers are 1-based";
  EXPECT_EQ(*text(out->rows[0], "text"), "TODO fix the loop")
      << "exact line bytes, no terminator, no decoration";
  EXPECT_EQ(*uint(out->rows[1], "line"), 3u);
  EXPECT_EQ(*text(out->rows[1], "text"), "another TODO: tests");
}

TEST_F(SearchToolsTest, GrepIsLiteralNotRegex) {
  write_file(tmp_ / "root" / "re.txt", "abc\na.c\n");
  GrepTool grep;
  auto out = call(grep, {{"pattern", std::string{"a.c"}},
                         {"paths", std::vector<std::string>{"re.txt"}}});
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->rows.size(), 1u) << "'.' is a byte, not a wildcard";
  EXPECT_EQ(*uint(out->rows[0], "line"), 2u);
}

TEST_F(SearchToolsTest, GrepMatchesTheLastLineWithoutATrailingNewline) {
  GrepTool grep;
  auto out = call(grep, {{"pattern", std::string{"TODO"}},
                         {"paths", std::vector<std::string>{"plain.txt"}}});
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->rows.size(), 1u);
  EXPECT_EQ(*text(out->rows[0], "text"), "no trailing newline TODO");
}

TEST_F(SearchToolsTest, GrepZeroMatchesIsZeroRowsNotAFailure) {
  GrepTool grep;
  auto out = call(grep, {{"pattern", std::string{"absent-string"}},
                         {"paths", std::vector<std::string>{"notes.txt"}}});
  ASSERT_TRUE(out.has_value()) << "a visible zero-match is the answer";
  EXPECT_TRUE(out->rows.empty());
}

TEST_F(SearchToolsTest, GrepRefusesAnEmptyPattern) {
  GrepTool grep;
  auto out = call(grep, {{"pattern", std::string{}},
                         {"paths", std::vector<std::string>{"notes.txt"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("empty pattern"), std::string::npos);
}

TEST_F(SearchToolsTest, GrepSearchesMultipleFilesInArgumentOrder) {
  GrepTool grep;
  auto out = call(grep,
                  {{"pattern", std::string{"TODO"}},
                   {"paths", std::vector<std::string>{"plain.txt", "notes.txt"}}});
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->rows.size(), 3u);
  EXPECT_EQ(*text(out->rows[0], "path"), "plain.txt");
  EXPECT_EQ(*text(out->rows[1], "path"), "notes.txt");
}

TEST_F(SearchToolsTest, GrepFailsClosedOnAMissingFile) {
  GrepTool grep;
  auto out = call(grep, {{"pattern", std::string{"TODO"}},
                         {"paths", std::vector<std::string>{"nope.txt"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("nope.txt"), std::string::npos);
}

TEST_F(SearchToolsTest, GrepRefusesAFileOverItsCap) {
  GrepTool grep{8};
  auto out = call(grep, {{"pattern", std::string{"TODO"}},
                         {"paths", std::vector<std::string>{"notes.txt"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("read cap"), std::string::npos);
}

// --- read cap (same mechanism, guidance included) ----------------------------

TEST_F(SearchToolsTest, ReadRefusesOverCapWithSizeCapAndGuidance) {
  ReadTool read{10};
  auto out = call(read, {{"paths", std::vector<std::string>{"notes.txt"}}});
  ASSERT_FALSE(out.has_value()) << "never a truncated read";
  const std::string& reason = out.error().reason;
  EXPECT_NE(reason.find("notes.txt"), std::string::npos);
  EXPECT_NE(reason.find("51 bytes"), std::string::npos) << "the actual size, as data";
  EXPECT_NE(reason.find("10-byte read cap"), std::string::npos);
  EXPECT_NE(reason.find("hash"), std::string::npos) << "the graceful half: what still works";
}

TEST_F(SearchToolsTest, ReadAtExactlyTheCapSucceeds) {
  write_file(tmp_ / "root" / "ten.txt", "0123456789");
  ReadTool read{10};
  auto out = call(read, {{"paths", std::vector<std::string>{"ten.txt"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(*text(out->rows[0], "content"), "0123456789");
}

TEST_F(SearchToolsTest, HashIsUncappedAndStreamsLargeFiles) {
  const std::string big(1'000'000, 'a');
  write_file(tmp_ / "root" / "big.txt", big);
  HashTool hash;  // no cap parameter exists, by design
  auto out = call(hash, {{"paths", std::vector<std::string>{"big.txt"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(*text(out->rows[0], "hash"),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")
      << "sha256sum-verified digest of one million 'a's";
}

// --- find --------------------------------------------------------------------

TEST_F(SearchToolsTest, FindMatchesGlobsAgainstNamesDepthFirstSorted) {
  FindTool find;
  auto out = call(find, {{"pattern", std::string{"*.c"}},
                         {"path", std::string{"."}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 3u);
  EXPECT_EQ(*text(out->rows[0], "path"), "src/a.c");
  EXPECT_EQ(*text(out->rows[1], "path"), "src/b.c");
  EXPECT_EQ(*text(out->rows[2], "path"), "src/deep/c.c")
      << "pre-order walk, sorted names: a.c, b.c, then the deep subtree";
}

TEST_F(SearchToolsTest, FindQuestionMarkMatchesExactlyOneCharacter) {
  FindTool find;
  auto out = call(find, {{"pattern", std::string{"?.c"}},
                         {"path", std::string{"src"}}});
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->rows.size(), 3u) << "a.c, b.c, deep/c.c all have one-char stems";
}

TEST_F(SearchToolsTest, FindDoesNotWalkThroughASymlinkedDirectory) {
  FindTool find;
  auto out = call(find, {{"pattern", std::string{"*.c"}},
                         {"path", std::string{"."}}});
  ASSERT_TRUE(out.has_value());
  for (const ToolRow& row : out->rows) {
    EXPECT_EQ(text(row, "path")->find("link_dir"), std::string::npos)
        << "nothing behind a symlink is walked";
  }
}

TEST_F(SearchToolsTest, FindReportsTheSymlinkItselfWhenItsNameMatches) {
  FindTool find;
  auto out = call(find, {{"pattern", std::string{"link_*"}},
                         {"path", std::string{"."}}});
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->rows.size(), 1u) << "the entry matches; its target is not entered";
  EXPECT_EQ(*text(out->rows[0], "path"), "link_dir");
}

TEST_F(SearchToolsTest, FindStarMatchesDotfiles) {
  FindTool find;
  auto out = call(find, {{"pattern", std::string{".h*"}},
                         {"path", std::string{"."}}});
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->rows.size(), 1u);
  EXPECT_EQ(*text(out->rows[0], "path"), ".hidden");
}

TEST_F(SearchToolsTest, FindMatchesDirectoriesToo) {
  FindTool find;
  auto out = call(find, {{"pattern", std::string{"deep"}},
                         {"path", std::string{"src"}}});
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->rows.size(), 1u);
  EXPECT_EQ(*text(out->rows[0], "path"), "src/deep");
}

TEST_F(SearchToolsTest, FindZeroMatchesIsZeroRows) {
  FindTool find;
  auto out = call(find, {{"pattern", std::string{"*.nomatch"}},
                         {"path", std::string{"."}}});
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->rows.empty());
}

TEST_F(SearchToolsTest, FindRefusesAFileStart) {
  FindTool find;
  auto out = call(find, {{"pattern", std::string{"*"}},
                         {"path", std::string{"notes.txt"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("notes.txt"), std::string::npos);
}

// --- the full observe surface composes ---------------------------------------

TEST_F(SearchToolsTest, AllFiveObserveToolsShareOneRegistry) {
  hermes::ToolRegistry registry;
  ASSERT_TRUE(registry.add(std::make_unique<ReadTool>()).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<HashTool>()).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<FindTool>()).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<GrepTool>()).has_value());
  EXPECT_EQ(registry.tools().size(), 4u);
  EXPECT_NE(registry.find("grep"), nullptr);
  EXPECT_NE(registry.find("find"), nullptr);
}

}  // namespace
