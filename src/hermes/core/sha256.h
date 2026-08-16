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

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace hermes {

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view bytes);

/// Lowercase hex, 64 characters -- the form tool results carry.
[[nodiscard]] std::string sha256_hex(std::string_view bytes);

}  // namespace hermes
