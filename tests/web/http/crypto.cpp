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
        std::byte{0xba}, std::byte{0x78}, std::byte{0x16}, std::byte{0xbf}, std::byte{0x8f}, std::byte{0x01}, std::byte{0xcf}, std::byte{0xea},
        std::byte{0x41}, std::byte{0x41}, std::byte{0x40}, std::byte{0xde}, std::byte{0x5d}, std::byte{0xae}, std::byte{0x22}, std::byte{0x23},
        std::byte{0xb0}, std::byte{0x03}, std::byte{0x61}, std::byte{0xa3}, std::byte{0x96}, std::byte{0x17}, std::byte{0x7a}, std::byte{0x9c},
        std::byte{0xb4}, std::byte{0x10}, std::byte{0xff}, std::byte{0x61}, std::byte{0xf2}, std::byte{0x00}, std::byte{0x15}, std::byte{0xad}
    };
    EXPECT_EQ(ruvia::sha256_of("abc"), expected);
    ruvia::sha256 small;
    small.update("a");
    small.update(ruvia::byte_view{});
    small.update("bc");
    EXPECT_EQ(small.finish(), expected);

    const std::string source(2048, '\0');
    ruvia::sha256 streamed;
    streamed.update(std::string_view(source).substr(0, 511));
    streamed.update(std::string_view(source).substr(511, 2));
    streamed.update(std::string_view(source).substr(513));
    ruvia::sha256_digest one_shot;
    ASSERT_TRUE(ruvia::compute_sha256(ruvia::as_bytes(source), one_shot));
    EXPECT_EQ(streamed.finish(), one_shot);
}

TEST(Ruvia, crypto_hmac_matches_rfc4231_and_rejects_short_output) {
    const std::string key(20, '\x0b');
    const std::array<std::byte, 32> expected{
        std::byte{0xb0}, std::byte{0x34}, std::byte{0x4c}, std::byte{0x61}, std::byte{0xd8}, std::byte{0xdb}, std::byte{0x38}, std::byte{0x53},
        std::byte{0x5c}, std::byte{0xa8}, std::byte{0xaf}, std::byte{0xce}, std::byte{0xaf}, std::byte{0x0b}, std::byte{0xf1}, std::byte{0x2b},
        std::byte{0x88}, std::byte{0x1d}, std::byte{0xc2}, std::byte{0x00}, std::byte{0xc9}, std::byte{0x83}, std::byte{0x3d}, std::byte{0xa7},
        std::byte{0x26}, std::byte{0xe9}, std::byte{0x37}, std::byte{0x6c}, std::byte{0x2e}, std::byte{0x32}, std::byte{0xcf}, std::byte{0xf7}
    };
    std::array<std::byte, ruvia::hmac_max_digest_size> output{};
    ASSERT_TRUE(ruvia::compute_hmac(ruvia::hmac_digest::sha256, key, "Hi There", output));
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output.begin()));
    EXPECT_FALSE(ruvia::compute_hmac(ruvia::hmac_digest::sha256, key, "Hi There", std::span(output).first(31)));
    EXPECT_FALSE(ruvia::compute_hmac(static_cast<ruvia::hmac_digest>(255), key, "Hi There", output));
}

TEST(Ruvia, crypto_comparison_and_random_bytes_accept_binary_and_empty_input) {
    const std::string_view binary("a\0b", 3);
    EXPECT_TRUE(ruvia::constant_time_bytes_equal(ruvia::byte_view{}, ruvia::byte_view{}));
    EXPECT_TRUE(ruvia::constant_time_bytes_equal(binary, binary));
    EXPECT_FALSE(ruvia::constant_time_bytes_equal(binary, std::string_view("a\0c", 3)));
    EXPECT_FALSE(ruvia::constant_time_bytes_equal(binary, "a"));
    EXPECT_TRUE(ruvia::fill_random_bytes(ruvia::mutable_byte_view{}));
    std::array<std::byte, 32> random{};
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
