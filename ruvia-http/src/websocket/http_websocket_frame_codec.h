#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <variant>

#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_protocol.h"

// The RFC 6455 section 5.2 wire vocabulary: what a frame's first two bytes mean,
// which opcode and length combinations are legal, and the byte-level encode and
// unmask primitives. Everything here is a pure function of bytes -- no buffer
// ownership, no message state.

namespace ruvia::detail {

// The fixed-size scratch a frame header is encoded into (2 bytes plus up to an
// 8-byte extended length).
using websocket_frame_header_type = std::array<char, 10>;

// Wire failures are protocol values, not exceptions. The numeric values are the
// RFC 6455 §7.4.1 Close codes the connection must send before ending transport.
enum class websocket_protocol_failure : std::uint16_t {
    protocol_error = 1002,
    invalid_payload_data = 1007,
    message_too_large = 1009,
};

[[nodiscard]] constexpr std::uint16_t websocket_protocol_failure_close_code(
    websocket_protocol_failure failure) noexcept {
    return static_cast<std::uint16_t>(failure);
}

enum class websocket_frame_kind : std::uint8_t {
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

// Validated first-byte semantics. The raw continuation opcode is preserved as
// its own kind instead of being normalized to Text plus an independent boolean;
// compression is likewise admitted only where RFC 7692 permits RSV1.
class websocket_frame_start final {
public:
    [[nodiscard]] constexpr websocket_frame_kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr bool final() const noexcept {
        return final_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return compressed_;
    }

private:
    friend std::optional<websocket_frame_start> decode_websocket_frame_start(
        unsigned char, unsigned char, bool) noexcept;
    friend std::optional<websocket_frame_start> decode_websocket_frame_start(
        unsigned char, unsigned char, bool, bool) noexcept;

    constexpr websocket_frame_start(websocket_frame_kind kind, bool final, bool compressed) noexcept
        : kind_(kind),
          final_(final),
          compressed_(compressed) {}

    websocket_frame_kind kind_;
    bool final_;
    bool compressed_;
};

[[nodiscard]] inline bool is_invalid_websocket_raw_opcode(std::uint8_t raw_opcode) noexcept {
    return (raw_opcode >= 0x3 && raw_opcode <= 0x7) || raw_opcode >= 0xB;
}

[[nodiscard]] inline bool is_websocket_control_opcode(websocket_opcode opcode) noexcept {
    return static_cast<std::uint8_t>(opcode) >= 0x8;
}

[[nodiscard]] inline bool is_websocket_control_frame_kind(websocket_frame_kind kind) noexcept {
    return static_cast<std::uint8_t>(kind) >= 0x8;
}

// allow_rsv1 enables the RSV1 (compressed) bit when permessage-deflate is
// negotiated; it is valid only on the first frame of a data message, never on a
// continuation or control frame (RFC 7692 §6.1). RSV2/RSV3 are always rejected.
[[nodiscard]] inline std::optional<websocket_frame_start> decode_websocket_frame_start(
    unsigned char first, unsigned char second, bool allow_rsv1, bool expect_masked) noexcept {
    const auto raw_opcode = static_cast<std::uint8_t>(first & 0x0FU);
    const bool rsv1 = (first & 0x40U) != 0;
    if ((first & 0x30U) != 0 || ((second & 0x80U) != 0) != expect_masked ||
        is_invalid_websocket_raw_opcode(raw_opcode)) {
        return std::nullopt;
    }
    if (rsv1 && (!allow_rsv1 || raw_opcode == 0 || raw_opcode >= 0x8)) {
        return std::nullopt;
    }
    return websocket_frame_start(
        static_cast<websocket_frame_kind>(raw_opcode), (first & 0x80U) != 0, rsv1);
}

[[nodiscard]] inline std::optional<websocket_frame_start> decode_websocket_frame_start(
    unsigned char first, unsigned char second, bool allow_rsv1) noexcept {
    return decode_websocket_frame_start(first, second, allow_rsv1, true);
}

[[nodiscard]] inline bool is_invalid_websocket_control_frame(
    const websocket_frame_start& frame, std::uint64_t payload_size) noexcept {
    return is_websocket_control_frame_kind(frame.kind()) && (!frame.final() || payload_size > 125);
}

[[nodiscard]] inline bool websocket_message_exceeds_limit(
    std::size_t payload_size, protocol_byte_limit message_limit) noexcept {
    return message_limit.exceeds(payload_size);
}

[[nodiscard]] inline bool websocket_append_exceeds_limit(
    std::size_t current_size, std::size_t append_size, protocol_byte_limit message_limit) noexcept {
    return message_limit.addition_exceeds(current_size, append_size);
}

[[nodiscard]] inline bool websocket_frame_length_exceeds_limit(
    std::uint64_t payload_size, protocol_byte_limit message_limit) noexcept {
    if (payload_size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return true;
    }
    return message_limit.exceeds(static_cast<std::size_t>(payload_size));
}

// A control frame (Close/Ping/Pong) is capped at 125 bytes by RFC 6455 §5.5 and
// is explicitly NOT subject to the per-message size limit, so only data frames
// (Text/Binary/Continuation) are measured against max_message_bytes_. Applying the
// limit to control frames would reject a legal Ping/Pong, or a Close carrying a
// reason phrase, once max_message_bytes_ drops below 125 -- silently breaking the
// close handshake and keepalive on a small-message configuration.
[[nodiscard]] inline bool websocket_frame_exceeds_message_limit(
    websocket_frame_kind kind, std::uint64_t payload_size, protocol_byte_limit message_limit) noexcept {
    return !is_websocket_control_frame_kind(kind) &&
           websocket_frame_length_exceeds_limit(payload_size, message_limit);
}

[[nodiscard]] inline bool websocket_masked_frame_read_size_overflows(
    std::uint64_t payload_size, std::size_t header_size) noexcept {
    constexpr std::size_t mask_bytes = 4;
    constexpr auto max_size = (std::numeric_limits<std::size_t>::max)();
    return header_size > max_size - mask_bytes ||
           payload_size > static_cast<std::uint64_t>(max_size - header_size - mask_bytes);
}

[[nodiscard]] inline std::uint16_t read_websocket_uint16(const char* data) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<unsigned char>(data[0])) << 8) |
        static_cast<unsigned char>(data[1]));
}

