#include <hermes/core/sha256.h>

#include <bit>
#include <cstring>

namespace hermes {
namespace {

constexpr std::array<std::uint32_t, 64> kK{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

struct State {
  std::array<std::uint32_t, 8> h;
};

void process_block(State& s, const unsigned char* p) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (std::uint32_t{p[4 * i]} << 24) | (std::uint32_t{p[4 * i + 1]} << 16) |
           (std::uint32_t{p[4 * i + 2]} << 8) | std::uint32_t{p[4 * i + 3]};
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 =
        std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3];
  std::uint32_t e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t big_s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + big_s1 + ch + kK[static_cast<std::size_t>(i)] + w[i];
    const std::uint32_t big_s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = big_s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s.h[0] += a;
  s.h[1] += b;
  s.h[2] += c;
  s.h[3] += d;
  s.h[4] += e;
  s.h[5] += f;
  s.h[6] += g;
  s.h[7] += h;
}

}  // namespace

std::array<std::uint8_t, 32> sha256(std::string_view bytes) {
  State s{{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
           0x9b05688c, 0x1f83d9ab, 0x5be0cd19}};

  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  const std::size_t n = bytes.size();
  const std::size_t full_blocks = n / 64;
  for (std::size_t i = 0; i < full_blocks; ++i) {
    process_block(s, data + 64 * i);
  }

  // Padding: 0x80, zeros, then the message length in bits as 8 big-endian
  // bytes -- one trailing block, or two when fewer than 9 bytes remain free.
  unsigned char tail[128] = {};
  const std::size_t rem = n - full_blocks * 64;
  if (rem > 0) std::memcpy(tail, data + full_blocks * 64, rem);
  tail[rem] = 0x80;
  const std::size_t tail_len = (rem < 56) ? 64 : 128;
  const std::uint64_t bit_len = static_cast<std::uint64_t>(n) * 8;
  for (int i = 0; i < 8; ++i) {
    tail[tail_len - 1 - static_cast<std::size_t>(i)] =
        static_cast<unsigned char>(bit_len >> (8 * i));
  }
  process_block(s, tail);
  if (tail_len == 128) process_block(s, tail + 64);

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t i = 0; i < 8; ++i) {
    digest[4 * i] = static_cast<std::uint8_t>(s.h[i] >> 24);
    digest[4 * i + 1] = static_cast<std::uint8_t>(s.h[i] >> 16);
    digest[4 * i + 2] = static_cast<std::uint8_t>(s.h[i] >> 8);
    digest[4 * i + 3] = static_cast<std::uint8_t>(s.h[i]);
  }
  return digest;
}

std::string sha256_hex(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  const auto digest = sha256(bytes);
  std::string out;
  out.reserve(64);
  for (const std::uint8_t byte : digest) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

}  // namespace hermes
