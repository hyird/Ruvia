#pragma once

// Raw HTTP/3 request and response stream bytes for codec tests that must
// reach the receiving codec with field sections its peer API never sends.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/http3_connection.h"

#include "http3_relay.h"

namespace ruvia::test {

// RFC 9204 §4.1.1 prefixed integer.
inline void append_qpack_integer(std::string& bytes, std::size_t value, unsigned prefix_bits, unsigned tag) {
    const auto mask = (1u << prefix_bits) - 1;
    if (value < mask) {
        bytes.push_back(static_cast<char>(tag | static_cast<unsigned>(value)));
        return;
    }
    bytes.push_back(static_cast<char>(tag | mask));
    value -= mask;
    while (value >= 128) {
        bytes.push_back(static_cast<char>((value & 127) | 128));
        value >>= 7;
    }
    bytes.push_back(static_cast<char>(value));
}

// RFC 9000 §16 variable-length integer.
inline void append_quic_varint(std::string& bytes, std::uint64_t value) {
    if (value < 64) {
        bytes.push_back(static_cast<char>(value));
    } else if (value < 16384) {
        bytes.push_back(static_cast<char>(0x40 | (value >> 8)));
        bytes.push_back(static_cast<char>(value & 255));
    } else if (value < (std::uint64_t{1} << 30)) {
        bytes.push_back(static_cast<char>(0x80 | (value >> 24)));
        bytes.push_back(static_cast<char>((value >> 16) & 255));
        bytes.push_back(static_cast<char>((value >> 8) & 255));
        bytes.push_back(static_cast<char>(value & 255));
    } else {
        bytes.push_back(static_cast<char>(0xc0 | (value >> 56)));
        for (int shift = 48; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<char>((value >> shift) & 255));
        }
    }
}

inline void append_http3_wire_frame(std::string& bytes, std::uint64_t type, std::string_view payload) {
    append_quic_varint(bytes, type);
    append_quic_varint(bytes, payload.size());
    bytes.append(payload);
}

// One field line exactly as the wire carries it.
struct http3_wire_field final {
    std::string_view name_;
    std::string_view value_;
};

// RFC 9204 §4.5.6: literal names and values, without dynamic table
// references or Huffman coding, exercise the real decoder with arbitrary
// HTTP semantics.
inline void append_http3_wire_headers(std::string& bytes, std::span<const http3_wire_field> fields) {
    std::string section(2, '\0');  // Required Insert Count = 0; Delta Base = 0.
    for (const auto& field : fields) {
        append_qpack_integer(section, field.name_.size(), 3, 0x20);
        section.append(field.name_);
        append_qpack_integer(section, field.value_.size(), 7, 0);
        section.append(field.value_);
    }
    append_http3_wire_frame(bytes, 1, section);
}

inline void append_http3_wire_headers(std::string& bytes, std::initializer_list<http3_wire_field> fields) {
    append_http3_wire_headers(bytes, std::span(fields.begin(), fields.size()));
}

// Feeds raw stream bytes to `receiver` and logs the events they produced.
[[nodiscard]] inline http3_transport_result receive_http3_wire(http3_connection& receiver,
    std::int64_t stream_id, std::string_view bytes, bool fin, std::uint64_t& timestamp, http3_event_log& log) {
    const auto result = receiver.read_stream(stream_id, http3_bytes(bytes), fin, timestamp++);
    while (auto event = receiver.next_event()) {
        log(*event);
    }
    return result;
}

[[nodiscard]] inline bool has_local_message_error(const http3_event_log& log, std::int64_t stream_id) {
    bool stop_sending = false;
    bool reset_stream = false;
    for (const auto& action : log.transport_actions_) {
        if (action.stream_id() == stream_id && action.error() == http3_error_code::message_error &&
            action.origin() == http3_transport_action_origin::local) {
            stop_sending = stop_sending || action.action() == http3_transport_action::stop_sending;
            reset_stream = reset_stream || action.action() == http3_transport_action::reset_stream;
        }
    }
    return stop_sending && reset_stream;
}

// A malformed message is answered with H3_MESSAGE_ERROR (RFC 9114 §4.1.2).
// nghttp3 read errors are connection-fatal: its documented API forbids
// further calls except destruction. Codec validation resets only the stream,
// with local STOP_SENDING and RESET_STREAM.
[[nodiscard]] inline bool rejected_http3_message(const http3_transport_result& result,
    const http3_connection& connection, const http3_event_log& log, std::int64_t stream_id) {
    return (result.status_ == http3_transport_status::protocol_error &&
               result.error_code_ == http3_error_code::message_error &&
               connection.connection_error() == http3_error_code::message_error) ||
           (result.status_ == http3_transport_status::accepted && has_local_message_error(log, stream_id));
}

}  // namespace ruvia::test
