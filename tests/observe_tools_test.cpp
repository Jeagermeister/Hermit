#include <hermit/core/tools/hash.h>
#include <hermit/core/tools/list.h>
#include <hermit/core/tools/read.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <hermit/core/observed.h>
#include <hermit/core/sha256.h>
#include <hermit/core/tool.h>

namespace fs = std::filesystem;
using hermit::Field;
using hermit::HashTool;
using hermit::ListTool;
using hermit::parse_args;
using hermit::RawArgs;
using hermit::ReadTool;
using hermit::Sandbox;
using hermit::sha256_hex;
using hermit::Tool;
using hermit::ToolOutput;
using hermit::ToolRegistry;
using hermit::ToolRow;

namespace {

// Layout built for every test:
//
//   <tmp>/root/            <- the sandbox root
//   <tmp>/root/a.txt       "alpha\n"
//   <tmp>/root/blob.bin    "b\0in\0!" (6 bytes, embedded NULs)
//   <tmp>/root/sub/c.txt   "gamma"
//   <tmp>/root/link  -> sub/c.txt
//   <tmp>/root/empty/      empty directory
class ObserveToolsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermit_obs_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));

    fs::create_directories(tmp_ / "root" / "sub");
    fs::create_directories(tmp_ / "root" / "empty");
    write_file(tmp_ / "root" / "a.txt", "alpha\n");
    write_file(tmp_ / "root" / "blob.bin", std::string_view{"b\0in\0!", 6});
    write_file(tmp_ / "root" / "sub" / "c.txt", "gamma");
    fs::create_symlink("sub/c.txt", tmp_ / "root" / "link");

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

  // Field lookup helpers: null when the name is absent or the type is wrong.
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
  static const std::int64_t* integer(const ToolRow& row, std::string_view name) {
    for (const Field& f : row.fields) {
      if (f.name == name) return std::get_if<std::int64_t>(&f.value);
    }
    return nullptr;
  }
  static bool has_field(const ToolRow& row, std::string_view name) {
    for (const Field& f : row.fields) {
      if (f.name == name) return true;
    }
    return false;
  }

  // Parse + invoke through the registry, the way a dispatch site will.
  std::expected<ToolOutput, hermit::ToolError> call(Tool& tool, const RawArgs& raw) {
    auto parsed = parse_args(tool.spec(), raw, *box_);
    if (!parsed) return std::unexpected{hermit::ToolError{to_string(parsed.error())}};
    return tool.invoke(*parsed);
  }

  fs::path tmp_;
  std::unique_ptr<Sandbox> box_;
  hermit::ObservedState state_;
};

// --- read --------------------------------------------------------------------

