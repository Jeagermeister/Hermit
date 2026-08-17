#pragma once

// SHA-256 (FIPS 180-4), implemented here rather than pinned or vendored: R3
// needs a content hash, the build has no crypto dependency, and the algorithm
// is ~150 lines checked against published test vectors (the constants are the
// standard ones; a single wrong digit fails every vector in the tests).
//
// Not a security boundary, stated so the choice is not over-read: R3 compares
// a file with itself across time, so the requirement is that any byte change
// changes the hash -- not resistance to an adversary constructing collisions.
// If the requirement ever becomes adversarial, revisit this decision, not the
// constant tables.
//
// Streaming interface so `hash` verifies a file of any size at constant
// memory -- the read cap deliberately does not apply to hashing, because
// verification is exactly the job that must not degrade with size.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hermit {

class Sha256 {
 public:
  void update(std::string_view bytes);

  /// Finalise and return the digest. Call once; the object is spent after.
  [[nodiscard]] std::array<std::uint8_t, 32> finish();

 private:
  std::array<std::uint32_t, 8> h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                  0xa54ff53a, 0x510e527f, 0x9b05688c,
                                  0x1f83d9ab, 0x5be0cd19};
  unsigned char buf_[64] = {};
  std::size_t buffered_ = 0;
  std::uint64_t total_ = 0;
};

[[nodiscard]] std::string to_hex(const std::array<std::uint8_t, 32>& digest);

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view bytes);

/// Lowercase hex, 64 characters -- the form tool results carry.
[[nodiscard]] std::string sha256_hex(std::string_view bytes);

}  // namespace hermit
