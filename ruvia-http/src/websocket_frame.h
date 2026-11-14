#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

#include "borrowed_text.h"
#include "http_io_buffer.h"
#include "http_protocol.h"
#include "websocket_connection.h"
#include "websocket_deflate.h"

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
    friend std::optional<websocket_frame_start> decode_websocket_frame_start(unsigned char, unsigned char, bool, bool) noexcept;

    constexpr websocket_frame_start(websocket_frame_kind kind, bool final, bool compressed) noexcept :
        kind_(kind),
        final_(final),
        compressed_(compressed) {}

    websocket_frame_kind kind_;
    bool final_;
    bool compressed_;
};

[[nodiscard]] inline bool is_invalid_websocket_raw_opcode(std::uint8_t raw_opcode) noexcept {
    return (raw_opcode >= 0x3 && raw_opcode <= 0x7) || raw_opcode >= 0xB;
}

[[nodiscard]] inline bool is_websocket_control_frame_kind(websocket_frame_kind kind) noexcept {
    return static_cast<std::uint8_t>(kind) >= 0x8;
}

// allow_rsv1 enables the RSV1 (compressed) bit when permessage-deflate is
// negotiated; it is valid only on the first frame of a data message, never on a
// continuation or control frame (RFC 7692 §6.1). RSV2/RSV3 are always rejected.
[[nodiscard]] inline std::optional<websocket_frame_start> decode_websocket_frame_start(unsigned char first, unsigned char second, bool allow_rsv1, bool expect_masked) noexcept {
    const auto raw_opcode = static_cast<std::uint8_t>(first & 0x0FU);
    const bool rsv1 = (first & 0x40U) != 0;
    if ((first & 0x30U) != 0 || ((second & 0x80U) != 0) != expect_masked || is_invalid_websocket_raw_opcode(raw_opcode)) {
        return std::nullopt;
    }
    if (rsv1 && (!allow_rsv1 || raw_opcode == 0 || raw_opcode >= 0x8)) {
        return std::nullopt;
    }
    return websocket_frame_start(static_cast<websocket_frame_kind>(raw_opcode), (first & 0x80U) != 0, rsv1);
}

[[nodiscard]] inline bool is_invalid_websocket_control_frame(const websocket_frame_start& frame, std::uint64_t payload_size) noexcept {
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
// (Text/Binary/Continuation) are measured against max_message_bytes_. Applying the
// limit to control frames would reject a legal Ping/Pong, or a Close carrying a
// reason phrase, once max_message_bytes_ drops below 125 -- silently breaking the
// close handshake and keepalive on a small-message configuration.
[[nodiscard]] inline bool websocket_frame_exceeds_message_limit(websocket_frame_kind kind, std::uint64_t payload_size, protocol_byte_limit message_limit) noexcept {
    return !is_websocket_control_frame_kind(kind) && websocket_frame_length_exceeds_limit(payload_size, message_limit);
}

[[nodiscard]] inline std::uint16_t read_websocket_uint16(const char* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(static_cast<unsigned char>(data[0])) << 8) | static_cast<unsigned char>(data[1]));
}

[[nodiscard]] constexpr std::variant<std::uint64_t, websocket_protocol_failure> read_websocket_uint64(std::span<const char, 8> data) noexcept {
    if ((static_cast<unsigned char>(data[0]) & 0x80U) != 0) {
        return websocket_protocol_failure::protocol_error;
    }
    std::uint64_t value = 0;
    for (const char byte : data) {
        value = (value << 8) | static_cast<unsigned char>(byte);
    }
    return value;
}

// `opcode_bits` is 0 for a continuation frame (RFC 6455 section 5.4).
[[nodiscard]] inline std::size_t encode_websocket_frame_header(websocket_frame_header_type& header_value, std::uint8_t opcode_bits, bool final, std::size_t payload_size, bool rsv1, bool masked) noexcept {
    std::size_t header_size = 0;
    header_value[header_size++] = static_cast<char>((final ? 0x80U : 0U) | (rsv1 ? 0x40U : 0U) | opcode_bits);
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

// XORs `payload_value` with the 4-byte key in place, starting at key offset 0.
// The key is replicated into wider words loaded and stored through memcpy, which
// is valid at any alignment and aliasing and keeps the byte order intact on any
// endianness; the tail shorter than a word is masked bytewise.
inline void decode_masked_websocket_payload(char* payload_value, std::size_t payload_size, const char* mask) noexcept {
    std::array<char, 8> key_bytes;
    std::memcpy(key_bytes.data(), mask, 4);
    std::memcpy(key_bytes.data() + 4, mask, 4);
    std::uint64_t key;
    std::memcpy(&key, key_bytes.data(), sizeof(key));
    std::size_t i = 0;
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    const auto wide_key = _mm_set1_epi64x(static_cast<long long>(key));
    for (; payload_size - i >= 16; i += 16) {
        auto* const block = reinterpret_cast<__m128i*>(payload_value + i);
        _mm_storeu_si128(block, _mm_xor_si128(_mm_loadu_si128(block), wide_key));
    }
#endif
    for (; payload_size - i >= sizeof(key); i += sizeof(key)) {
        std::uint64_t word;
        std::memcpy(&word, payload_value + i, sizeof(word));
        word ^= key;
        std::memcpy(payload_value + i, &word, sizeof(word));
    }
    for (; i < payload_size; ++i) {
        payload_value[i] = static_cast<char>(payload_value[i] ^ mask[i & 3U]);
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
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
        return std::string_view(bytes_.data(), size_);
    }
    [[nodiscard]] constexpr std::string_view bytes() const&& = delete;

private:
    friend class websocket_close_payload_encode_result;
    friend websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code, std::string_view) noexcept;

    websocket_encoded_close_payload(websocket_close_code code, std::string_view reason) noexcept;

    std::array<char, 125> bytes_;
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
    [[nodiscard]] constexpr const websocket_encoded_close_payload* encoded() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] constexpr const websocket_encoded_close_payload* encoded() const&& = delete;

    [[nodiscard]] constexpr const websocket_close_payload_encode_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] constexpr const websocket_close_payload_encode_failure* failure() const&& = delete;

