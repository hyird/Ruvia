#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ruvia {

[[nodiscard]] inline constexpr std::size_t base64_encoded_size(std::size_t input_size) noexcept {
    return 4 * ((input_size + 2) / 3);
}

// Standard-alphabet base64 with '=' padding. `output` must have room for
// base64_encoded_size(input.size()) characters.
inline void encode_base64(char* output, std::span<const std::uint8_t> input) noexcept {
    static constexpr char table_value[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::size_t i = 0;
    std::size_t out = 0;
    while (i + 3 <= input.size()) {
        const auto value = (static_cast<std::uint32_t>(input[i]) << 16) |
                           (static_cast<std::uint32_t>(input[i + 1]) << 8) |
                           static_cast<std::uint32_t>(input[i + 2]);
        output[out++] = table_value[(value >> 18) & 0x3F];
        output[out++] = table_value[(value >> 12) & 0x3F];
        output[out++] = table_value[(value >> 6) & 0x3F];
        output[out++] = table_value[value & 0x3F];
        i += 3;
    }
    if (i == input.size()) {
        return;
    }
    const auto remaining = input.size() - i;
    const auto value = static_cast<std::uint32_t>(input[i]) << 16 |
                       (remaining == 2 ? static_cast<std::uint32_t>(input[i + 1]) << 8 : 0U);
    output[out++] = table_value[(value >> 18) & 0x3F];
    output[out++] = table_value[(value >> 12) & 0x3F];
    output[out++] = remaining == 2 ? table_value[(value >> 6) & 0x3F] : '=';
    output[out] = '=';
}

}  // namespace ruvia

namespace ruvia::detail {
using ::ruvia::base64_encoded_size;
using ::ruvia::encode_base64;
}  // namespace ruvia::detail
