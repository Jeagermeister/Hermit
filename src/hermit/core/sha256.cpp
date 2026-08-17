#include <hermit/core/sha256.h>

#include <bit>
#include <cstring>

namespace hermit {
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

void process_block(std::array<std::uint32_t, 8>& state, const unsigned char* p) {
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

  std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
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
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

void Sha256::update(std::string_view bytes) {
  total_ += bytes.size();
  const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
  std::size_t n = bytes.size();

  if (buffered_ > 0) {
    const std::size_t take = std::min(n, sizeof(buf_) - buffered_);
    std::memcpy(buf_ + buffered_, p, take);
    buffered_ += take;
    p += take;
    n -= take;
    if (buffered_ == sizeof(buf_)) {
      process_block(h_, buf_);
      buffered_ = 0;
    }
  }
  while (n >= sizeof(buf_)) {
    process_block(h_, p);
    p += sizeof(buf_);
    n -= sizeof(buf_);
  }
  if (n > 0) {
    std::memcpy(buf_, p, n);
    buffered_ = n;
  }
}

std::array<std::uint8_t, 32> Sha256::finish() {
  // Padding: 0x80, zeros to 56 mod 64, then the message length in bits as 8
  // big-endian bytes. bit_len is captured first; update() counting the
  // padding into total_ afterwards is harmless, the value is never read again.
  const std::uint64_t bit_len = total_ * 8;

  unsigned char pad[64] = {0x80};
  const std::size_t pad_len = (buffered_ < 56) ? (56 - buffered_) : (120 - buffered_);
  update({reinterpret_cast<const char*>(pad), pad_len});

  unsigned char len_be[8];
  for (int i = 0; i < 8; ++i) {
    len_be[7 - i] = static_cast<unsigned char>(bit_len >> (8 * i));
  }
  update({reinterpret_cast<const char*>(len_be), 8});

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t i = 0; i < 8; ++i) {
    digest[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
    digest[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
    digest[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
    digest[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
  }
  return digest;
}

std::string to_hex(const std::array<std::uint8_t, 32>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const std::uint8_t byte : digest) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

std::array<std::uint8_t, 32> sha256(std::string_view bytes) {
  Sha256 h;
  h.update(bytes);
  return h.finish();
}

std::string sha256_hex(std::string_view bytes) { return to_hex(sha256(bytes)); }

}  // namespace hermit
