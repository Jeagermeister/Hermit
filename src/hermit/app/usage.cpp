#include <hermit/app/usage.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace hermit::app {
namespace {

// $/M tokens, published at ollama.com/pricing, snapshotted 2026-09-02. Full input rate,
// not the discounted cached rate -- see price_usage's doc comment. Keyed by the model id
// as it appears in the rate card, with no provider prefix and no Cloud suffix: a record's
// `model` field is normalised down to one of these before lookup.
struct RateCardEntry {
  std::string_view model;
  double input_per_million;
  double output_per_million;
};

constexpr std::array<RateCardEntry, 19> kRateCard{{
    {"nemotron-3-super", 0.015, 0.60},
    {"nemotron-3-nano", 0.06, 0.24},
    {"nemotron-3-ultra", 0.10, 3.00},
    {"gpt-oss:20b", 0.07, 0.30},
    {"gpt-oss:120b", 0.15, 0.60},
    {"gemma4", 0.14, 0.40},
    {"glm-5.3-flash", 0.15, 0.50},
    {"glm-5.1", 1.00, 3.20},
    {"glm-5.2", 1.40, 4.40},
    {"glm-5.3", 1.40, 4.40},
    {"minimax-m2.7", 0.30, 1.20},
    {"minimax-m3", 0.60, 2.40},
    {"deepseek-v4-flash", 0.44, 1.32},
    {"deepseek-v4-pro", 1.32, 3.96},
    {"mistral-large-3", 0.50, 1.50},
    {"qwen3.5:397b", 0.60, 3.60},
    {"kimi-k2.6", 0.95, 4.00},
    {"kimi-k2.7-code", 0.95, 4.00},
    {"kimi-k3", 3.00, 15.00},
}};

// Strips whichever of D18's two Cloud suffixes (`ollama::is_cloud_tag`) is present, then
// -- if the result still doesn't name a rate-card entry directly, as for
// "qwen3.5:397b-cloud" -> "qwen3.5:397b" needing no further work but "gemma4:31b-cloud"
// -> "gemma4:31b" needing one more step -- strips a trailing `:size` segment and tries
// again. Mirrors the same two-stage normalisation the OpenCode-facing burn-rate skill
// already uses, for the same reason: a tag carries more than the rate card's own key.
std::string_view strip_cloud_suffix(std::string_view model) {
  constexpr std::string_view kColon = ":cloud";
  constexpr std::string_view kHyphen = "-cloud";
  if (model.size() > kColon.size() && model.ends_with(kColon)) {
    model.remove_suffix(kColon.size());
  } else if (model.size() > kHyphen.size() && model.ends_with(kHyphen)) {
    model.remove_suffix(kHyphen.size());
  }
  return model;
}

const RateCardEntry* find_rate(std::string_view model) {
  const std::string_view stripped = strip_cloud_suffix(model);
  for (const auto& entry : kRateCard) {
    if (entry.model == stripped) return &entry;
  }
  const auto colon = stripped.rfind(':');
  if (colon == std::string_view::npos) return nullptr;
  const std::string_view shorter = stripped.substr(0, colon);
  for (const auto& entry : kRateCard) {
    if (entry.model == shorter) return &entry;
  }
  return nullptr;
}

}  // namespace

std::filesystem::path resolve_usage_dir(const Sandbox& box) {
  return box.root().parent_path() / (".hermit-usage-" + box.root().filename().string());
}

bool record_cloud_usage(const Sandbox& box, std::string_view model,
                        std::uint64_t prompt_tokens, std::uint64_t completion_tokens) {
  const std::filesystem::path dir = resolve_usage_dir(box);

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::cerr << "warning: could not create " << dir << " for usage tracking ("
              << ec.message() << "); this job's cost will not be logged\n";
    return false;
  }

  const nlohmann::json record = {
      {"ts", std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()},
      {"model", std::string{model}},
      {"prompt_tokens", prompt_tokens},
      {"completion_tokens", completion_tokens},
  };

  const std::filesystem::path log = dir / "usage.jsonl";
  std::ofstream out{log, std::ios::app};
  if (!out) {
    std::cerr << "warning: could not open " << log
              << " for usage tracking; this job's cost will not be logged\n";
    return false;
  }
  out << record.dump() << '\n';
  return static_cast<bool>(out);
}

std::vector<UsageRecord> read_usage_records(const std::filesystem::path& dir,
                                            std::vector<std::string>& warnings) {
  std::vector<UsageRecord> records;
  const std::filesystem::path log = dir / "usage.jsonl";

  std::ifstream in{log};
  if (!in) return records;  // No file yet is "nothing logged", not a failure.

  std::string line;
  std::size_t line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    if (line.empty()) continue;
    try {
      const nlohmann::json parsed = nlohmann::json::parse(line);
      UsageRecord record;
      record.ts = parsed.at("ts").get<std::int64_t>();
      record.model = parsed.at("model").get<std::string>();
      record.prompt_tokens = parsed.at("prompt_tokens").get<std::uint64_t>();
      record.completion_tokens = parsed.at("completion_tokens").get<std::uint64_t>();
      records.push_back(std::move(record));
    } catch (const std::exception& e) {
      std::ostringstream note;
      note << log.string() << ":" << line_number << ": " << e.what();
      warnings.push_back(note.str());
    }
  }
  return records;
}

std::optional<double> price_usage(const UsageRecord& record) {
  const RateCardEntry* rate = find_rate(record.model);
  if (rate == nullptr) return std::nullopt;
  return (static_cast<double>(record.prompt_tokens) / 1'000'000.0) * rate->input_per_million +
        (static_cast<double>(record.completion_tokens) / 1'000'000.0) *
            rate->output_per_million;
}

}  // namespace hermit::app
