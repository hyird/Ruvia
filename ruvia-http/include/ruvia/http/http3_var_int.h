#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace ruvia {

inline constexpr std::uint64_t http3_var_int_max = (std::uint64_t{1} << 62) - 1;
inline constexpr std::size_t http3_var_int_max_bytes = 8;

enum class http3_codec_error : std::uint8_t {
    need_more_data,
    value_out_of_range,
    output_too_small
};

struct http3_var_int final {
    std::uint64_t value_{0};
    std::size_t encoded_bytes_{0};
};

[[nodiscard]] inline constexpr std::variant<http3_var_int, http3_codec_error> decode_http3_var_int(
    std::span<const char> input) noexcept {
    if (input.empty()) {
        return http3_codec_error::need_more_data;
    }
    const auto first = static_cast<std::uint8_t>(input[0]);
    const auto encoded_bytes = std::size_t{1} << (first >> 6);
    if (input.size() < encoded_bytes) {
        return http3_codec_error::need_more_data;
    }
    std::uint64_t value = first & 0x3fU;
    for (std::size_t i = 1; i < encoded_bytes; ++i) {
        value = (value << 8) | static_cast<std::uint8_t>(input[i]);
    }
    return http3_var_int{.value_ = value, .encoded_bytes_ = encoded_bytes};
}

[[nodiscard]] inline constexpr std::variant<std::size_t, http3_codec_error> encode_http3_var_int(
    std::span<char> output, std::uint64_t value) noexcept {
    if (value > http3_var_int_max) {
        return http3_codec_error::value_out_of_range;
    }
    const std::size_t size = value < (std::uint64_t{1} << 6)    ? 1
                             : value < (std::uint64_t{1} << 14) ? 2
                             : value < (std::uint64_t{1} << 30) ? 4
                                                                : 8;
    if (output.size() < size) {
        return http3_codec_error::output_too_small;
    }
    for (std::size_t i = size; i > 0; --i) {
        output[i - 1] = static_cast<char>(value & 0xffU);
        value >>= 8;
    }
    const auto prefix = static_cast<std::uint8_t>(size == 1 ? 0 : size == 2 ? 1
                                                              : size == 4   ? 2
                                                                            : 3);
    output[0] = static_cast<char>(static_cast<std::uint8_t>(output[0]) | (prefix << 6));
    return size;
}

[[nodiscard]] inline constexpr std::size_t http3_var_int_encoded_size(std::uint64_t value) noexcept {
    return value < (std::uint64_t{1} << 6)    ? 1
           : value < (std::uint64_t{1} << 14) ? 2
           : value < (std::uint64_t{1} << 30) ? 4
                                              : 8;
}

}  // namespace ruvia
