#include "crypto.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

TEST(Ruvia, crypto_sha256_matches_known_vectors_and_streams_beyond_inline_storage) {
    const ruvia::sha256_digest expected{
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    EXPECT_EQ(ruvia::sha256_of("abc"), expected);
    ruvia::sha256 small;
    small.update("a");
    small.update({});
    small.update("bc");
    EXPECT_EQ(small.finish(), expected);

    const std::string source(2048, '\0');
    ruvia::sha256 streamed;
    streamed.update(std::string_view(source).substr(0, 511));
    streamed.update(std::string_view(source).substr(511, 2));
    streamed.update(std::string_view(source).substr(513));
    ruvia::sha256_digest one_shot;
    ASSERT_TRUE(ruvia::compute_sha256(std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(source.data()), source.size()), one_shot));
    EXPECT_EQ(streamed.finish(), one_shot);
}

TEST(Ruvia, crypto_hmac_matches_rfc4231_and_rejects_short_output) {
    const std::string key(20, '\x0b');
    const std::array<std::uint8_t, 32> expected{
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7
    };
    std::array<std::uint8_t, ruvia::hmac_max_digest_size> output{};
    ASSERT_TRUE(ruvia::compute_hmac(ruvia::hmac_digest::sha256, key, "Hi There", output));
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output.begin()));
    EXPECT_FALSE(ruvia::compute_hmac(ruvia::hmac_digest::sha256, key, "Hi There", std::span(output).first(31)));
    EXPECT_FALSE(ruvia::compute_hmac(static_cast<ruvia::hmac_digest>(255), key, "Hi There", output));
}

TEST(Ruvia, crypto_comparison_and_random_bytes_accept_binary_and_empty_input) {
    const std::string_view binary("a\0b", 3);
    EXPECT_TRUE(ruvia::constant_time_bytes_equal({}, {}));
    EXPECT_TRUE(ruvia::constant_time_bytes_equal(binary, binary));
    EXPECT_FALSE(ruvia::constant_time_bytes_equal(binary, std::string_view("a\0c", 3)));
    EXPECT_FALSE(ruvia::constant_time_bytes_equal(binary, "a"));
    EXPECT_TRUE(ruvia::fill_random_bytes({}));
    std::array<unsigned char, 32> random;
    EXPECT_TRUE(ruvia::fill_random_bytes(random));
}

TEST(Ruvia, crypto_sha256_enforces_state_guard_and_supports_reset_reuse) {
    ruvia::sha256 hasher;
    hasher.update("abc");
    const auto first = hasher.finish();
    EXPECT_EQ(first, ruvia::sha256_of("abc"));

    EXPECT_THROW(hasher.update("x"), std::logic_error);
    EXPECT_THROW(hasher.update_u64_le(1), std::logic_error);
    EXPECT_THROW((void)hasher.finish(), std::logic_error);

    hasher.reset();
    hasher.update("def");
    EXPECT_EQ(hasher.finish(), ruvia::sha256_of("def"));

    hasher.reset();
    const std::string large(1024, 'z');
    hasher.update(large);
    EXPECT_EQ(hasher.finish(), ruvia::sha256_of(large));
    EXPECT_THROW(hasher.update("tail"), std::logic_error);

    hasher.reset();
    hasher.update_u64_le(0x0102030405060708ULL);
    const auto le_digest = hasher.finish();
    const std::string expected_le_bytes("\x08\x07\x06\x05\x04\x03\x02\x01", 8);
    EXPECT_EQ(le_digest, ruvia::sha256_of(expected_le_bytes));
}
