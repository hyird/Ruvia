#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

#include "borrowed_text.h"
#include "byte_view.h"
#include "http_io_buffer.h"
#include "http_protocol.h"
#include "websocket_connection.h"

namespace ruvia {

// Maximum encoded size of an unmasked WebSocket frame header (2 bytes plus up
// to an 8-byte extended payload length; RFC 6455 §5.2).
inline constexpr std::size_t websocket_max_frame_header_bytes = 10;
using websocket_frame_header_bytes = std::array<std::byte, websocket_max_frame_header_bytes>;

// Wire failures are protocol values, not exceptions. The numeric values are the
// RFC 6455 §7.4.1 Close codes the connection must send before ending transport.
enum class websocket_protocol_failure : std::uint16_t {
    protocol_error = 1002,
    invalid_payload_data = 1007,
    message_too_large = 1009,
};

[[nodiscard]] constexpr std::uint16_t websocket_protocol_failure_close_code(websocket_protocol_failure failure) noexcept {
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

[[nodiscard]] constexpr bool is_websocket_control_frame_kind(websocket_frame_kind kind) noexcept {
    return static_cast<std::uint8_t>(kind) >= 0x8;
}

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
    friend constexpr std::optional<websocket_frame_start> decode_websocket_frame_start(std::byte, std::byte, bool, bool) noexcept;

    constexpr websocket_frame_start(websocket_frame_kind kind, bool final, bool compressed) noexcept :
        kind_(kind),
        final_(final),
        compressed_(compressed) {}

    websocket_frame_kind kind_;
    bool final_;
    bool compressed_;
};

namespace detail {

[[nodiscard]] constexpr bool is_invalid_websocket_raw_opcode(std::uint8_t raw_opcode) noexcept {
    return (raw_opcode >= 0x3 && raw_opcode <= 0x7) || raw_opcode >= 0xB;
}

[[nodiscard]] constexpr bool is_invalid_websocket_control_frame(const websocket_frame_start& frame, std::uint64_t payload_size) noexcept {
    return is_websocket_control_frame_kind(frame.kind()) && (!frame.final() || payload_size > 125);
}

[[nodiscard]] inline bool websocket_frame_length_exceeds_limit(std::uint64_t payload_size, protocol_byte_limit message_limit) noexcept {
    if (payload_size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return true;
    }
    return message_limit.exceeds(static_cast<std::size_t>(payload_size));
}

// A control frame (Close/Ping/Pong) is capped at 125 bytes by RFC 6455 §5.5 and
// is explicitly NOT subject to the per-message size limit, so only data frames
// (Text/Binary/Continuation) are measured against max_message_bytes_.
[[nodiscard]] inline bool websocket_frame_exceeds_message_limit(websocket_frame_kind kind, std::uint64_t payload_size, protocol_byte_limit message_limit) noexcept {
    return !is_websocket_control_frame_kind(kind) && websocket_frame_length_exceeds_limit(payload_size, message_limit);
}

[[nodiscard]] constexpr std::uint16_t read_websocket_uint16(byte_view data) noexcept {
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(data[0]) << 8) | std::to_integer<std::uint16_t>(data[1]));
}

[[nodiscard]] constexpr std::variant<std::uint64_t, websocket_protocol_failure> read_websocket_uint64(std::span<const std::byte, 8> data) noexcept {
    if ((std::to_integer<unsigned int>(data[0]) & 0x80U) != 0) {
        return websocket_protocol_failure::protocol_error;
    }
    std::uint64_t value = 0;
    for (const std::byte byte : data) {
        value = (value << 8) | std::to_integer<unsigned int>(byte);
    }
    return value;
}

}  // namespace detail

// allow_rsv1 enables the RSV1 (compressed) bit when permessage-deflate is
// negotiated; it is valid only on the first frame of a data message, never on a
// continuation or control frame (RFC 7692 §6.1). RSV2/RSV3 are always rejected.
[[nodiscard]] constexpr std::optional<websocket_frame_start> decode_websocket_frame_start(std::byte first_byte, std::byte second_byte, bool allow_rsv1, bool expect_masked) noexcept {
    const auto first = std::to_integer<unsigned int>(first_byte);
    const auto second = std::to_integer<unsigned int>(second_byte);
    const auto raw_opcode = static_cast<std::uint8_t>(first & 0x0FU);
    const bool rsv1 = (first & 0x40U) != 0;
    if ((first & 0x30U) != 0 || ((second & 0x80U) != 0) != expect_masked || detail::is_invalid_websocket_raw_opcode(raw_opcode)) {
        return std::nullopt;
    }
    if (rsv1 && (!allow_rsv1 || raw_opcode == 0 || raw_opcode >= 0x8)) {
        return std::nullopt;
    }
    return websocket_frame_start(static_cast<websocket_frame_kind>(raw_opcode), (first & 0x80U) != 0, rsv1);
}