private:
    friend websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code, std::string_view) noexcept;

    explicit websocket_close_payload_encode_result(websocket_encoded_close_payload encoded) noexcept :
        value_(encoded) {}

    explicit constexpr websocket_close_payload_encode_result(websocket_close_payload_encode_failure failure) noexcept :
        value_(failure) {}

    using value_type = std::variant<websocket_encoded_close_payload, websocket_close_payload_encode_failure>;
    value_type value_;
};

[[nodiscard]] websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code code, std::string_view reason) noexcept;

// The protocol failure a received Close payload commits, or nullopt when it is legal.
[[nodiscard]] std::optional<websocket_protocol_failure> websocket_close_payload_failure(std::string_view payload_value) noexcept;

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
// one typed protocol failure. No default frame, EOF boolean, or wire-format
// exception can coexist with another outcome; only need_input() carries a
// byte count.
class websocket_frame_read_result final {
public:
    [[nodiscard]] constexpr const websocket_frame_need_input* need_input() const& noexcept {
        return std::get_if<websocket_frame_need_input>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_need_input* need_input() const&& = delete;

    [[nodiscard]] constexpr const websocket_frame_view* frame() const& noexcept {
        return std::get_if<websocket_frame_view>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_view* frame() const&& = delete;

    [[nodiscard]] constexpr const websocket_frame_read_failure* failure() const& noexcept {
        return std::get_if<websocket_frame_read_failure>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_read_failure* failure() const&& = delete;

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
[[nodiscard]] inline websocket_frame_read_result websocket_try_read_frame(http_io_buffer& buffer, std::size_t& pending_consume, protocol_byte_limit message_limit, bool permessage_deflate, bool expect_masked) {
    consume_websocket_read_frame(buffer, pending_consume);
    const auto available = buffer.size();
    if (available < 2) {
        return websocket_frame_read_result::make_need_input();
    }
    const auto first = static_cast<unsigned char>(buffer.data()[0]);
    const auto second = static_cast<unsigned char>(buffer.data()[1]);
    std::uint64_t length = second & 0x7FU;
    std::size_t header_size = 2;

    const auto frame_start = decode_websocket_frame_start(first, second, permessage_deflate, expect_masked);
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
        length = read_websocket_uint16(buffer.data() + header_size);
        header_size += 2;
    } else if (length == 127) {
        if (available < header_size + 8) {
            return websocket_frame_read_result::make_need_input();
        }
        const auto decoded_length = read_websocket_uint64(std::span<const char, 8>(buffer.data() + header_size, 8));
        if ((decoded_length.index() != 0)) {
            return websocket_frame_read_result::make_failure(std::get<1>(decoded_length));
        }
        length = std::get<0>(decoded_length);
        header_size += 8;
    }

    if (is_invalid_websocket_control_frame(*frame_start, length)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    if (websocket_frame_exceeds_message_limit(frame_start->kind(), length, message_limit)) {
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
    auto* payload_value = buffer.data() + payload_offset;
    if (expect_masked) {
        decode_masked_websocket_payload(payload_value, payload_size, buffer.data() + mask_offset);
    }
    const auto payload_view = std::string_view(payload_value, payload_size);
    if (frame_start->kind() == websocket_frame_kind::close) {
        if (const auto failure = websocket_close_payload_failure(payload_view)) {
            return websocket_frame_read_result::make_failure(*failure);
        }
    }
    pending_consume = total_frame_bytes;
    return websocket_frame_read_result::make_frame(*frame_start, payload_view);
}

class websocket_inbound_result;

class websocket_inbound_continue final {
private:
    friend class websocket_inbound_result;
    constexpr websocket_inbound_continue() noexcept = default;
};

class websocket_inbound_control_frame final {
public:
    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class websocket_inbound_result;

    constexpr websocket_inbound_control_frame(websocket_opcode opcode, std::string_view payload_value) noexcept :
        opcode_(opcode),
        payload_(payload_value) {}

    websocket_opcode opcode_;
    std::string_view payload_;
};

enum class websocket_inbound_content_encoding : std::uint8_t {
    identity,
    per_message_deflate,
};

class websocket_inbound_message final {
public:
    [[nodiscard]] constexpr const websocket_message& message() const& noexcept {
        return message_;
    }
    [[nodiscard]] constexpr const websocket_message& message() const&& = delete;

    [[nodiscard]] constexpr websocket_inbound_content_encoding content_encoding() const noexcept {
        return content_encoding_;
    }

private:
    friend class websocket_inbound_result;

    constexpr websocket_inbound_message(websocket_message message, websocket_inbound_content_encoding content_encoding) noexcept :
        message_(message),
        content_encoding_(content_encoding) {}

    websocket_message message_;
    websocket_inbound_content_encoding content_encoding_;
};

class websocket_inbound_failure final {
public:
    [[nodiscard]] constexpr websocket_protocol_failure error() const noexcept {
        return error_;
    }

private:
    friend class websocket_inbound_result;

    explicit constexpr websocket_inbound_failure(websocket_protocol_failure error) noexcept :
        error_(error) {}

    websocket_protocol_failure error_;
};

// A consumed frame has exactly one semantic outcome. Control payload, application
// message, and failure code live only on their corresponding alternatives; there
// is no action enum coupled to an output parameter.
class websocket_inbound_result final {
public:
    [[nodiscard]] constexpr const websocket_inbound_continue* continue_reading() const& noexcept {
        return std::get_if<websocket_inbound_continue>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_continue* continue_reading() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_control_frame* control_frame() const& noexcept {
        return std::get_if<websocket_inbound_control_frame>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_control_frame* control_frame() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_message* message() const& noexcept {
        return std::get_if<websocket_inbound_message>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_message* message() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_failure* failure() const& noexcept {
        return std::get_if<websocket_inbound_failure>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_failure* failure() const&& = delete;

private:
    friend class websocket_inbound_assembler;

    using value_type = std::variant<websocket_inbound_continue, websocket_inbound_control_frame, websocket_inbound_message, websocket_inbound_failure>;

    template <typename alternative_type>
    explicit constexpr websocket_inbound_result(alternative_type alternative) noexcept :
        value_(alternative) {}

    [[nodiscard]] static constexpr websocket_inbound_result make_continue() noexcept {
        return websocket_inbound_result(websocket_inbound_continue());
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_control_frame(websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_inbound_result(websocket_inbound_control_frame(opcode, payload_value));
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_message(websocket_message message, websocket_inbound_content_encoding content_encoding) noexcept {
        return websocket_inbound_result(websocket_inbound_message(message, content_encoding));
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_failure(websocket_protocol_failure error) noexcept {
        return websocket_inbound_result(websocket_inbound_failure(error));
    }

    value_type value_;
};

// Single owner of the websocket inbound message-reassembly state machine (RFC
// 6455 §5.4): control-frame dispatch, fragmentation across continuation frames,
// per-message size limits and UTF-8 validation. Every wire failure is returned as
// a typed alternative carrying the exact RFC Close reason; none is thrown.
struct websocket_inbound_idle final {};

class websocket_inbound_fragmented final {
public:
    constexpr websocket_inbound_fragmented(websocket_opcode opcode, websocket_inbound_content_encoding encoding) noexcept :
        opcode_(opcode),
        encoding_(encoding) {}

    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }
    [[nodiscard]] constexpr websocket_inbound_content_encoding encoding() const noexcept {
        return encoding_;
    }

private:
    websocket_opcode opcode_;
    websocket_inbound_content_encoding encoding_;
};

class websocket_inbound_assembler final {
public:
    websocket_inbound_assembler() = default;

    // A delivered view expires at the next poll, while a fragmented message
    // remains pinned until its final continuation or connection teardown. A
    // delivered message keeps only a bounded scratch, so streams of small
    // fragmented messages do not allocate per message and large ones are not
    // pinned to the connection.
    void release_completed(websocket_receive_storage& storage) noexcept;

    [[nodiscard]] bool fragmented() const noexcept {
        return std::holds_alternative<websocket_inbound_fragmented>(state_);
    }

    [[nodiscard]] websocket_inbound_result accept(const websocket_frame_view& frame, protocol_byte_limit message_limit, websocket_receive_storage& storage);

private:
    std::string message_;
    std::variant<websocket_inbound_idle, websocket_inbound_fragmented> state_;
};

}  // namespace ruvia::detail
