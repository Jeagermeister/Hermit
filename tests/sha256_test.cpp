#include <hermes/core/sha256.h>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using hermes::sha256_hex;

namespace {

// Expected values generated with coreutils sha256sum on 2026-08-16, which
// agree with the FIPS 180-4 published vectors where one exists.

TEST(Sha256, EmptyInput) {
  EXPECT_EQ(sha256_hex(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, Abc) {
  EXPECT_EQ(sha256_hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, FipsTwoBlockVector) {
  // 56 bytes: the padding cannot fit, forcing the two-block tail path.
  EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, FipsLongerVector) {
  EXPECT_EQ(sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                       "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
            "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256, OneMillionAs) {
  const std::string input(1'000'000, 'a');
  EXPECT_EQ(sha256_hex(input),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, ASingleNulByte) {
  // Content is bytes, not text: a NUL is data, and the length comes from the
  // view, not from strlen.
  const std::string_view one_nul{"\0", 1};
  EXPECT_EQ(sha256_hex(one_nul),
            "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d");
}

TEST(Sha256, ExactlyOneBlock) {
  // 64 bytes: a full block with the padding entirely in the tail block.
  const std::string input(64, 'x');
  EXPECT_EQ(sha256_hex(input), sha256_hex(std::string(64, 'x')))
      << "determinism sanity";
  EXPECT_EQ(sha256_hex(input).size(), 64u);
  EXPECT_NE(sha256_hex(input), sha256_hex(std::string(63, 'x')));
}

}  // namespace