// Encodes an unmasked frame header (excluding any 4-byte masking key).
// `opcode_bits` is 0 for a continuation frame (RFC 6455 section 5.4).
// Returns 0 when `output` is smaller than the required 2, 4 or 10 bytes.
[[nodiscard]] constexpr std::size_t encode_websocket_frame_header(mutable_byte_view output, std::uint8_t opcode_bits, bool final, std::size_t payload_size, bool rsv1 = false, bool masked = false) noexcept {
    const std::size_t required = payload_size <= 125 ? 2 : (payload_size <= 0xFFFFU ? 4 : 10);
    if (output.size() < required) {
        return 0;
    }
    output[0] = static_cast<std::byte>((final ? 0x80U : 0U) | (rsv1 ? 0x40U : 0U) | (opcode_bits & 0x0FU));
    if (payload_size <= 125) {
        output[1] = static_cast<std::byte>((masked ? 0x80U : 0U) | payload_size);
    } else if (payload_size <= 0xFFFFU) {
        output[1] = static_cast<std::byte>((masked ? 0x80U : 0U) | 126U);
        output[2] = static_cast<std::byte>((payload_size >> 8) & 0xFFU);
        output[3] = static_cast<std::byte>(payload_size & 0xFFU);
    } else {
        output[1] = static_cast<std::byte>((masked ? 0x80U : 0U) | 127U);
        const auto size = static_cast<std::uint64_t>(payload_size);
        std::size_t cursor = 2;
        for (int shift = 56; shift >= 0; shift -= 8) {
            output[cursor++] = static_cast<std::byte>((size >> shift) & 0xFFU);
        }
    }
    return required;
}

// XORs `payload` with the 4-byte masking key in place (RFC 6455 §5.3), starting
// at key offset 0. Replicates the key into wider words loaded/stored via memcpy
// (valid at any alignment and endianness) with SSE2 acceleration when available.
inline void mask_websocket_payload(mutable_byte_view payload, std::span<const std::byte, 4> mask_key) noexcept {
    auto* const payload_bytes = payload.data();
    const auto payload_size = payload.size();
    std::array<std::byte, 8> key_bytes;
    std::memcpy(key_bytes.data(), mask_key.data(), 4);
    std::memcpy(key_bytes.data() + 4, mask_key.data(), 4);
    std::uint64_t key = 0;
    std::memcpy(&key, key_bytes.data(), sizeof(key));
    std::size_t i = 0;
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    const auto wide_key = _mm_set1_epi64x(static_cast<long long>(key));
    for (; payload_size - i >= 16; i += 16) {
        auto* const block = reinterpret_cast<__m128i*>(payload_bytes + i);
        _mm_storeu_si128(block, _mm_xor_si128(_mm_loadu_si128(block), wide_key));
    }
#endif
    for (; payload_size - i >= sizeof(key); i += sizeof(key)) {
        std::uint64_t word = 0;
        std::memcpy(&word, payload_bytes + i, sizeof(word));
        word ^= key;
        std::memcpy(payload_bytes + i, &word, sizeof(word));
    }
    for (; i < payload_size; ++i) {
        payload_bytes[i] ^= mask_key[i & 3U];
    }
}

// The Close frame payload (RFC 6455 section 5.5.1): encoding the one an
// endpoint sends (a validated status code followed by an optional UTF-8
// reason, capped at the 125-byte control-frame limit) and checking the one a
// peer sent.
enum class websocket_close_payload_encode_error : std::uint8_t {
    invalid_reason,
    reason_too_large,
};

class websocket_close_payload_encode_result;

class websocket_encoded_close_payload final {
public:
    [[nodiscard]] constexpr byte_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return byte_view(bytes_.data(), size_);
    }
    byte_view bytes() const&& = delete;

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return as_chars(bytes());
    }
    std::string_view view() const&& = delete;

private:
    friend class websocket_close_payload_encode_result;
    friend websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code, std::string_view) noexcept;

    websocket_encoded_close_payload(websocket_close_code code, std::string_view reason) noexcept;

    std::array<std::byte, 125> bytes_{};
    std::uint8_t size_{0};
};