TEST_F(ObserveToolsTest, ReadReturnsExactBytesAndHashPerFile) {
  ReadTool read{state_};
  auto out = call(read, {{"paths", std::vector<std::string>{"a.txt", "blob.bin"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 2u);

  ASSERT_NE(text(out->rows[0], "path"), nullptr);
  EXPECT_EQ(*text(out->rows[0], "path"), "a.txt");
  ASSERT_NE(text(out->rows[0], "content"), nullptr);
  EXPECT_EQ(*text(out->rows[0], "content"), "alpha\n");
  ASSERT_NE(text(out->rows[0], "hash"), nullptr);
  EXPECT_EQ(*text(out->rows[0], "hash"),
            "b6a98d9ce9a2d9149288fa3df42d377c3e42737afdcdaf714e33c0a100b51060")
      << "sha256sum-verified value for 'alpha\\n'";

  const std::string binary{"b\0in\0!", 6};
  ASSERT_NE(text(out->rows[1], "content"), nullptr);
  EXPECT_EQ(*text(out->rows[1], "content"), binary) << "NUL bytes survive intact";
  EXPECT_EQ(*text(out->rows[1], "hash"),
            "27b8a4f90b560e5e8660d60c2c47734048a6934ee7555972a7ff38aea3dc2b6a");
}

TEST_F(ObserveToolsTest, ReadContentCarriesNoDecoration) {
  ReadTool read{state_};
  auto out = call(read, {{"paths", std::vector<std::string>{"a.txt"}}});
  ASSERT_TRUE(out.has_value());
  const std::string& content = *text(out->rows[0], "content");
  // The N| incident, pinned: no line-number prefix, no trailing marker.
  EXPECT_EQ(content.find("1|"), std::string::npos);
  EXPECT_EQ(content, "alpha\n") << "bytes exact, nothing appended";
}

TEST_F(ObserveToolsTest, ReadFailsClosedNamingTheFileThatFailed) {
  ReadTool read{state_};
  auto out = call(read, {{"paths", std::vector<std::string>{"a.txt", "nope.txt"}}});
  ASSERT_FALSE(out.has_value()) << "one whole job: no partial answer";
  EXPECT_NE(out.error().reason.find("nope.txt"), std::string::npos);
}

TEST_F(ObserveToolsTest, ReadRefusesADirectoryTarget) {
  ReadTool read{state_};
  auto out = call(read, {{"paths", std::vector<std::string>{"sub"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("not a regular file"), std::string::npos);
}

TEST_F(ObserveToolsTest, ReadThroughASymlinkReadsTheResolvedTarget) {
  // resolve() expands symlinks (D6, POSIX order), so "link" names sub/c.txt
  // and the row reports the resolved relative path -- the file actually read.
  ReadTool read{state_};
  auto out = call(read, {{"paths", std::vector<std::string>{"link"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  EXPECT_EQ(*text(out->rows[0], "path"), "sub/c.txt");
  EXPECT_EQ(*text(out->rows[0], "content"), "gamma");
}

// --- hash --------------------------------------------------------------------

TEST_F(ObserveToolsTest, HashReturnsHashesWithoutContent) {
  HashTool hash;
  auto out = call(hash, {{"paths", std::vector<std::string>{"a.txt", "blob.bin"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 2u);

  EXPECT_EQ(*text(out->rows[0], "path"), "a.txt");
  EXPECT_EQ(*text(out->rows[0], "hash"),
            "b6a98d9ce9a2d9149288fa3df42d377c3e42737afdcdaf714e33c0a100b51060");
  EXPECT_FALSE(has_field(out->rows[0], "content"))
      << "hash exists so that verification does not pay for file bodies";
  EXPECT_EQ(out->rows[0].fields.size(), 2u);
}

TEST_F(ObserveToolsTest, HashAgreesWithReadsHash) {
  ReadTool read{state_};
  HashTool hash;
  auto r = call(read, {{"paths", std::vector<std::string>{"sub/c.txt"}}});
  auto h = call(hash, {{"paths", std::vector<std::string>{"sub/c.txt"}}});
  ASSERT_TRUE(r.has_value() && h.has_value());
  EXPECT_EQ(*text(r->rows[0], "hash"), *text(h->rows[0], "hash"))
      << "one hash currency across the surface (R3)";
}

TEST_F(ObserveToolsTest, HashFailsClosedOnAMissingFile) {
  HashTool hash;
  auto out = call(hash, {{"paths", std::vector<std::string>{"nope.txt"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("nope.txt"), std::string::npos);
}

// --- list --------------------------------------------------------------------

TEST_F(ObserveToolsTest, ListReturnsSortedEntriesWithTypeAndSize) {
  ListTool list{state_};
  auto out = call(list, {{"path", std::string{"."}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;

  // a.txt, blob.bin, empty, link, sub -- name-sorted.
  ASSERT_EQ(out->rows.size(), 5u);
  EXPECT_EQ(*text(out->rows[0], "path"), "a.txt");
  EXPECT_EQ(*text(out->rows[1], "path"), "blob.bin");
  EXPECT_EQ(*text(out->rows[2], "path"), "empty");
  EXPECT_EQ(*text(out->rows[3], "path"), "link");
  EXPECT_EQ(*text(out->rows[4], "path"), "sub");

  EXPECT_EQ(*text(out->rows[0], "type"), "file");
  EXPECT_EQ(*text(out->rows[2], "type"), "dir");
  EXPECT_EQ(*text(out->rows[3], "type"), "symlink") << "reported, never followed";
  EXPECT_EQ(*text(out->rows[4], "type"), "dir");

  EXPECT_EQ(*uint(out->rows[0], "size"), 6u) << "alpha\\n is six bytes";

  // The identity tuple is recorded supervisor-side, where the staleness guard
  // reads it; no tool accepts it back, so a rendered copy would only spend
  // prompt tokens on every later turn. Its absence from the row is the contract.
  EXPECT_EQ(uint(out->rows[0], "dev"), nullptr);
  EXPECT_EQ(uint(out->rows[0], "ino"), nullptr);
  EXPECT_EQ(integer(out->rows[0], "mtime_ns"), nullptr);
  EXPECT_EQ(integer(out->rows[0], "ctime_ns"), nullptr);
}

TEST_F(ObserveToolsTest, ListOfASubdirectoryPrefixesTheRelativePath) {
  ListTool list{state_};
  auto out = call(list, {{"path", std::string{"sub"}}});
  ASSERT_TRUE(out.has_value()) << out.error().reason;
  ASSERT_EQ(out->rows.size(), 1u);
  EXPECT_EQ(*text(out->rows[0], "path"), "sub/c.txt")
      << "directly usable as a later read/edit target";
}

TEST_F(ObserveToolsTest, ListOfAnEmptyDirectoryIsZeroRowsNotAFailure) {
  ListTool list{state_};
  auto out = call(list, {{"path", std::string{"empty"}}});
  ASSERT_TRUE(out.has_value()) << "zero entries is the correct answer";
  EXPECT_TRUE(out->rows.empty());
}

TEST_F(ObserveToolsTest, ListRefusesAFileTarget) {
  ListTool list{state_};
  auto out = call(list, {{"path", std::string{"a.txt"}}});
  ASSERT_FALSE(out.has_value());
  EXPECT_NE(out.error().reason.find("a.txt"), std::string::npos);
}

TEST_F(ObserveToolsTest, ListRecordsAStableIdentityTupleSupervisorSide) {
  ListTool list{state_};
  auto first = call(list, {{"path", std::string{"."}}});
  ASSERT_TRUE(first.has_value());
  const auto seen_first = state_.lookup("a.txt");
  ASSERT_EQ(seen_first.status, hermit::ObservedState::Status::Present);

  auto second = call(list, {{"path", std::string{"."}}});
  ASSERT_TRUE(second.has_value());
  const auto seen_second = state_.lookup("a.txt");
  ASSERT_EQ(seen_second.status, hermit::ObservedState::Status::Present);

  EXPECT_EQ(seen_first.tuple, seen_second.tuple)
      << "an untouched file keeps its tuple -- the staleness guard's premise";
  EXPECT_NE(seen_first.tuple.ino, 0u);
  EXPECT_GT(seen_first.tuple.ctime_ns, 1'577'000'000'000'000'000)
      << "plausible timestamp -- catches a forgotten field or unit mixup";
}

// --- the three together ------------------------------------------------------

TEST_F(ObserveToolsTest, AllThreeComposeIntoARegistry) {
  ToolRegistry registry;
  ASSERT_TRUE(registry.add(std::make_unique<ReadTool>(state_)).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<HashTool>()).has_value());
  ASSERT_TRUE(registry.add(std::make_unique<ListTool>(state_)).has_value());

  Tool* read = registry.find("read");
  ASSERT_NE(read, nullptr);
  auto parsed = parse_args(read->spec(),
                           {{"paths", std::vector<std::string>{"a.txt"}}}, *box_);
  ASSERT_TRUE(parsed.has_value());
  auto out = read->invoke(*parsed);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->rows.size(), 1u);
}

}  // namespace
