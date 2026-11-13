#pragma once

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/http/websocket_protocol.h"

#include "http_connection_fields.h"
#include "http_wire_sink.h"
#include "http_zlib_stream.h"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

#include <type_traits>

#include "ruvia/http/http_client.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_request_content_semantics.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

class http_request;

namespace detail {

// The base64-encoded SHA-1 of the client key plus the RFC 6455 GUID, as it
// appears in the Sec-websocket-Accept response field (always 28 bytes).
using websocket_accept_key_type = std::array<char, 28>;

// Throws std::length_error before inspecting the view when key + the RFC
// GUID cannot be represented by the SHA-1 64-bit bit-length field or by the
// size_t arithmetic used by the incremental implementation.
void encode_websocket_accept(websocket_accept_key_type& output, std::string_view key);

[[nodiscard]] bool append_websocket_extension_list(
    std::string_view value, bool& has_extension) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(
    const websocket_subprotocol_set& offered,
    std::span<const std::string_view> supported) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(
    const http_request& request, std::span<const std::string_view> supported) noexcept;

// Request-side Sec-WebSocket-* field checks. A recipient (server) skips a
// repeated offered subprotocol; a sender (client) must offer unique ones
// (RFC 6455 section 4.1).
class websocket_request_header_validator final {
public:
    explicit websocket_request_header_validator(http_field_list_role role) noexcept
        : role_(role) {}

    [[nodiscard]] bool observe_header(http_field_kind kind, std::string_view value) noexcept;
    [[nodiscard]] bool offers_valid() const noexcept;
    [[nodiscard]] bool client_offers_valid() const noexcept;
    [[nodiscard]] websocket_handshake_validation_result finish(
        websocket_handshake_validation_options options = {}) const noexcept;

private:
    websocket_subprotocol_set protocols_{};
    http_field_list_role role_;
    std::string_view version_{};
    bool version_present_{false};
    bool valid_{true};
    bool protocols_present_{false};
    bool extensions_present_{false};
    bool has_extension_{false};
};

[[nodiscard]] websocket_handshake_validation_result validate_extended_connect_websocket_headers(
    const http_request& request, websocket_handshake_validation_options options = {}) noexcept;
}  // namespace detail
}  // namespace ruvia

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

