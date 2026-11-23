#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "byte_view.h"

namespace ruvia {

// RFC 9000 §16 variable-length integer, shared by QUIC packet framing, HTTP/3
// stream/frame framing (RFC 9114 §7), the Capsule Protocol (RFC 9297 §3.2) and
// HTTP Datagram framing (RFC 9297 §2.1).
inline constexpr std::uint64_t quic_varint_max = (std::uint64_t{1} << 62) - 1;

struct quic_varint final {
    std::uint64_t value_{0};
    std::size_t encoded_bytes_{0};

    friend constexpr bool operator==(const quic_varint&, const quic_varint&) noexcept = default;
};

enum class quic_varint_error : std::uint8_t {
    value_out_of_range,
    output_too_small,
};

// Encoded wire length (1, 2, 4 or 8 bytes) for `value <= quic_varint_max`.
[[nodiscard]] constexpr std::size_t quic_varint_size(std::uint64_t value) noexcept {
    return value < (std::uint64_t{1} << 6)
        ? 1
        : value < (std::uint64_t{1} << 14)
        ? 2
        : value < (std::uint64_t{1} << 30)
        ? 4
        : 8;
}

// Returns nullopt until `input` holds the whole integer its first byte announces.
[[nodiscard]] constexpr std::optional<quic_varint> decode_quic_varint(byte_view input) noexcept {
    if (input.empty()) {
        return std::nullopt;
    }
    const auto first = std::to_integer<unsigned int>(input.front());
    const std::size_t size = std::size_t{1} << (first >> 6);
    if (input.size() < size) {
        return std::nullopt;
    }
    std::uint64_t value = first & 0x3fU;
    for (std::size_t i = 1; i < size; ++i) {
        value = (value << 8) | std::to_integer<unsigned int>(input[i]);
    }
    return quic_varint{value, size};
}

[[nodiscard]] constexpr std::variant<std::size_t, quic_varint_error> encode_quic_varint(mutable_byte_view output, std::uint64_t value) noexcept {
    if (value > quic_varint_max) {
        return quic_varint_error::value_out_of_range;
    }
    const auto size = quic_varint_size(value);
    if (output.size() < size) {
        return quic_varint_error::output_too_small;
    }
    for (std::size_t i = size; i != 0; --i) {
        output[i - 1] = static_cast<std::byte>(value & 0xffU);
        value >>= 8;
    }
    const unsigned int tag = size == 1
        ? 0U
        : size == 2
        ? 0x40U
        : size == 4
        ? 0x80U
        : 0xc0U;
    output[0] = static_cast<std::byte>(std::to_integer<unsigned int>(output[0]) | tag);
    return size;
}

}  // namespace ruvia
