#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_content_codec.h"

using ruvia::decode_http_content;
using ruvia::encode_http_content;
using ruvia::http_content_coding;
using ruvia::http_content_encode_context;

namespace {

constexpr std::array compressing_codings{http_content_coding::gzip, http_content_coding::deflate,
    http_content_coding::brotli, http_content_coding::zstd};

// Compressible but not trivially repetitive, so encoders emit several output
// blocks for the larger sizes.
[[nodiscard]] std::string sample_body(std::size_t size, unsigned seed) {
    std::string body(size, '\0');
    unsigned state_value = seed * 2654435761U + 1U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        body[i] = static_cast<char>('a' + ((state_value >> 16U) % 7U) + ((i / 64U) % 3U));
    }
    return body;
}

[[nodiscard]] std::string one_shot(http_content_coding coding, std::string_view input, std::size_t cap) {
    auto result_value = encode_http_content(coding, input, {.max_encoded_bytes_ = cap});
    return result_value.has_value() ? std::move(*result_value) : std::string();
}

[[nodiscard]] std::optional<std::string> encode_with(http_content_encode_context& context,
    std::string_view input, std::size_t cap) {
    auto result_value = context.encode(input, {.max_encoded_bytes_ = cap});
    if (!result_value.has_value()) {
        return std::nullopt;
    }
    return std::string(*result_value);
}

}  // namespace

TEST(Ruvia, http_content_encode_context_reuse_matches_one_shot_bytes) {
    constexpr std::array sizes{std::size_t{0}, std::size_t{1}, std::size_t{1000}, std::size_t{8192},
        std::size_t{70'000}, std::size_t{300'000}};
    for (const auto coding : compressing_codings) {
        http_content_encode_context context(coding);
        EXPECT_TRUE(context.coding() == coding);
        unsigned seed = 0;
        // Twice through, so every size also follows a larger representation.
        for (int round = 0; round < 2; ++round) {
            for (const auto size : sizes) {
                const auto input = sample_body(size, ++seed);
                const auto expected = one_shot(coding, input, input.size() + 1024);
                const auto actual = encode_with(context, input, input.size() + 1024);
                EXPECT_TRUE(actual.has_value());
                EXPECT_TRUE(actual.value_or(std::string()) == expected);
                const auto decoded = decode_http_content(
                    std::array{coding}, actual.value_or(std::string()), {.max_decoded_bytes_ = size});
                EXPECT_TRUE(std::holds_alternative<std::string>(decoded));
                if (const auto* content = std::get_if<std::string>(&decoded)) {
                    EXPECT_TRUE(std::string_view(*content) == input);
                }
            }
        }
    }
}