[[nodiscard]] inline bool is_invalid_websocket_control_frame(
    const websocket_frame_start& frame, std::uint64_t payload_size) noexcept {
    return is_websocket_control_frame_kind(frame.kind()) && (!frame.final() || payload_size > 125);
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

// `opcode_bits` is 0 for a continuation frame (RFC 6455 section 5.4).
[[nodiscard]] inline std::size_t encode_websocket_frame_header(websocket_frame_header_type& header_value,
    std::uint8_t opcode_bits, bool final, std::size_t payload_size, bool rsv1, bool masked) noexcept {
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
inline void decode_masked_websocket_payload(
    char* payload_value, std::size_t payload_size, const char* mask) noexcept {
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
}  // namespace ruvia::detail

#pragma once

// The one RFC 7692 whole-message deflate and inflate loop, shared by
// per-connection codecs and the shared no-context-takeover codec set. Context
// takeover is the owner's decision: these functions neither reset nor keep the
// state between messages.

namespace ruvia::detail {

// zlib cannot emit an 8-bit raw window, so an 8-bit sender window uses zlib's
// 9-bit window restricted to Huffman coding, which never refers back at all.
[[nodiscard]] constexpr int websocket_deflate_zlib_window_bits(int window_bits) noexcept {
    return -std::max(window_bits, 9);
}

[[nodiscard]] constexpr int websocket_deflate_zlib_strategy(int window_bits) noexcept {
    return window_bits == 8 ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY;
}

// Compresses a message, or one fragment of it, up to a sync flush and appends
// the raw-DEFLATE blocks to `out`. At the message end the trailing
// 0x00 0x00 0xFF 0xFF flush marker is removed (RFC 7692 §7.2.1); earlier
// fragments keep it, which the receiver inflates as an empty stored block.
[[nodiscard]] inline bool websocket_deflate_message(zlib_deflate_stream& deflater, std::string_view input,
    std::string& out, bool message_end = true) {
    std::optional<std::string> owned_input;
    if (http_text_aliases_string(input, out)) {
        input = owned_input.emplace(input);
    }
    auto& stream = deflater.native();
    const zlib_borrow_scope borrow(stream);
    const auto start = out.size();
    stream.avail_in = 0;
    std::size_t supplied = 0;
    char buffer[4096];
    for (;;) {
        zlib_refill_input(stream, input, supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const int status = deflate(&stream, supplied == input.size() ? Z_SYNC_FLUSH : Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR) {
            deflater.rethrow_allocation_failure();
            return false;
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        out.append(buffer, produced);
        if (stream.avail_out != 0 && stream.avail_in == 0 && supplied == input.size()) {
            break;
        }
        if (produced == 0 && stream.avail_in == before_input &&
            !(stream.avail_in == 0 && supplied < input.size())) {
            return false;
        }
    }
    if (!message_end) {
        return true;
    }
    if (out.size() - start >= 4) {
        out.resize(out.size() - 4);
    }
    if (out.size() == start) {
        out.push_back('\0');
    }
    return true;
}

// One inflate pass over `data`; the message limit covers everything appended
// to `out` since `start`.
[[nodiscard]] inline websocket_inflate_result websocket_inflate_chunk(zlib_inflate_stream& inflater, const char* data,
    std::size_t size, std::string& out, std::size_t start, protocol_byte_limit message_limit) {
    auto& stream = inflater.native();
    const zlib_borrow_scope borrow(stream);
    stream.avail_in = 0;
    std::size_t supplied = 0;
    char buffer[8192];
    for (;;) {
        zlib_refill_input(stream, std::string_view(data, size), supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_STREAM_END && status != Z_OK && status != Z_BUF_ERROR) {
            inflater.rethrow_allocation_failure();
            return websocket_inflate_result::error;
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (message_limit.addition_exceeds(out.size() - start, produced)) {
            return websocket_inflate_result::too_large;
        }
        out.append(buffer, produced);
        // RFC 7692 permits BFINAL=1 followed by another DEFLATE stream.
        // Preserve the LZ77 window across streams, including takeover
        // history, while discarding the terminated stream's padding bits.
        if (status == Z_STREAM_END && inflateResetKeep(&stream) != Z_OK) {
            return websocket_inflate_result::error;
        }
        if (stream.avail_out != 0 && stream.avail_in == 0 && supplied == size) {
            break;
        }
        if (produced == 0 && stream.avail_in == before_input &&
            !(stream.avail_in == 0 && supplied < size)) {
            return websocket_inflate_result::error;
        }
    }
    return websocket_inflate_result::ok;
}

// Decompresses a whole message: appends the 0x00 0x00 0xFF 0xFF marker that
// the sender stripped, then raw-inflates into `out`, bounded by one explicit
// message limit across the appended bytes to defuse decompression bombs
// (RFC 7692 §7.2.2).
[[nodiscard]] inline websocket_inflate_result websocket_inflate_message(zlib_inflate_stream& inflater,
    std::string_view input, std::string& out, protocol_byte_limit message_limit) {
    static constexpr unsigned char flush_marker[4] = {0x00, 0x00, 0xFF, 0xFF};
    std::optional<std::string> owned_input;
    if (http_text_aliases_string(input, out)) {
        input = owned_input.emplace(input);
    }
    const auto start = out.size();
    if (const auto result_value = websocket_inflate_chunk(
            inflater, input.data(), input.size(), out, start, message_limit);
        result_value != websocket_inflate_result::ok) {
        return result_value;
    }
    if (const auto result_value = websocket_inflate_chunk(inflater,
            reinterpret_cast<const char*>(flush_marker), sizeof(flush_marker), out, start, message_limit);
        result_value != websocket_inflate_result::ok) {
        return result_value;
    }
    // zlib reports bit 128 only at a complete DEFLATE block boundary.
    // Consuming every input byte alone does not detect a truncated block.
    return (inflater.native().data_type & 128) != 0 ? websocket_inflate_result::ok : websocket_inflate_result::error;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// RFC 7692 codec state of one connection. A direction negotiated without
// context takeover starts every message from an empty dictionary, so it uses
// the borrowed shared codec set when the owner supplies one and then holds no
// codec memory. Every other direction owns a private zlib stream,
// created on its first compressed message and kept,
// with its dictionary, until the connection ends. The owner validates the
// deflater (validate_websocket_deflater_config) and windows (8-15).
class websocket_deflate final {
public:
    websocket_deflate(websocket_deflate_codecs* shared,
        websocket_deflater_config deflater, bool send_takeover, bool receive_takeover, int send_window,
        int receive_window) noexcept
        : shared_(shared),
          deflater_config_(deflater),
          send_window_(send_window),
          receive_window_(receive_window),
          send_takeover_(send_takeover),
          receive_takeover_(receive_takeover) {}

    websocket_deflate(const websocket_deflate&) = delete;
    websocket_deflate& operator=(const websocket_deflate&) = delete;

    // Appends the compressed message to `out`. false reports a codec failure;
    // allocation exceptions propagate.
    bool compress(std::string_view input, std::string& out) {
        if (!send_takeover_ && shared_ != nullptr) {
            // Reset after every call, including failures and exceptions.
            return shared_->deflate_message(input, out, deflater_config_.compression_level_, send_window_);
        }
        if (deflater_ == nullptr) {
            deflater_ = std::make_unique<zlib_deflate_stream>(
                deflater_config_.compression_level_, websocket_deflate_zlib_window_bits(send_window_),
                deflater_config_.mem_level_, websocket_deflate_zlib_strategy(send_window_));
            if (!deflater_->initialized()) {
                deflater_.reset();
                return false;
            }
        } else if (!send_takeover_ && !deflater_->reset()) {
            return false;
        }
        return websocket_deflate_message(*deflater_, input, out);
    }

    // Compresses one fragment of a message sent as several frames. The whole
    // message is one DEFLATE stream, so it uses a private codec even where
    // whole messages borrow the shared one, and starts from an empty
    // dictionary at its first fragment unless context takeover applies.
    bool compress_fragment(std::string_view input, bool first, bool final, std::string& out) {
        if (deflater_ == nullptr) {
            deflater_ = std::make_unique<zlib_deflate_stream>(
                deflater_config_.compression_level_, websocket_deflate_zlib_window_bits(send_window_),
                deflater_config_.mem_level_, websocket_deflate_zlib_strategy(send_window_));
            if (!deflater_->initialized()) {
                deflater_.reset();
                return false;
            }
        } else if (first && !send_takeover_ && !deflater_->reset()) {
            return false;
        }
        return websocket_deflate_message(*deflater_, input, out, final);
    }

    websocket_inflate_result decompress(
        std::string_view input, std::string& out, protocol_byte_limit message_limit) {
        if (!receive_takeover_ && shared_ != nullptr) {
            // The shared inflater has a 15-bit window, which also inflates
            // every smaller negotiated window.
            return shared_->inflate_message(input, out, message_limit);
        }
        if (inflater_ == nullptr) {
            inflater_ = std::make_unique<zlib_inflate_stream>(-receive_window_);
            if (!inflater_->initialized()) {
                inflater_.reset();
                return websocket_inflate_result::error;
            }
        } else if (!receive_takeover_ && !inflater_->reset()) {
            return websocket_inflate_result::error;
        }
        return websocket_inflate_message(*inflater_, input, out, message_limit);
    }

    // A trial that is not sent must not enter the peer's future dictionary.
    // Starting the next compressed message with an empty sender dictionary is
    // legal even when the receiver retains its prior dictionary (RFC 7692).
    // The shared codecs already reset after every message.
    void discard_compression() {
        if (deflater_ != nullptr && !deflater_->reset()) {
            throw std::runtime_error("failed to reset discarded WebSocket compression");
        }
    }

private:
    websocket_deflate_codecs* shared_;
    std::unique_ptr<zlib_deflate_stream> deflater_;
    std::unique_ptr<zlib_inflate_stream> inflater_;
    websocket_deflater_config deflater_config_;
    int send_window_;
    int receive_window_;
    bool send_takeover_;
    bool receive_takeover_;
};

// Decode RFC 7692 window parameters (8 through 15), including quoted tokens.
// The negotiated send and receive bounds are independent of takeover policy.
[[nodiscard]] inline std::optional<int> websocket_deflate_window_bits(
    std::string_view value) noexcept {
    value = http_trim_ows(value);
    int parsed_value = 0;
    std::size_t digits = 0;
    bool leading_zero = false;
    // RFC 7692 section 7.1.2 defines both max-window-bits parameters as
    // decimal integers without leading zeroes. Apply that grammar after
    // quoted-pair decoding as well as to tokens.
    const auto append_digit = [&](unsigned char ch) noexcept {
        if (ch < '0' || ch > '9' || ++digits > 2) {
            return false;
        }
        if (digits == 1) {
            leading_zero = ch == '0';
        } else if (leading_zero) {
            return false;
        }
        parsed_value = parsed_value * 10 + (ch - '0');
        return true;
    };
    if (!value.empty() && value.front() == '"') {
        http_quoted_string_cursor quoted(value);
        unsigned char byte = 0;
        while (quoted.next(byte)) {
            if (!append_digit(byte)) {
                return std::nullopt;
            }
        }
        if (!quoted.closed() || quoted.consumed() != value.size()) {
            return std::nullopt;
        }
    } else {
        for (const char ch : value) {
            if (!append_digit(static_cast<unsigned char>(ch))) {
                return std::nullopt;
            }
        }
    }
    return digits != 0 && parsed_value >= 8 && parsed_value <= 15 ? std::optional<int>(parsed_value) : std::nullopt;
}

// RFC 7692 section 7.1 parameters of one permessage-deflate extension element.
// client_max_window_bits may appear without a value; whether that is
// acceptable depends on the role, so its presence is kept apart from its value.
struct permessage_deflate_params final {
    bool server_no_context_takeover_{false};
    bool client_no_context_takeover_{false};
    std::optional<int> server_max_window_bits_;
    bool client_max_window_bits_present_{false};
    std::optional<int> client_max_window_bits_;
};

// Grammar shared by offers and responses: every parameter is defined, appears
// at most once and carries a value exactly where RFC 7692 defines one (an
// undefined or malformed parameter cannot be negotiated). Returns nullopt for
// another extension or a malformed element; role policy is left to callers.
[[nodiscard]] inline std::optional<permessage_deflate_params> parse_permessage_deflate_params(
    std::string_view element) noexcept {
    const auto name = http_header_token_before_parameters(element);
    if (!http_ascii_equals_ignore_case(name, "permessage-deflate")) {
        return std::nullopt;
    }

    permessage_deflate_params params;
    const bool valid = http_all_parameters(element, [&params](std::string_view parameter) noexcept {
        const auto equals = parameter.find('=');
        const bool has_value = equals != std::string_view::npos;
        const auto parameter_name = http_trim_ows(parameter.substr(0, equals));
        const auto parameter_value =
            has_value ? http_trim_ows(parameter.substr(equals + 1)) : std::string_view{};

        switch (parameter_name.size()) {
            case 22:
                if (http_ascii_equals_ignore_case(parameter_name, "server_max_window_bits")) {
                    // A missing or empty value fails window decoding.
                    if (params.server_max_window_bits_.has_value()) {
                        return false;
                    }
                    params.server_max_window_bits_ = websocket_deflate_window_bits(parameter_value);
                    return params.server_max_window_bits_.has_value();
                }
                if (http_ascii_equals_ignore_case(parameter_name, "client_max_window_bits")) {
                    if (params.client_max_window_bits_present_) {
                        return false;
                    }
                    params.client_max_window_bits_present_ = true;
                    if (has_value) {
                        params.client_max_window_bits_ = websocket_deflate_window_bits(parameter_value);
                        return params.client_max_window_bits_.has_value();
                    }
                    return true;
                }
                return false;
            case 26:
                if (http_ascii_equals_ignore_case(parameter_name, "server_no_context_takeover")) {
                    if (params.server_no_context_takeover_ || has_value) {
                        return false;
                    }
                    params.server_no_context_takeover_ = true;
                    return true;
                }
                if (http_ascii_equals_ignore_case(parameter_name, "client_no_context_takeover")) {
                    if (params.client_no_context_takeover_ || has_value) {
                        return false;
                    }
                    params.client_no_context_takeover_ = true;
                    return true;
                }
                return false;
            default:
                // Includes an empty parameter, such as a trailing ';'.
                return false;
        }
    });
    return valid ? std::optional(params) : std::nullopt;
}

// Server role: an offered client_max_window_bits, with or without a value, is
// accepted but never echoed in the response.
[[nodiscard]] inline std::optional<websocket_compression> websocket_parse_deflate_offer(
    std::string_view offer, bool context_takeover) noexcept {
    const auto params = parse_permessage_deflate_params(offer);
    if (!params.has_value()) {
        return std::nullopt;
    }
    websocket_compression accepted{.enabled_ = true, .server_max_window_bits_ = params->server_max_window_bits_};
    if (context_takeover) {
        accepted.server_no_context_takeover_ = params->server_no_context_takeover_;
        accepted.client_no_context_takeover_ = params->client_no_context_takeover_;
    }
    return accepted;
}

[[nodiscard]] inline websocket_compression websocket_scan_deflate_offers(
    std::string_view offers, bool context_takeover) noexcept {
    std::optional<websocket_compression> accepted;
    http_visit_comma_separated_quoted_field_items(offers, [&accepted, context_takeover](std::string_view offer) noexcept {
        if (offer.empty()) {
            return true;
        }
        accepted = websocket_parse_deflate_offer(offer, context_takeover);
        return !accepted.has_value();
    });
    return accepted.value_or((websocket_compression{}));
}

[[nodiscard]] inline websocket_compression websocket_negotiate_permessage_deflate(
    const http_request& request, websocket_deflate_config config) {
    validate_websocket_deflater_config(config.deflater_);
    if (!config.enabled_) {
        return (websocket_compression{});
    }
    // RFC 6455 §9.1: extension declarations may be split across multiple
    // Sec-WebSocket-Extensions field lines, which RFC 9110 §5.3 makes equivalent to
    // one comma-joined list. Scan every line in order, validating the whole
    // extension list while honoring the first acceptable permessage-deflate offer.
    bool has_extension = false;
    websocket_compression negotiated{};
    const bool valid = http_request_access::visit_known_headers(request,
        http_field_kind::sec_websocket_extensions,
        [&has_extension, &negotiated, &config](std::string_view value) noexcept {
            if (!append_websocket_extension_list(value, has_extension)) {
                return false;
            }
            if (!negotiated.enabled_) {
                negotiated = websocket_scan_deflate_offers(value, config.context_takeover_);
            }
            return true;
        });
    return valid && has_extension ? negotiated : (websocket_compression{});
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// HTTP-version-independent result of one server-side websocket negotiation. HTTP/1
// and RFC 8441 consume this same immutable value for their response head, and the
// subsequent ws_connection consumes its exact deflate alternative. This prevents
// response metadata and frame RSV1 semantics from being configured separately.
// The selected subprotocol is copied into owned storage so this
// committed value never borrows mutable route configuration.
class websocket_server_negotiation final {
public:
    websocket_server_negotiation(const websocket_server_negotiation&) = delete;
    websocket_server_negotiation& operator=(const websocket_server_negotiation&) = delete;
    websocket_server_negotiation(websocket_server_negotiation&&) noexcept = default;
    websocket_server_negotiation& operator=(websocket_server_negotiation&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

    [[nodiscard]] std::string_view extensions() const& noexcept {
        return extensions_.view();
    }
    std::string_view extensions() const&& = delete;

    [[nodiscard]] std::span<const http_header> response_headers() const& noexcept {
        return response_headers_;
    }
    std::span<const http_header> response_headers() const&& = delete;

    [[nodiscard]] std::string take_subprotocol() && noexcept {
        return std::move(subprotocol_);
    }

    [[nodiscard]] std::vector<http_header> take_response_headers() && noexcept {
        return std::move(response_headers_);
    }

private:
    friend websocket_server_negotiation make_websocket_server_negotiation(
        const http_request&, const websocket_server_handshake_options&);

    websocket_server_negotiation(std::string_view subprotocol, websocket_compression compression,
        std::span<const http_header_view> response_headers_value);

    std::string subprotocol_;
    websocket_compression compression_;
    websocket_compression_extension extensions_;
    std::vector<http_header> response_headers_;
};

// Copies the selected subprotocol and response fields.
[[nodiscard]] websocket_server_negotiation make_websocket_server_negotiation(
    const http_request& request, const websocket_server_handshake_options& options);

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] constexpr http_response_content_semantics classify_http_response_content_semantics(
    http_known_method request_method, http_status_code status_code) noexcept {
    if (status_code == http_status::switching_protocols) {
        return http_response_content_semantics::protocol_switch;
    }
    if (status_code.is_informational()) {
        return http_response_content_semantics::informational;
    }
    if (request_method == http_known_method::connect && status_code.is_successful()) {
        return http_response_content_semantics::connect_tunnel;
    }
    if (request_method == http_known_method::head || status_code == http_status::no_content ||
        status_code == http_status::not_modified) {
        return http_response_content_semantics::without_content;
    }
    return http_response_content_semantics::with_content;
}

struct http_response_body_access final {
    [[nodiscard]] static const http_response_body& body(const http_response& response) noexcept {
        return response.body_;
    }
    [[nodiscard]] static const http_response_body& body(const http_response&& response) = delete;
};

[[nodiscard]] inline const http_response_body& response_body(const http_response& response) noexcept {
    return http_response_body_access::body(response);
}
[[nodiscard]] const http_response_body& response_body(const http_response&& response) = delete;

struct http_response_header_state_access final {
    [[nodiscard]] static std::uint32_t known_bits(const http_response_headers& headers) noexcept {
        return headers.known_header_bits_;
    }

    [[nodiscard]] static const http_response_header* known_header(
        const http_response_headers& headers, std::uint32_t bit) noexcept {
        return headers.find_header_for_read({}, bit);
    }

    [[nodiscard]] static std::string_view known_header_value(
        const http_response_headers& headers, std::uint32_t bit) noexcept {
        const auto* const header_value = known_header(headers, bit);
        return header_value == nullptr ? std::string_view{} : header_value->value();
    }
};

struct http_response_header_access final {
    [[nodiscard]] static constexpr http_response_header make(const char* bytes_value, std::uint32_t name_size,
        std::uint32_t value_size, std::uint32_t known_bit, bool owned) noexcept {
        http_response_header header_value{};
        header_value.bytes_ = bytes_value;
        header_value.name_size_ = name_size;
        header_value.value_size_ = value_size;
        header_value.known_bit_ = known_bit;
        header_value.owned_ = owned;
        header_value.append_ = false;
        return header_value;
    }

    [[nodiscard]] static char* value_begin(http_response_header& header_value) noexcept {
        if (header_value.bytes_ == nullptr) {
            return nullptr;
        }
        return const_cast<char*>(header_value.bytes_) + header_value.name_size_;
    }

    [[nodiscard]] static char* value_end(http_response_header& header_value) noexcept {
        auto* const begin = value_begin(header_value);
        return begin == nullptr ? nullptr : begin + header_value.value_size_;
    }

    [[nodiscard]] static std::uint32_t known_bit(const http_response_header& header_value) noexcept {
        return header_value.known_bit_;
    }

    [[nodiscard]] static bool append(const http_response_header& header_value) noexcept {
        return header_value.append_;
    }

    static void set_append(http_response_header& header_value) noexcept {
        header_value.append_ = true;
    }
};

[[nodiscard]] inline std::uint32_t response_known_header_bits(const http_response& response) noexcept {
    return http_response_header_state_access::known_bits(response.headers());
}

[[nodiscard]] inline bool response_has_known_header(
    const http_response_headers& headers, std::uint32_t bit) noexcept {
    return (http_response_header_state_access::known_bits(headers) & bit) != 0;
}

[[nodiscard]] inline bool response_has_known_header(
    const http_response& response, std::uint32_t bit) noexcept {
    return response_has_known_header(response.headers(), bit);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// http_response_header stores both lengths in uint32_t and releases the one
// name/value allocation using their sum. Never let a public or runtime helper
// construct a descriptor whose wire/storage lengths cannot be represented;
// truncating either field would make the response header view disagree with
// the allocation and could turn a large application value into malformed wire
// bytes or an out-of-bounds read during emission.
[[nodiscard]] inline constexpr bool response_header_storage_size_fits(
    std::size_t name_size, std::size_t value_size) noexcept {
    constexpr auto max_size = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    return name_size <= max_size && value_size <= max_size && value_size <= max_size - name_size;
}

inline void validate_response_header_storage_size(std::size_t name_size, std::size_t value_size) {
    if (!response_header_storage_size_fits(name_size, value_size)) {
        throw std::length_error("HTTP response header is too large");
    }
}

// The one field-identity rule: known fields match by bit, unknown fields by
// case-insensitive name among unknown fields only.
[[nodiscard]] inline bool response_header_matches(
    const http_response_header& header_value, std::string_view key, std::uint32_t known_bit) noexcept {
    const auto header_known_bit = http_response_header_access::known_bit(header_value);
    return known_bit != 0 ? header_known_bit == known_bit
                          : header_known_bit == 0 && http_ascii_equals_ignore_case(header_value.name(), key);
}

[[nodiscard]] inline bool response_header_storage_overlaps(
    const http_response_header& header_value, std::string_view value) noexcept {
    const auto name = header_value.name();
    if (name.data() == nullptr) {
        return false;
    }
    return http_text_overlaps(
        {name.data(), name.size() + header_value.value().size()}, value);
}

[[nodiscard]] inline bool response_header_views_contiguous(
    std::string_view name, std::string_view value) noexcept {
    if (name.data() == nullptr) {
        return false;
    }
    if (value.empty()) {
        return true;
    }
    if (value.data() == nullptr) {
        return false;
    }
    return reinterpret_cast<std::uintptr_t>(value.data()) ==
           reinterpret_cast<std::uintptr_t>(name.data() + name.size());
}

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class http_binary_final_response_control_plan_error : std::uint8_t {
    invalid_status,
    upgrade_unavailable,
    connection_specific_field_forbidden
};

class http1_final_response_control;

[[nodiscard]] http1_final_response_control http1_final_response_control_plan(const http_response& response);

[[nodiscard]] std::optional<http_binary_final_response_control_plan_error> http_binary_final_response_control_plan(
    const http_response& response) noexcept;
// The protocol writer alone frames a response (RFC 9112 section 6.1, RFC 9113
// section 8.2.2), so an application Transfer-Encoding is accepted only as a
// hint that names exactly the single `chunked` coding, which every writer
// replaces with its own framing. Any other value claims a coding the writer
// never applies and fails closed on both protocols.
[[nodiscard]] inline bool response_transfer_encoding_replaceable(const http_response& response) noexcept {
    if (!response_has_known_header(response, response_header_transfer_encoding)) {
        return true;
    }
    return http_ascii_equals_ignore_case(
        http_trim_ows(http_response_header_state_access::known_header_value(
            response.headers(), response_header_transfer_encoding)),
        "chunked");
}

class http1_final_response_control final {
public:
    [[nodiscard]] http_connection_options connection_options() const noexcept {
        return connection_options_;
    }

    [[nodiscard]] http_upgrade_protocols upgrade_protocols() const noexcept {
        return upgrade_protocols_;
    }

private:
    friend http1_final_response_control http1_final_response_control_plan(const http_response&);

    http1_final_response_control(
        http_connection_options connection_options, http_upgrade_protocols upgrade_protocols) noexcept
        : connection_options_(connection_options),
          upgrade_protocols_(upgrade_protocols) {}

    http_connection_options connection_options_;
    http_upgrade_protocols upgrade_protocols_;
};

// Validate HTTP/1 control fields before the response mutates Connection state.
// Success owns the parsed repeated Connection and Upgrade fields. http_response
// already rejects a non-final status and malformed Connection, Upgrade and TE
// fields on insertion; these checks keep the commit self-contained.
[[nodiscard]] inline http1_final_response_control http1_final_response_control_plan(const http_response& response) {
    const auto status_code = response.status();
    if (!status_code.is_final()) {
        throw http1_response_head_error("invalid final HTTP response status");
    }
    if (!response_transfer_encoding_replaceable(response)) {
        throw http1_response_head_error("HTTP response Transfer-Encoding must be exactly chunked");
    }

    http_connection_options connection_options;
    http_upgrade_protocols upgrade_protocols;
    if (response_has_known_header(
            response, response_header_connection | response_header_upgrade)) {
        for (const auto& header : response.headers()) {
            switch (http_response_header_access::known_bit(header)) {
                case response_header_connection:
                    if (connection_options.parse_field(header.value(), http_connection_field_role::sender) !=
                        http_field_list_parse_status::ok) {
                        throw http1_response_head_error("invalid HTTP Connection header");
                    }
                    break;
                case response_header_upgrade:
                    if (upgrade_protocols.parse_field(header.value(), http_field_list_role::sender) !=
                        http_field_list_parse_status::ok) {
                        throw http1_response_head_error("invalid HTTP Upgrade header");
                    }
                    break;
                default:
                    break;
            }
        }
    }
    // RFC 9110 §15.5.22: a 426 response MUST send an Upgrade field.
    if (status_code == http_status::upgrade_required && !upgrade_protocols.has_protocol()) {
        throw http1_response_head_error("Upgrade Required response requires an Upgrade protocol");
    }
    return http1_final_response_control(connection_options, upgrade_protocols);
}

// Validate HTTP/2 and HTTP/3 control semantics before field encoding or stream
// state is mutated. Success proves that no connection-specific field exists;
// RFC 9113 section 8.2.2 and RFC 9114 section 4.2 require rejection rather than
// silent filtering. A replaceable Transfer-Encoding hint is not application
// metadata but writer-owned framing, so both encoders omit it.
[[nodiscard]] inline std::optional<http_binary_final_response_control_plan_error>
http_binary_final_response_control_plan(const http_response& response) noexcept {
    const auto status_code = response.status();
    if (!status_code.is_final()) {
        return http_binary_final_response_control_plan_error::invalid_status;
    }
    if (status_code == http_status::upgrade_required) {
        return http_binary_final_response_control_plan_error::upgrade_unavailable;
    }
    if (response_has_known_header(response,
            response_header_connection | response_header_upgrade | response_header_keep_alive |
                response_header_proxy_connection) ||
        !response_transfer_encoding_replaceable(response)) {
        return http_binary_final_response_control_plan_error::connection_specific_field_forbidden;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail

// Shared response Content-Length semantics. HTTP/1 adds its chunked and
// close-delimited framing policy; HTTP/2 and HTTP/3 delimit content with DATA
// frames but enforce the same normalized field value.

namespace ruvia::detail {

enum class http_framed_response_head_plan_error : std::uint8_t {
    invalid_content_length,
    connect_tunnel_required,
    response_status_mismatch,
    response_representation_mismatch
};

class http_framed_response_head_plan final {
public:
    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    // The normalized Content-Length field to encode, if any.
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept {
        return content_length_mode_ == content_length_mode_type::omit
                   ? std::nullopt
                   : std::optional<std::uint64_t>(content_length_);
    }

    // Only an application-declared streaming length constrains subsequent DATA.
    // Framework-generated buffered lengths are already bound to their response
    // representation and therefore do not create a streaming accounting limit.
    [[nodiscard]] std::optional<std::uint64_t> streaming_content_length() const noexcept {
        return content_length_mode_ == content_length_mode_type::explicit_value
                   ? std::optional<std::uint64_t>(content_length_)
                   : std::nullopt;
    }

    [[nodiscard]] static http_framed_response_head_plan canonical(
        http_response_body_plan body_plan, std::uint64_t value) noexcept {
        return http_framed_response_head_plan(body_plan, content_length_mode_type::canonical, value);
    }

    [[nodiscard]] static std::variant<http_framed_response_head_plan, http_framed_response_head_plan_error>
    preserve_explicit(http_response_body_plan body_plan, const http_response& response) noexcept {
        if (!response_has_known_header(response, response_header_content_length)) {
            return omit(body_plan);
        }

        const auto value = http_response_header_state_access::known_header_value(
            response.headers(), response_header_content_length);
        http_content_length_state state;
        if (state.parse_field(value) != http_content_length_parse_status::ok) {
            return http_framed_response_head_plan_error::invalid_content_length;
        }
        return http_framed_response_head_plan(
            body_plan, content_length_mode_type::explicit_value, *state.value());
    }

    [[nodiscard]] static http_framed_response_head_plan omit(http_response_body_plan body_plan) noexcept {
        return http_framed_response_head_plan(body_plan, content_length_mode_type::omit);
    }

private:
    enum class content_length_mode_type : std::uint8_t {
        omit,
        canonical,
        explicit_value,
    };

    http_framed_response_head_plan(http_response_body_plan body_plan,
        content_length_mode_type content_length_mode, std::uint64_t content_length = 0) noexcept
        : body_plan_(body_plan),
          content_length_mode_(content_length_mode),
          content_length_(content_length) {}

    http_response_body_plan body_plan_;
    content_length_mode_type content_length_mode_;
    std::uint64_t content_length_{0};
};

static_assert(std::is_trivially_copyable_v<http_framed_response_head_plan>);
static_assert(sizeof(http_framed_response_head_plan) <= 24);

using http_framed_response_head_plan_result =
    std::variant<http_framed_response_head_plan, http_framed_response_head_plan_error>;

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_buffered_response_head_plan(
    const http_response_body_plan& body_plan, std::uint64_t content_length,
    const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http_framed_response_head_plan_error::response_status_mismatch;
    }
    if (content_length != body_plan.buffered_representation_length(response)) {
        return http_framed_response_head_plan_error::response_representation_mismatch;
    }
    // An empty suppressed HEAD body describes the GET representation only
    // through its declared Content-Length, if any (RFC 9110 section 8.6).
    if (body_plan.body_suppressed() && body_plan.status_allows_body() &&
        body_plan.explicit_content_length_allowed() && content_length == 0 &&
        (response.has_head_metadata_body() ||
            response_has_known_header(response, response_header_content_length))) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    if (body_plan.auto_content_length_allowed()) {
        return http_framed_response_head_plan::canonical(
            body_plan, body_plan.status_allows_body() ? content_length : 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http_framed_response_head_plan::omit(body_plan);
}

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_buffered_response_head_plan(
    const http_buffered_response_write_plan& write_plan, const http_response& response) noexcept {
    return http_framed_buffered_response_head_plan(
        write_plan.body_plan(), write_plan.content_length(), response);
}

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_streaming_response_head_plan(
    const http_response_body_plan& body_plan, const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http_framed_response_head_plan_error::response_status_mismatch;
    }
    if (body_plan.auto_content_length_allowed() && !body_plan.status_allows_body()) {
        return http_framed_response_head_plan::canonical(body_plan, 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http_framed_response_head_plan::omit(body_plan);
}

// A successful CONNECT opens a tunnel and carries no Content-Length (RFC 9110
// §9.3.6); any other outcome must take the generic final-response path.
[[nodiscard]] inline http_framed_response_head_plan_result http_framed_connect_response_head_plan(
    const http_response_body_plan& body_plan) noexcept {
    if (body_plan.content_semantics() == ::ruvia::http_response_content_semantics::connect_tunnel) {
        return http_framed_response_head_plan::omit(body_plan);
    }
    return http_framed_response_head_plan_error::connect_tunnel_required;
}

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

// RFC 9113 §6.5.2 / RFC 9114 §4.2.2: decoded fields, including
// pseudo-fields, cost their name and value lengths plus 32 bytes.
// Rejected additions leave the accumulated size unchanged.
class binary_field_section_size final {
public:
    explicit constexpr binary_field_section_size(std::uint64_t limit) noexcept
        : limit_(limit) {}

    [[nodiscard]] bool add(std::string_view name, std::string_view value) noexcept {
        constexpr std::uint64_t field_metadata_bytes = 32;
        const auto remaining = limit_ - bytes_;
        if (remaining < field_metadata_bytes || name.size() > remaining - field_metadata_bytes ||
            value.size() > remaining - field_metadata_bytes - name.size()) {
            return false;
        }
        bytes_ += name.size() + value.size() + field_metadata_bytes;
        return true;
    }

private:
    std::uint64_t limit_;
    std::uint64_t bytes_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Borrow unchanged names; request writable storage only at the first uppercase
// byte. The provider must keep the input alive and return name.size() bytes.
template <typename prepare_type>
[[nodiscard]] inline std::string_view http_lower_header_name(
    std::string_view name, const prepare_type& prepare) {
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if (byte < 'A' || byte > 'Z') {
            continue;
        }
        auto* target = prepare(name.size());
        if (i != 0) {
            std::memcpy(target, name.data(), i);
        }
        for (std::size_t index = i; index < name.size(); ++index) {
            target[index] = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(name[index])));
        }
        return std::string_view(target, name.size());
    }
    return name;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

inline constexpr std::size_t http_origin_length_prefix_bytes = 2;
inline constexpr std::size_t http_max_prefixed_origin_length = 0xffff;

[[nodiscard]] inline bool reserve_http_prefixed_origin(
    std::size_t& remaining, std::string_view origin) noexcept {
    if (origin.size() > http_max_prefixed_origin_length ||
        remaining < http_origin_length_prefix_bytes ||
        origin.size() > remaining - http_origin_length_prefix_bytes) {
        return false;
    }
    remaining -= http_origin_length_prefix_bytes + origin.size();
    return true;
}

inline char* write_http_prefixed_origin(char* out, std::string_view origin) noexcept {
    *out++ = static_cast<char>(origin.size() >> 8);
    *out++ = static_cast<char>(origin.size());
    if (!origin.empty()) {
        std::memcpy(out, origin.data(), origin.size());
    }
    return out + origin.size();
}

// Shared RFC 8336 §2 / RFC 9412 §2 ORIGIN payload validator and serializer.
[[nodiscard]] inline std::variant<std::size_t, http_connection_advertisement_error>
http_origin_payload_size(std::span<const std::string_view> origins, std::size_t payload_limit) {
    std::size_t remaining = payload_limit;
    for (const auto origin : origins) {
        if (!is_valid_http_serialized_origin(origin)) {
            return http_connection_advertisement_error::invalid_origin;
        }
        if (!reserve_http_prefixed_origin(remaining, origin)) {
            return http_connection_advertisement_error::limit;
        }
    }
    return payload_limit - remaining;
}

inline void write_http_origin_payload(
    std::span<char> payload, std::span<const std::string_view> origins) noexcept {
    auto* out = payload.data();
    for (const auto origin : origins) {
        out = write_http_prefixed_origin(out, origin);
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Local HTTP message content accounting is distinct from the peer-body accounting
// stored on the same bidirectional stream. In particular, a server receives request
// content while independently producing response content.
class http_local_content_known_length final {
public:
    [[nodiscard]] constexpr std::uint64_t declared_length() const noexcept {
        return declared_length_;
    }

private:
    friend class http_local_content_state;

    explicit constexpr http_local_content_known_length(std::uint64_t declared_length) noexcept
        : declared_length_(declared_length) {}

    std::uint64_t declared_length_;
};

enum class http_local_content_check : std::uint8_t {
    accepted,
    not_started,
    forbidden,
    length_exceeded,
    length_incomplete
};

class http_local_content_state final {
    enum class phase_type : std::uint8_t { unset,
        forbidden,
        unbounded };

public:
    constexpr http_local_content_state() noexcept = default;

    void begin_forbidden() noexcept {
        reset(content_type(phase_type::forbidden));
    }

    void begin_unbounded() noexcept {
        reset(content_type(phase_type::unbounded));
    }

    void begin_known_length(std::uint64_t length) noexcept {
        reset(content_type(http_local_content_known_length(length)));
    }

    [[nodiscard]] constexpr const void* unset() const& noexcept {
        return has_phase(phase_type::unset) ? this : nullptr;
    }
    const void* unset() const&& = delete;

    [[nodiscard]] constexpr const void* forbidden() const& noexcept {
        return has_phase(phase_type::forbidden) ? this : nullptr;
    }
    const void* forbidden() const&& = delete;

    [[nodiscard]] constexpr const void* unbounded() const& noexcept {
        return has_phase(phase_type::unbounded) ? this : nullptr;
    }
    const void* unbounded() const&& = delete;

    [[nodiscard]] constexpr const http_local_content_known_length* known_length() const& noexcept {
        return std::get_if<http_local_content_known_length>(&content_);
    }
    const http_local_content_known_length* known_length() const&& = delete;

    // Transactional preflight for one submit_data input. No counters change here.
    // A terminal known-length submission must complete the declared length exactly;
    // callers can retry a rejected input with a corrected terminal flag or size.
    [[nodiscard]] http_local_content_check check_accept(
        std::size_t bytes_value, bool terminal) const noexcept {
        if (unset() != nullptr) {
            return http_local_content_check::not_started;
        }
        if (forbidden() != nullptr) {
            return http_local_content_check::forbidden;
        }

        const auto amount = static_cast<std::uint64_t>(bytes_value);
        if (amount > std::numeric_limits<std::uint64_t>::max() - accepted_bytes_) {
            return http_local_content_check::length_exceeded;
        }
        const auto* known_length_content = known_length();
        if (known_length_content == nullptr) {
            return http_local_content_check::accepted;
        }
        const auto declared_length = known_length_content->declared_length();
        if (accepted_bytes_ > declared_length || amount > declared_length - accepted_bytes_) {
            return http_local_content_check::length_exceeded;
        }
        if (terminal && accepted_bytes_ + amount != declared_length) {
            return http_local_content_check::length_incomplete;
        }
        return http_local_content_check::accepted;
    }

    void commit(std::size_t bytes_value) noexcept {
        accepted_bytes_ += static_cast<std::uint64_t>(bytes_value);
    }

    [[nodiscard]] bool length_complete() const noexcept {
        if (unset() != nullptr) {
            return false;
        }
        const auto* known_length_content = known_length();
        return known_length_content == nullptr ||
               accepted_bytes_ == known_length_content->declared_length();
    }

private:
    using content_type = std::variant<phase_type, http_local_content_known_length>;

    [[nodiscard]] constexpr bool has_phase(phase_type phase) const noexcept {
        const auto* current = std::get_if<phase_type>(&content_);
        return current != nullptr && *current == phase;
    }

    void reset(content_type content) noexcept {
        content_ = content;
        accepted_bytes_ = 0;
    }

    content_type content_{phase_type::unset};
    std::uint64_t accepted_bytes_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline bool http_binary_is_valid_outbound_regular_header(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && is_valid_http_header_value(value) &&
           !is_forbidden_http_binary_connection_field(name);
}

// Cross-field constraints only. Decoded :path syntax is already checked;
// senders combine this predicate with the origin-form grammar below.
[[nodiscard]] inline bool http_binary_request_path_matches_method_and_scheme(
    http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    if (path.empty()) {
        return !is_http_request_scheme(scheme);
    }
    return path != "*" || method == http_known_method::options;
}

[[nodiscard]] inline bool http_binary_is_valid_regular_request_path(
    http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    return http_binary_request_path_matches_method_and_scheme(method, scheme, path) &&
           (path.empty() || path == "*" || is_valid_origin_form_target(path));
}

[[nodiscard]] inline bool http_binary_is_valid_extended_connect_path(
    std::string_view scheme, std::string_view path) noexcept {
    return http_binary_request_path_matches_method_and_scheme(http_known_method::connect, scheme, path) &&
           (path.empty() || is_valid_origin_form_target(path));
}

[[nodiscard]] inline bool http_binary_is_valid_outbound_method(
    std::string_view method, http_known_method known_method) noexcept {
    if (known_method == http_known_method::connect) {
        return false;
    }
    return known_method != http_known_method::unknown || is_valid_http_method_token(method);
}

[[nodiscard]] inline bool http_binary_are_valid_outbound_request_headers(std::optional<std::string_view> authority,
    std::uint16_t default_port, std::span<const http_header_view> headers, bool allow_host,
    bool allow_trailers, binary_field_section_size& section_size, request_field_state& fields,
    http_request_field_source source, bool websocket_connect = false) noexcept {
    if (!is_valid_http_request_field_source(source)) {
        return false;
    }
    websocket_request_header_validator websocket_headers(http_field_list_role::sender);
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        if (!http_binary_is_valid_outbound_regular_header(header.name(), header.value()) ||
            !section_size.add(header.name(), header.value())) {
            return false;
        }
        const auto kind = classify_http_field(header.name());
        if ((!allow_trailers && (kind == http_field_kind::trailer || kind == http_field_kind::te)) ||
            (!allow_host && kind == http_field_kind::host)) {
            return false;
        }
        // Content-Length and Expect belong exclusively to the typed request
        // content and expectation plans; the validator refuses raw fields.
        if (validate_request_field(kind, header.value(), http_field_list_role::sender, fields, source) !=
            request_field_status::ok) {
            return false;
        }
        if (kind == http_field_kind::host && authority.has_value() &&
            !authority_matches_host(*authority, header.value(), default_port)) {
            return false;
        }
        // Host is already refused for websocket extended CONNECT by the outbound field
        // check (:authority is the sole target authority there).
        if (websocket_connect &&
            (kind == http_field_kind::sec_websocket_key ||
                (kind == http_field_kind::other &&
                    http_ascii_equals_ignore_case(header.name(), "sec-websocket-accept")) ||
                !websocket_headers.observe_header(kind, header.value()))) {
            return false;
        }
    }
    return !websocket_connect || websocket_headers.client_offers_valid();
}

[[nodiscard]] inline bool http_binary_is_valid_outbound_regular_request_head(std::string_view method,
    http_known_method known_method, std::string_view scheme, std::uint16_t default_port,
    std::optional<std::string_view> authority, std::string_view path,
    std::span<const http_header_view> headers,
    http_request_content_indication content_indication, std::string_view content_length_value,
    http_client_request_expectation expectation, http_request_field_source source, std::uint64_t limit) noexcept {
    if (!http_binary_is_valid_outbound_method(method, known_method) || !is_valid_uri_scheme(scheme) ||
        (authority.has_value() && !is_valid_binary_request_authority(scheme, *authority)) ||
        !http_binary_is_valid_regular_request_path(known_method, scheme, path) ||
        (!authority.has_value() && binary_regular_request_requires_authority(scheme, path))) {
        return false;
    }
    // An asterisk-form target carries no authority information; binary senders
    // omit :authority (RFC 9113 §8.3.1 / RFC 9114 §4.3.1).
    if (path == "*" && authority.has_value()) {
        return false;
    }
    binary_field_section_size section_size(limit);
    bool expect_continue = false;
    switch (expectation) {
        case http_client_request_expectation::none:
            break;
        case http_client_request_expectation::continue_value:
            expect_continue = true;
            break;
        default:
            return false;
    }
    if (!http_client_expectation_is_valid(expect_continue, content_indication) ||
        !section_size.add(":method", method) || !section_size.add(":scheme", scheme) ||
        (authority.has_value() && !section_size.add(":authority", *authority)) ||
        !section_size.add(":path", path) ||
        (expect_continue && !section_size.add("expect", http_continue_expectation_token))) {
        return false;
    }
    request_field_state fields{request_field_framing::binary};
    if (!http_binary_are_valid_outbound_request_headers(authority, default_port, headers,
            /*allow_host=*/true,
            /*allow_trailers=*/true, section_size, fields, source)) {
        return false;
    }
    if (!content_length_value.empty() && !section_size.add("content-length", content_length_value)) {
        return false;
    }
    if (content_indication == http_request_content_indication::no_content) {
        return true;
    }
    const auto content_semantics = classify_http_request_content_semantics(method);
    // RFC 9110 section 9.3.7 applies the Content-Type requirement to generation.
    return content_semantics != http_request_content_semantics::forbidden &&
           (source == http_request_field_source::forwarded ||
               content_semantics != http_request_content_semantics::content_type_required ||
               fields.seen(http_field_kind::content_type));
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Sinks expose clear(), reserve(count), add(lowercase_name, borrowed_value),
// add_lowercase(name, borrowed_value), and add_owned_value(lowercase_name, value).
// add_owned_value copies temporary formatting/date bytes; the other values must
// outlive submission. Each sink applies http_field_never_indexed to every field.
// Validation and size preflight complete before any sink mutation.
template <typename sink_type>
inline void append_http_binary_response_field(sink_type& fields, std::string_view name,
    std::string_view value, std::uint32_t known_bit) {
    const auto known_name = response_known_header_lowercase_name(known_bit);
    if (known_name.empty()) {
        fields.add_lowercase(name, value);
    } else {
        fields.add(known_name, value);
    }
}

[[nodiscard]] inline bool validate_http_binary_interim_response_fields(
    const http_interim_response_head& response, std::uint64_t limit) noexcept {
    const auto headers = response.headers();
    const auto status_token = http_status_code_token(response.status());
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", http_status_code_token_view(status_token))) {
        return false;
    }
    http_interim_response_sender_validator validator;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto name = headers[i].name();
        const auto value = headers[i].value();
        if (validator.validate(name, value) != http_interim_response_header_validation_status::ok ||
            is_forbidden_http_binary_response_field(name) || !section_size.add(name, value)) {
            return false;
        }
    }
    return true;
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_interim_response_fields(
    sink_type& fields, const http_interim_response_head& response, std::uint64_t limit) {
    if (!validate_http_binary_interim_response_fields(response, limit)) {
        return false;
    }
    fields.clear();
    const auto headers = response.headers();
    const auto status_token = http_status_code_token(response.status());
    fields.reserve(headers.size() + 1);
    fields.add_owned_value(":status", http_status_code_token_view(status_token));
    for (std::size_t i = 0; i < headers.size(); ++i) {
        append_http_binary_response_field(fields, headers[i].name(), headers[i].value(),
            classify_response_header_name(headers[i].name()));
    }
    return true;
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_response_fields(sink_type& fields,
    const http_response& response, const http_framed_response_head_plan& plan, std::uint64_t limit) {
    // The owning header block already enforces syntax, sender metadata grammar
    // and singleton rules. Only binary framing and section limits remain here.
    const auto known_bits = response_known_header_bits(response);
    const auto status_token = http_status_code_token(plan.body_plan().response_status());
    const auto status_value = http_status_code_token_view(status_token);
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", status_value)) {
        return false;
    }
    // A replaceable chunked hint has no binary-protocol form; Content-Length
    // comes exclusively from the prepared plan used for content accounting.
    constexpr auto writer_framed_bits = response_header_content_length | response_header_transfer_encoding;
    for (const auto& header : response.headers()) {
        if ((http_response_header_access::known_bit(header) & writer_framed_bits) != 0) {
            continue;
        }
        if (!section_size.add(header.name(), header.value())) {
            return false;
        }
    }
    std::string_view generated_date;
    if ((known_bits & response_header_date) == 0) {
        generated_date = cached_date_value();
        if (!generated_date.empty() && !section_size.add("date", generated_date)) {
            return false;
        }
    }
    std::array<char, http_max_unsigned_decimal_size> content_length_bytes;
    std::string_view content_length_value;
    if (const auto content_length = plan.content_length()) {
        content_length_value = http_format_unsigned(content_length_bytes, *content_length);
        if (!section_size.add("content-length", content_length_value)) {
            return false;
        }
    }

    fields.clear();
    fields.reserve(response.headers().size() + 3);
    fields.add_owned_value(":status", status_value);
    for (const auto& header : response.headers()) {
        const auto known_bit = http_response_header_access::known_bit(header);
        if ((known_bit & writer_framed_bits) != 0) {
            continue;
        }
        append_http_binary_response_field(fields, header.name(), header.value(), known_bit);
    }
    if (!generated_date.empty()) {
        fields.add_owned_value("date", generated_date);
    }
    if (!content_length_value.empty()) {
        fields.add_owned_value("content-length", content_length_value);
    }
    return true;
}

[[nodiscard]] inline bool is_valid_http_binary_connect_response_head(const http_response& response) noexcept {
    const auto& body = response_body(response);
    if (!response.status().is_successful() || body.size() != 0 || body.file().has_value()) {
        return false;
    }
    return !response_has_known_header(
        response, response_header_content_length | response_header_transfer_encoding);
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_websocket_response_fields(
    sink_type& fields, const websocket_server_negotiation& negotiation, std::uint64_t limit) {
    // Negotiation fields are already lowercase and validated by their owner.
    const auto response_fields = negotiation.response_headers();
    const bool has_date = std::ranges::any_of(response_fields, [](const http_header& field) {
        return field.name() == "date";
    });
    const auto date = has_date ? std::string_view{} : cached_date_value();
    const auto subprotocol = negotiation.subprotocol();
    const auto extensions = negotiation.extensions();
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", "200") ||
        (!date.empty() && !section_size.add("date", date)) ||
        (!subprotocol.empty() && !section_size.add("sec-websocket-protocol", subprotocol)) ||
        (!extensions.empty() && !section_size.add("sec-websocket-extensions", extensions))) {
        return false;
    }
    for (const auto& field : response_fields) {
        if (!section_size.add(field.name(), field.value())) {
            return false;
        }
    }

    fields.clear();
    fields.reserve(response_fields.size() + 4);
    fields.add(":status", "200");
    if (!date.empty()) {
        fields.add_owned_value("date", date);
    }
    if (!subprotocol.empty()) {
        fields.add("sec-websocket-protocol", subprotocol);
    }
    if (!extensions.empty()) {
        fields.add("sec-websocket-extensions", extensions);
    }
    for (const auto& field : response_fields) {
        fields.add(field.name(), field.value());
    }
    return true;
}

}  // namespace ruvia::detail
