#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "ruvia/http/http_datagram.h"

namespace ruvia::detail {

// RFC 9000 §16 variable-length integer, shared by the Capsule Protocol (RFC
// 9297 §3.2) and HTTP/3 Datagram framing (RFC 9297 §2.1).
inline constexpr std::uint64_t quic_varint_max = (std::uint64_t{1} << 62) - 1;

struct quic_varint final {
    std::uint64_t value_;
    std::size_t encoded_bytes_;
};

[[nodiscard]] constexpr std::size_t quic_varint_size(std::uint64_t value) noexcept {
    return value < (std::uint64_t{1} << 6) ? 1 : value < (std::uint64_t{1} << 14) ? 2
                                             : value < (std::uint64_t{1} << 30)   ? 4
                                                                                  : 8;
}

[[nodiscard]] inline std::variant<quic_varint, http_capsule_codec_error> decode_quic_varint(
    std::span<const char> input) noexcept {
    if (input.empty()) {
        return http_capsule_codec_error::need_more_data;
    }
    const auto first = static_cast<unsigned char>(input.front());
    const std::size_t size = std::size_t{1} << (first >> 6);
    if (input.size() < size) {
        return http_capsule_codec_error::need_more_data;
    }
    std::uint64_t value = first & 0x3f;
    for (std::size_t i = 1; i < size; ++i) {
        value = (value << 8) | static_cast<unsigned char>(input[i]);
    }
    return quic_varint{value, size};
}

[[nodiscard]] inline std::variant<std::size_t, http_capsule_codec_error> encode_quic_varint(
    std::span<char> output, std::uint64_t value) noexcept {
    if (value > quic_varint_max) {
        return http_capsule_codec_error::value_out_of_range;
    }
    const auto size = quic_varint_size(value);
    if (output.size() < size) {
        return http_capsule_codec_error::output_too_small;
    }
    for (std::size_t i = size; i != 0; --i) {
        output[i - 1] = static_cast<char>(value & 0xff);
        value >>= 8;
    }
    const unsigned tag = size == 1 ? 0 : size == 2 ? 0x40
                                     : size == 4   ? 0x80
                                                   : 0xc0;
    output[0] = static_cast<char>(static_cast<unsigned char>(output[0]) | tag);
    return size;
}

}  // namespace ruvia::detail
