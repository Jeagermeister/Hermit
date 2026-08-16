#pragma once

// Observed state -- ROUTING.md section 4: per-session, in memory, nothing
// persisted. A path is unseen, absent, or present at tuple T; a successful
// read, list or mutation records presence, a miss records absence, and a
// fresh session has observed nothing and must read before it may edit.
//
// The identity tuple is the one currency the whole surface shares: what
// `list` returns per entry is directly usable as the expected value the
// staleness guard compares -- dev:ino catches unlink-and-recreate, ctime
// catches what a confined process can still forge mtime around (D10).
//
// Keys are sandbox-relative paths. SandboxPath::relative() produces them for
// resolved targets; `list` joins its directory's relative path with entry
// names (which cannot contain '/'). Callers are first-party tools -- the same
// trust boundary as the tool bodies themselves.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include <sys/stat.h>

namespace hermes {

struct IdentityTuple {
  std::uint64_t dev = 0;
  std::uint64_t ino = 0;
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;
  std::int64_t ctime_ns = 0;

  friend bool operator==(const IdentityTuple&, const IdentityTuple&) = default;
};

[[nodiscard]] IdentityTuple tuple_from(const struct ::stat& st) noexcept;

class ObservedState {
 public:
  enum class Status { Unseen, Absent, Present };

  struct View {
    Status status = Status::Unseen;
    IdentityTuple tuple{};  // meaningful only when status == Present
  };

  void record_present(const std::filesystem::path& relative, const IdentityTuple& t) {
    map_[relative.string()] = t;
  }
  void record_absent(const std::filesystem::path& relative) {
    map_[relative.string()] = std::nullopt;
  }

  [[nodiscard]] View lookup(const std::filesystem::path& relative) const {
    const auto it = map_.find(relative.string());
    if (it == map_.end()) return View{};
    if (!it->second.has_value()) return View{.status = Status::Absent};
    return View{.status = Status::Present, .tuple = *it->second};
  }

 private:
  // nullopt = observed absent; no entry = unseen.
  std::unordered_map<std::string, std::optional<IdentityTuple>> map_;
};

}  // namespace hermes
