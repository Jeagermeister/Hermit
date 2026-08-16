#include <hermes/core/observed.h>

#include <sys/stat.h>

namespace hermes {

IdentityTuple tuple_from(const struct ::stat& st) noexcept {
  const auto ns = [](const timespec& t) noexcept {
    return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
  };
  return IdentityTuple{
      .dev = static_cast<std::uint64_t>(st.st_dev),
      .ino = static_cast<std::uint64_t>(st.st_ino),
      .size = static_cast<std::uint64_t>(st.st_size),
      .mtime_ns = ns(st.st_mtim),
      .ctime_ns = ns(st.st_ctim),
  };
}

}  // namespace hermes