[[nodiscard]] constexpr std::variant<std::uint64_t, websocket_protocol_failure> read_websocket_uint64(
    std::span<const char, 8> data) noexcept {
    if ((static_cast<unsigned char>(data[0]) & 0x80U) != 0) {
        return websocket_protocol_failure::protocol_error;
    }
    std::uint64_t value = 0;
    for (const char byte : data) {
        value = (value << 8) | static_cast<unsigned char>(byte);
    }
    return value;
}

[[nodiscard]] inline std::size_t encode_websocket_frame_header(websocket_frame_header_type& header_value,
    websocket_opcode opcode, std::size_t payload_size, bool rsv1 = false,
    bool masked = false) noexcept {
    std::size_t header_size = 0;
    header_value[header_size++] =
        static_cast<char>(0x80U | (rsv1 ? 0x40U : 0U) | static_cast<std::uint8_t>(opcode));
    if (payload_size <= 125) {
        header_value[header_size++] = static_cast<char>((masked ? 0x80U : 0U) | payload_size);
    } else if (payload_size <= 0xFFFF) {
        header_value[header_size++] = static_cast<char>((masked ? 0x80U : 0U) | 126U);
        header_value[header_size++] = static_cast<char>((payload_size >> 8) & 0xFF);
        header_value[header_size++] = static_cast<char>(payload_size & 0xFF);
    } else {
        header_value[header_size++] = static_cast<char>((masked ? 0x80U : 0U) | 127U);
        const auto size = static_cast<std::uint64_t>(payload_size);
        for (int shift = 56; shift >= 0; shift -= 8) {
            header_value[header_size++] = static_cast<char>((size >> shift) & 0xFF);
        }
    }
    return header_size;
}

inline void decode_masked_websocket_payload(
    char* payload_value, std::size_t payload_size, const char* mask) noexcept {
    const auto m0 = static_cast<unsigned char>(mask[0]);
    const auto m1 = static_cast<unsigned char>(mask[1]);
    const auto m2 = static_cast<unsigned char>(mask[2]);
    const auto m3 = static_cast<unsigned char>(mask[3]);
    std::size_t i = 0;
    for (; i + 4 <= payload_size; i += 4) {
        payload_value[i] = static_cast<char>(static_cast<unsigned char>(payload_value[i]) ^ m0);
        payload_value[i + 1] = static_cast<char>(static_cast<unsigned char>(payload_value[i + 1]) ^ m1);
        payload_value[i + 2] = static_cast<char>(static_cast<unsigned char>(payload_value[i + 2]) ^ m2);
        payload_value[i + 3] = static_cast<char>(static_cast<unsigned char>(payload_value[i + 3]) ^ m3);
    }
    if (i < payload_size) {
        payload_value[i] = static_cast<char>(static_cast<unsigned char>(payload_value[i]) ^ m0);
        ++i;
    }
    if (i < payload_size) {
        payload_value[i] = static_cast<char>(static_cast<unsigned char>(payload_value[i]) ^ m1);
        ++i;
    }
    if (i < payload_size) {
        payload_value[i] = static_cast<char>(static_cast<unsigned char>(payload_value[i]) ^ m2);
    }
}
}  // namespace ruvia::detail