class websocket_close_payload_encode_failure final {
public:
    [[nodiscard]] constexpr websocket_close_payload_encode_error error() const noexcept {
        return error_;
    }

private:
    friend class websocket_close_payload_encode_result;
    friend websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code, std::string_view) noexcept;

    explicit constexpr websocket_close_payload_encode_failure(websocket_close_payload_encode_error error) noexcept :
        error_(error) {}

    websocket_close_payload_encode_error error_;
};

class websocket_close_payload_encode_result final {
public:
    [[nodiscard]] constexpr const websocket_encoded_close_payload* encoded() const& noexcept RUVIA_LIFETIMEBOUND {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const websocket_encoded_close_payload* encoded() const&& = delete;

    [[nodiscard]] constexpr const websocket_close_payload_encode_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const websocket_close_payload_encode_failure* failure() const&& = delete;

private:
    friend websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code, std::string_view) noexcept;

    explicit websocket_close_payload_encode_result(websocket_encoded_close_payload encoded) noexcept :
        value_(encoded) {}

    explicit constexpr websocket_close_payload_encode_result(websocket_close_payload_encode_failure failure) noexcept :
        value_(failure) {}

    using value_type = std::variant<websocket_encoded_close_payload, websocket_close_payload_encode_failure>;
    value_type value_;
};

[[nodiscard]] websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code code, std::string_view reason = {}) noexcept;

// The protocol failure a received Close payload commits, or nullopt when it is legal.
[[nodiscard]] std::optional<websocket_protocol_failure> websocket_close_payload_failure(byte_view payload_value) noexcept;
[[nodiscard]] inline std::optional<websocket_protocol_failure> websocket_close_payload_failure(std::string_view payload_value) noexcept {
    return websocket_close_payload_failure(as_bytes(payload_value));
}

class websocket_frame_read_result;

// One borrowed frame published by the wire reader, which owns masking, length,
// opcode/compression combinations, and Close payload validation. Payload
// storage must outlive the view.
class websocket_frame_view final {
public:
    [[nodiscard]] constexpr websocket_frame_kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] byte_view payload_bytes() const noexcept {
        return as_bytes(payload_);
    }

    [[nodiscard]] constexpr bool final() const noexcept {
        return final_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return compressed_;
    }

private:
    friend class websocket_frame_read_result;

    constexpr websocket_frame_view(const websocket_frame_start& start, std::string_view payload_value) noexcept :
        kind_(start.kind()),
        payload_(payload_value),
        final_(start.final()),
        compressed_(start.compressed()) {}

    websocket_frame_kind kind_;
    std::string_view payload_;
    bool final_;
    bool compressed_;
};

// Drop the bytes the previously returned frame occupied. Consuming never moves
// bytes; the buffer moves the unread tail only when a later read needs room.
// A drained buffer keeps at most websocket_retained_buffer_bytes, so a burst
// of large messages does not stay pinned to the connection.
inline void consume_websocket_read_frame(http_io_buffer& buffer, std::size_t& pending_consume) noexcept {
    if (pending_consume == 0) {
        return;
    }
    buffer.consume(std::exchange(pending_consume, 0));
    if (buffer.empty()) {
        buffer.clear_retaining_small(websocket_retained_buffer_bytes);
    }
}

class websocket_frame_need_input final {
public:
    // Bytes the frame still lacks once its header is complete; 0 while the
    // header itself is incomplete. The header length is already within the
    // message limit.
    [[nodiscard]] constexpr std::size_t missing_bytes() const noexcept {
        return missing_bytes_;
    }

private:
    friend class websocket_frame_read_result;
    explicit constexpr websocket_frame_need_input(std::size_t missing_bytes) noexcept :
        missing_bytes_(missing_bytes) {}

    std::size_t missing_bytes_;
};

class websocket_frame_read_failure final {
public:
    [[nodiscard]] constexpr websocket_protocol_failure error() const noexcept {
        return error_;
    }

private:
    friend class websocket_frame_read_result;

    explicit constexpr websocket_frame_read_failure(websocket_protocol_failure error) noexcept :
        error_(error) {}

    websocket_protocol_failure error_;
};

// Parsing either needs more input, exposes one complete borrowed frame, or reports
// one typed protocol failure.
class websocket_frame_read_result final {
public:
    [[nodiscard]] constexpr const websocket_frame_need_input* need_input() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_frame_need_input>(&value_);
    }
    const websocket_frame_need_input* need_input() const&& = delete;

    [[nodiscard]] constexpr const websocket_frame_view* frame() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_frame_view>(&value_);
    }
    const websocket_frame_view* frame() const&& = delete;

    [[nodiscard]] constexpr const websocket_frame_read_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_frame_read_failure>(&value_);
    }
    const websocket_frame_read_failure* failure() const&& = delete;

