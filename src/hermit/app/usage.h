#pragma once

// D18's usage-visibility mechanism. Ollama Cloud has no account-usage API
// (ollama/ollama#15132, #15663), so this is the only way to know what a Cloud-routed job
// actually cost: capture what the wire already reports, write it down, and price it back
// against a rate table that has to be kept in sync by hand.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <hermit/core/sandbox.h>

namespace hermit::app {

/// Where `record_cloud_usage` writes and `read_usage_records` reads:
/// `.hermit-usage-<root name>`, beside the sandbox root on the same grounds
/// `resolve_backup_dir`'s default already is -- outside the root, so the model whose usage
/// this describes can neither read nor tamper with its own bill. Exposed as its own
/// function so a reader cannot recompute the writer's path independently and drift from
/// it; both call this rather than each spelling out `.hermit-usage-` + the root name.
[[nodiscard]] std::filesystem::path resolve_usage_dir(const Sandbox& box);

/// Appends one JSON line -- {"ts", "model", "prompt_tokens", "completion_tokens"} -- to
/// `resolve_usage_dir(box) / "usage.jsonl"`. Deliberately without `resolve_backup_dir`'s
/// containment machinery -- there is no `--usage-dir` flag to validate a caller-supplied
/// path against, so there is nothing for it to check.
///
/// Only meaningful for a Cloud-tagged model. Callers are expected to check
/// `ollama::is_cloud_tag` first rather than have this repeat that decision -- a local
/// job costs nothing to log and there is nothing to log it against.
///
/// No cost is computed here: that is `price_usage`'s job, against a rate table that goes
/// stale on its own schedule, independent of this code.
///
/// Best-effort. A failure here does not fail the job it describes -- losing one
/// accounting line is not the class of problem undo's IoError exists to surface -- so
/// the failure is reported through the return value for a caller to warn about, not
/// through std::expected.
bool record_cloud_usage(const Sandbox& box, std::string_view model,
                        std::uint64_t prompt_tokens, std::uint64_t completion_tokens);

/// One line from the usage log, as `record_cloud_usage` wrote it.
struct UsageRecord {
  std::int64_t ts = 0;
  std::string model;
  std::uint64_t prompt_tokens = 0;
  std::uint64_t completion_tokens = 0;
};

/// Reads every record in `dir / "usage.jsonl"`, oldest first. A missing file is an empty
/// result -- the same "no history yet" case `undo` treats as a fact, not an error. A line
/// that fails to parse is skipped and its reason appended to `warnings` rather than
/// failing the whole read over one damaged entry: the log is diagnostic and
/// `record_cloud_usage` already treats writing it as best-effort, so reading it should
/// not hold it to a stricter standard than it was written under.
[[nodiscard]] std::vector<UsageRecord> read_usage_records(const std::filesystem::path& dir,
                                                            std::vector<std::string>& warnings);

/// Estimated cost in USD for one record, against Ollama's published per-model rate card
/// as snapshotted into this file 2026-09-02 (same numbers as
/// `docs/31-ollama-cloud-economics.md` -- the two have no shared source and must be
/// updated together by hand when pricing changes). Priced at the model's full input
/// rate, not the discounted cached rate: Ollama Cloud's caching is not reliably applied
/// (ollama/ollama#16714), so the honest estimate assumes none is credited.
///
/// `nullopt` means the model has no rate-card entry -- a caller should report that
/// plainly rather than silently omitting the record from a total, the same reasoning
/// `is_cloud_tag`'s missed `-cloud` suffix (D18) argues for: a quiet gap in coverage is
/// the worse failure here.
[[nodiscard]] std::optional<double> price_usage(const UsageRecord& record);

}  // namespace hermit::app
