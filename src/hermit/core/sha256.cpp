#include <hermit/core/sha256.h>

#include <bit>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define HERMIT_SHA256_X86 1
#include <cpuid.h>
#include <immintrin.h>
#endif

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

void process_block_portable(std::array<std::uint32_t, 8>& state,
                            const unsigned char* p) {
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

#ifdef HERMIT_SHA256_X86

// The SHA extension path. Same FIPS 180-4 rounds the portable function runs, driven
// through the CPU's dedicated instructions; both paths answer to the same test vectors,
// and the round constants come from the one kK table above rather than being
// re-transcribed into immediates.
//
// The message-schedule loop below is the standard formulation for these intrinsics:
// four 128-bit lanes hold four rounds of W each, and lane g is rebuilt from lanes
// g-4..g-1 exactly as the scalar recurrence builds w[i] from w[i-16..i-2].
__attribute__((target("sha,sse4.1"))) void process_blocks_sha_ext(
    std::array<std::uint32_t, 8>& state, const unsigned char* p, std::size_t count) {
  // Big-endian words to native lanes, per 32-bit word.
  const __m128i byteswap =
      _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);

  // state is {a..h}; the instructions want the ABEF / CDGH arrangement.
  __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[0]));
  __m128i st1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&state[4]));
  tmp = _mm_shuffle_epi32(tmp, 0xB1);
  st1 = _mm_shuffle_epi32(st1, 0x1B);
  __m128i st0 = _mm_alignr_epi8(tmp, st1, 8);
  st1 = _mm_blend_epi16(st1, tmp, 0xF0);

  while (count-- > 0) {
    const __m128i save0 = st0;
    const __m128i save1 = st1;

    __m128i m[4];
    __m128i round;
    for (int g = 0; g < 16; ++g) {
      if (g < 4) {
        m[g] = _mm_shuffle_epi8(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * g)), byteswap);
      } else {
        __m128i sched = _mm_sha256msg1_epu32(m[g & 3], m[(g - 3) & 3]);
        sched = _mm_add_epi32(sched, _mm_alignr_epi8(m[(g - 1) & 3], m[(g - 2) & 3], 4));
        m[g & 3] = _mm_sha256msg2_epu32(sched, m[(g - 1) & 3]);
      }
      round = _mm_add_epi32(
          m[g & 3], _mm_loadu_si128(reinterpret_cast<const __m128i*>(&kK[4 * g])));
      st1 = _mm_sha256rnds2_epu32(st1, st0, round);
      round = _mm_shuffle_epi32(round, 0x0E);
      st0 = _mm_sha256rnds2_epu32(st0, st1, round);
    }

    st0 = _mm_add_epi32(st0, save0);
    st1 = _mm_add_epi32(st1, save1);
    p += 64;
  }

  tmp = _mm_shuffle_epi32(st0, 0x1B);
  st1 = _mm_shuffle_epi32(st1, 0xB1);
  st0 = _mm_blend_epi16(tmp, st1, 0xF0);
  st1 = _mm_alignr_epi8(st1, tmp, 8);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[0]), st0);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(&state[4]), st1);
}

#endif  // HERMIT_SHA256_X86

// Decided once per process. HERMIT_SHA256_PORTABLE=1 forces the portable rounds -- the
// escape hatch if a CPU misreports the extension, and what lets one machine's test run
// exercise both paths.
bool use_sha_extensions() {
#ifdef HERMIT_SHA256_X86
  if (const char* forced = std::getenv("HERMIT_SHA256_PORTABLE");
      forced != nullptr && forced[0] != '\0' && forced[0] != '0') {
    return false;
  }
  // CPUID.(EAX=7,ECX=0):EBX bit 29 is the architectural SHA flag; asked directly
  // rather than through __builtin_cpu_supports, whose feature-name list varies
  // across compiler versions.
  unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0) return false;
  return (ebx & (1u << 29)) != 0;
#else
  return false;
#endif
}

void process_blocks(std::array<std::uint32_t, 8>& state, const unsigned char* p,
                    std::size_t count) {
  static const bool hardware = use_sha_extensions();
#ifdef HERMIT_SHA256_X86
  if (hardware) {
    process_blocks_sha_ext(state, p, count);
    return;
  }
#else
  (void)hardware;
#endif
  for (; count > 0; --count, p += 64) process_block_portable(state, p);
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
      process_blocks(h_, buf_, 1);
      buffered_ = 0;
    }
  }
  if (const std::size_t whole = n / sizeof(buf_); whole > 0) {
    process_blocks(h_, p, whole);
    p += whole * sizeof(buf_);
    n -= whole * sizeof(buf_);
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