private:
    friend websocket_frame_read_result websocket_try_read_frame(http_io_buffer&, std::size_t&, protocol_byte_limit, bool, bool);

    using value_type = std::variant<websocket_frame_need_input, websocket_frame_view, websocket_frame_read_failure>;

    template <typename alternative_type>
    explicit constexpr websocket_frame_read_result(alternative_type alternative) noexcept :
        value_(alternative) {}

    [[nodiscard]] static constexpr websocket_frame_read_result make_need_input(std::size_t missing_bytes = 0) noexcept {
        return websocket_frame_read_result(websocket_frame_need_input(missing_bytes));
    }

    [[nodiscard]] static constexpr websocket_frame_read_result make_frame(const websocket_frame_start& start, std::string_view payload_value) noexcept {
        return websocket_frame_read_result(websocket_frame_view(start, payload_value));
    }

    [[nodiscard]] static constexpr websocket_frame_read_result make_failure(websocket_protocol_failure error) noexcept {
        return websocket_frame_read_result(websocket_frame_read_failure(error));
    }

    value_type value_;
};

// Single owner of websocket frame decode (RFC 6455 §5.2): FIN/opcode/length
// parsing, role-correct masking validation, control-frame and length-limit
// validation, and in-place unmasking when the peer is a client.
// It never performs I/O and never throws for peer bytes; callers append transport
// input after need_input(), while failure() carries the Close reason. A returned
// frame stays in the buffer, counted by `pending_consume`, until the next call.
[[nodiscard]] inline websocket_frame_read_result websocket_try_read_frame(http_io_buffer& buffer RUVIA_LIFETIMEBOUND, std::size_t& pending_consume, protocol_byte_limit message_limit, bool permessage_deflate, bool expect_masked) {
    consume_websocket_read_frame(buffer, pending_consume);
    const auto raw = buffer.mutable_bytes();
    const auto available = raw.size();
    if (available < 2) {
        return websocket_frame_read_result::make_need_input();
    }
    std::uint64_t length = std::to_integer<unsigned int>(raw[1]) & 0x7FU;
    std::size_t header_size = 2;

    const auto frame_start = decode_websocket_frame_start(raw[0], raw[1], permessage_deflate, expect_masked);
    if (!frame_start.has_value()) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    // RFC 6455 section 5.2 obliges the sender to use the minimal length form;
    // the frame stays unambiguous either way, so a receiver does not reject a
    // longer form (sender MUST, not recipient rule).
    if (length == 126) {
        if (available < header_size + 2) {
            return websocket_frame_read_result::make_need_input();
        }
        length = detail::read_websocket_uint16(raw.subspan(header_size, 2));
        header_size += 2;
    } else if (length == 127) {
        if (available < header_size + 8) {
            return websocket_frame_read_result::make_need_input();
        }
        const auto decoded_length = detail::read_websocket_uint64(std::span<const std::byte, 8>(raw.data() + header_size, 8));
        if ((decoded_length.index() != 0)) {
            return websocket_frame_read_result::make_failure(std::get<1>(decoded_length));
        }
        length = std::get<0>(decoded_length);
        header_size += 8;
    }

    if (detail::is_invalid_websocket_control_frame(*frame_start, length)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    if (detail::websocket_frame_exceeds_message_limit(frame_start->kind(), length, message_limit)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::message_too_large);
    }
    const auto mask_bytes = expect_masked ? std::size_t{4} : std::size_t{0};
    // header_size is at most 10, so only the payload length can overflow.
    if (length > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)() - header_size - mask_bytes)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    const auto total_frame_bytes = header_size + mask_bytes + static_cast<std::size_t>(length);
    if (available < total_frame_bytes) {
        return websocket_frame_read_result::make_need_input(total_frame_bytes - available);
    }

    const auto mask_offset = header_size;
    const auto payload_offset = mask_offset + mask_bytes;
    const auto payload_size = static_cast<std::size_t>(length);
    const auto payload_slice = raw.subspan(payload_offset, payload_size);
    if (expect_masked) {
        mask_websocket_payload(payload_slice, std::span<const std::byte, 4>(raw.data() + mask_offset, 4));
    }
    if (frame_start->kind() == websocket_frame_kind::close) {
        if (const auto failure = websocket_close_payload_failure(byte_view(payload_slice))) {
            return websocket_frame_read_result::make_failure(*failure);
        }
    }
    pending_consume = total_frame_bytes;
    return websocket_frame_read_result::make_frame(*frame_start, std::string_view(buffer.data() + payload_offset, payload_size));
}

}  // namespace ruvia
