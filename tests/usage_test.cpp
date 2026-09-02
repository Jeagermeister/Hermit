// D18's usage-visibility mechanism: resolve_usage_dir/record_cloud_usage/
// read_usage_records round-tripped through a real Sandbox on a real filesystem, and
// price_usage against the rate table on its own.

#include <hermit/app/usage.h>

#include <gtest/gtest.h>

#include <cstdlib>  // mkdtemp
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <hermit/core/sandbox.h>

namespace fs = std::filesystem;
using hermit::Sandbox;
using hermit::app::price_usage;
using hermit::app::read_usage_records;
using hermit::app::record_cloud_usage;
using hermit::app::resolve_usage_dir;
using hermit::app::UsageRecord;

namespace {

class UsageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermit_usage_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));

    fs::create_directories(tmp_ / "root");
    auto sb = Sandbox::open(tmp_ / "root");
    ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
    box_ = std::make_unique<Sandbox>(std::move(*sb));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  fs::path tmp_;
  std::unique_ptr<Sandbox> box_;
};

TEST_F(UsageTest, TheDirIsOutsideTheRootAndNamedAfterIt) {
  const auto dir = resolve_usage_dir(*box_);
  EXPECT_EQ(dir, tmp_ / ".hermit-usage-root");
  // Same reasoning as the backup store (docs/17): a directory the model that generated
  // the usage cannot itself read or tamper with.
  EXPECT_FALSE(dir.string().starts_with(box_->root().string() + "/"));
}

TEST_F(UsageTest, ReadingBeforeAnyRecordExistsIsEmptyNotAnError) {
  std::vector<std::string> warnings;
  const auto records = read_usage_records(resolve_usage_dir(*box_), warnings);
  EXPECT_TRUE(records.empty());
  EXPECT_TRUE(warnings.empty());
}

TEST_F(UsageTest, WhatIsWrittenIsWhatIsRead) {
  ASSERT_TRUE(record_cloud_usage(*box_, "deepseek-v4-flash:cloud", 1000, 50));
  ASSERT_TRUE(record_cloud_usage(*box_, "kimi-k3:cloud", 2000, 300));

  std::vector<std::string> warnings;
  const auto records = read_usage_records(resolve_usage_dir(*box_), warnings);
  EXPECT_TRUE(warnings.empty());
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].model, "deepseek-v4-flash:cloud");
  EXPECT_EQ(records[0].prompt_tokens, 1000u);
  EXPECT_EQ(records[0].completion_tokens, 50u);
  EXPECT_GT(records[0].ts, 0);
  EXPECT_EQ(records[1].model, "kimi-k3:cloud");
  EXPECT_EQ(records[1].prompt_tokens, 2000u);
}

TEST_F(UsageTest, RepeatedCallsAppendRatherThanOverwrite) {
  ASSERT_TRUE(record_cloud_usage(*box_, "gpt-oss:120b", 100, 10));
  ASSERT_TRUE(record_cloud_usage(*box_, "gpt-oss:120b", 200, 20));
  ASSERT_TRUE(record_cloud_usage(*box_, "gpt-oss:120b", 300, 30));

  std::vector<std::string> warnings;
  const auto records = read_usage_records(resolve_usage_dir(*box_), warnings);
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[2].prompt_tokens, 300u);
}

TEST_F(UsageTest, ADamagedLineIsSkippedAndReportedRatherThanFailingTheWholeRead) {
  ASSERT_TRUE(record_cloud_usage(*box_, "gpt-oss:120b", 100, 10));

  const auto dir = resolve_usage_dir(*box_);
  std::ofstream out{dir / "usage.jsonl", std::ios::app};
  out << "not json at all\n";
  out.close();

  ASSERT_TRUE(record_cloud_usage(*box_, "gpt-oss:120b", 200, 20));

  std::vector<std::string> warnings;
  const auto records = read_usage_records(dir, warnings);
  EXPECT_EQ(records.size(), 2u);  // the two real records, not the damaged line
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings.front().find("usage.jsonl:2"), std::string::npos);
}

TEST_F(UsageTest, ALineMissingAFieldIsTreatedAsDamagedToo) {
  const auto dir = resolve_usage_dir(*box_);
  fs::create_directories(dir);
  std::ofstream out{dir / "usage.jsonl", std::ios::app};
  out << R"({"ts": 1, "model": "gpt-oss:120b", "prompt_tokens": 100})" << '\n';  // no completion_tokens
  out.close();

  std::vector<std::string> warnings;
  const auto records = read_usage_records(dir, warnings);
  EXPECT_TRUE(records.empty());
  EXPECT_EQ(warnings.size(), 1u);
}

// --- price_usage ---------------------------------------------------------------

TEST(PriceUsage, PricesAModelWithNoSizeInItsTag) {
  UsageRecord record{.model = "deepseek-v4-flash:cloud", .prompt_tokens = 1'000'000,
                     .completion_tokens = 1'000'000};
  const auto cost = price_usage(record);
  ASSERT_TRUE(cost.has_value());
  EXPECT_NEAR(*cost, 0.44 + 1.32, 1e-9);
}

TEST(PriceUsage, PricesAModelWhoseSizeAlreadyOccupiesTheColon) {
  // D18's own correction: the hyphen form, confirmed against a live `ollama list`.
  UsageRecord record{.model = "qwen3.5:397b-cloud", .prompt_tokens = 1'000'000,
                     .completion_tokens = 1'000'000};
  const auto cost = price_usage(record);
  ASSERT_TRUE(cost.has_value());
  EXPECT_NEAR(*cost, 0.60 + 3.60, 1e-9);
}

TEST(PriceUsage, StripsTheSizeSegmentWhenTheRateCardKeyHasNone) {
  // "gemma4:31b-cloud" -> "gemma4:31b" (suffix stripped) -> "gemma4" (rate card key,
  // no size) needs the second normalisation step -- this is the case that would fail
  // if only the cloud-suffix strip ran.
  UsageRecord record{.model = "gemma4:31b-cloud", .prompt_tokens = 1'000'000,
                     .completion_tokens = 1'000'000};
  const auto cost = price_usage(record);
  ASSERT_TRUE(cost.has_value());
  EXPECT_NEAR(*cost, 0.14 + 0.40, 1e-9);
}

TEST(PriceUsage, AModelWithNoRateCardEntryIsNulloptNotZero) {
  // Zero would silently vanish from a caller's total; nullopt forces it to be reported.
  UsageRecord record{.model = "some-future-model:cloud", .prompt_tokens = 1000,
                     .completion_tokens = 100};
  EXPECT_FALSE(price_usage(record).has_value());
}

TEST(PriceUsage, ZeroTokensPricesToZeroForAKnownModel) {
  UsageRecord record{.model = "kimi-k3:cloud", .prompt_tokens = 0, .completion_tokens = 0};
  const auto cost = price_usage(record);
  ASSERT_TRUE(cost.has_value());
  EXPECT_NEAR(*cost, 0.0, 1e-9);
}

}  // namespace
