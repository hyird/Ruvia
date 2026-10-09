#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/http1/http1_server_request_parser.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_response_stream.h"

#include "field/hpack_huffman_tables.h"
#include "http2/http2_connection.h"
#include "http2/http2_frame_codec.h"
#include "http2/http2_header_block.h"
#include "http2/http2_hpack.h"
#include "http2/http2_receive_window_credit.h"
#include "http2/http2_window_update.h"
#include "http2_wire_fixture.h"
#include "test_harness.h"

namespace http2_connection_test {

using ruvia::http2_request_content;
using ruvia::http2_request_head_submit_failure;
using ruvia::http2_request_head_submit_result;
using ruvia::http2_response_head_submit_error;
using ruvia::http2_response_head_submit_failure;
using ruvia::http2_response_head_submit_result;
using ruvia::http2_streaming_response_head_submit_result;
using ruvia::http2_submitted_request_head;
using ruvia::http_response_stream_head_disposition;
using ruvia::http_response_stream_trailer_framing;
using ruvia::http_response_trailer_intent;
using ruvia::detail::hpack_decoder;
using ruvia::detail::hpack_encoder;
using ruvia::detail::http2_connect_form;
using ruvia::detail::http2_connection;
using ruvia::detail::http2_data_submit_status;
using ruvia::detail::http2_end_stream;
using ruvia::detail::http2_error_code;
using ruvia::detail::http2_event;
using ruvia::detail::http2_event_kind;
using ruvia::detail::http2_feed_result;
using ruvia::detail::http2_finish_submit_status;
using ruvia::detail::http2_frame_type;
using ruvia::detail::http2_local_content_known_length;
using ruvia::detail::http2_local_content_state;
using ruvia::detail::http2_local_send_state;
using ruvia::detail::http2_local_settings;
using ruvia::detail::http2_request_head_submit_error;
using ruvia::detail::http2_stream_close_source;
using ruvia::detail::http2_stream_state;
using ruvia::detail::http2_submit_status;
using ruvia::detail::http2_tunnel_state;
using ruvia::detail::http2_websocket_handshake_submit_failure;
using ruvia::detail::http2_websocket_handshake_submit_result;

inline ruvia::detail::http_response_trailer_section validated_trailers(
    std::span<const ruvia::http_header_view> fields_value) {
    const auto result_value = ruvia::detail::check_http_response_trailer_section(fields_value);
    if (result_value.section() == nullptr) {
        throw std::logic_error("expected valid response trailer section");
    }
    return *result_value.section();
}

inline const http2_local_content_known_length& require_local_known_length(const http2_stream_state& stream) {
    if (const auto* known_length = stream.local_content().known_length()) {
        return *known_length;
    }
    throw std::runtime_error("HTTP/2 local content is not known-length");
}

inline std::uint32_t submitted_request_stream_id(const http2_request_head_submit_result& result_value) {
    if (const auto* submitted = result_value.submitted()) {
        return submitted->stream_id();
    }
    throw std::runtime_error("HTTP/2 request head was not submitted");
}

inline http2_request_head_submit_error request_head_submit_error(
    const http2_request_head_submit_result& result_value) {
    if (const auto* failure = result_value.failure()) {
        return failure->error();
    }
    throw std::runtime_error("HTTP/2 request head did not fail");
}

template <typename result_type>
inline bool response_head_submitted(const result_type& result_value) {
    return result_value.submitted() != nullptr;
}

template <typename result_type>
inline std::string_view response_head_submit_failure_message(const result_type& result_value) {
    if (const auto* failure = result_value.failure()) {
        return ruvia::detail::http2_response_head_submit_error_message(failure->error());
    }
    throw std::runtime_error("HTTP/2 response head did not fail");
}

inline http2_response_head_submit_result submit_buffered_response_head(
    http2_connection& connection, std::uint32_t stream_id, const ruvia::http_response& response) {
    const auto* stream = connection.stream(stream_id);
    const auto request_method =
        stream == nullptr ? ruvia::http_known_method::unknown : stream->request_known_method();
    return connection.submit_response_head(
        stream_id, response, ruvia::plan_buffered_http_response_write(request_method, response));
}

template <typename result_type>
inline const auto& submitted_response_plan(const result_type& result_value) {
    if (const auto* submitted = result_value.submitted()) {
        return *submitted;
    }
    throw std::runtime_error("HTTP/2 response head was not submitted");
}

struct request_content_length_observation final {
    std::size_t count_{0};
    std::size_t authority_count_{0};
    std::size_t path_count_{0};
    std::string value_;
    std::string authority_;
    std::string scheme_;
    std::string path_;
};

inline bool observe_request_content_length(
    void* target, std::string_view name, std::string_view value) {
    auto& observation_value = *static_cast<request_content_length_observation*>(target);
    if (name == "content-length") {
        ++observation_value.count_;
        observation_value.value_.assign(value.data(), value.size());
    } else if (name == ":scheme") {
        observation_value.scheme_.assign(value.data(), value.size());
    } else if (name == ":authority") {
        ++observation_value.authority_count_;
        observation_value.authority_.assign(value.data(), value.size());
    } else if (name == ":path") {
        ++observation_value.path_count_;
        observation_value.path_.assign(value.data(), value.size());
    }
    return true;
}

inline std::pmr::string continuation_frame(std::pmr::memory_resource* resource,
    std::uint32_t stream_id, std::uint8_t flags, std::string_view fragment) {
    std::pmr::string frame(resource);
    char header[9];
    ruvia::detail::http2_encode_frame_header(header, static_cast<std::uint32_t>(fragment.size()),
        http2_frame_type::continuation, flags, stream_id);
    frame.append(header, sizeof(header));
    frame.append(fragment.data(), fragment.size());
    return frame;
}

inline std::pmr::string goaway_frame(
    std::pmr::memory_resource* resource, std::uint32_t last_stream_id, http2_error_code error) {
    std::pmr::string bytes(resource);
    char frame[9 + 8];
    ruvia::detail::http2_encode_frame_header(frame, 8, http2_frame_type::goaway, 0, 0);
    ruvia::detail::http2_write_goaway_payload(frame + 9, last_stream_id, error);
    bytes.append(frame, sizeof(frame));
    return bytes;
}

// Test-only HPACK literal with incremental indexing (short, non-Huffman strings).
// The next block on this connection can reference the inserted entry at index 62.
inline void encode_short_dynamic_header(
    std::pmr::string& block, std::string_view name, std::string_view value) {
    block.push_back(static_cast<char>(0x40));
    block.push_back(static_cast<char>(name.size()));
    block.append(name.data(), name.size());
    block.push_back(static_cast<char>(value.size()));
    block.append(value.data(), value.size());
}

inline void append_hpack_integer(
    std::pmr::string& block, std::size_t value, std::uint8_t prefix_bits, std::uint8_t first_bits) {
    const auto prefix_mask = static_cast<std::uint8_t>((1U << prefix_bits) - 1U);
    if (value < prefix_mask) {
        block.push_back(static_cast<char>(first_bits | value));
        return;
    }

    block.push_back(static_cast<char>(first_bits | prefix_mask));
    value -= prefix_mask;
    while (value >= 128) {
        block.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    block.push_back(static_cast<char>(value));
}

inline void encode_repeated_huffman_header(
    std::pmr::string& block, std::string_view name, unsigned char value, std::size_t count) {
    append_hpack_integer(block, 0, 4, ruvia::detail::hpack_literal_without_indexing);
    append_hpack_integer(block, name.size(), 7, 0);
    block.append(name.data(), name.size());

    const auto code = ruvia::detail::hpack_huffman_codes[value];
    const auto bit_length = ruvia::detail::hpack_huffman_lengths[value];
    const auto encoded_bytes = (count * bit_length + 7) / 8;
    append_hpack_integer(block, encoded_bytes, 7, 0x80);

    std::uint64_t pending = 0;
    std::uint8_t pending_bits = 0;
    for (std::size_t i = 0; i < count; ++i) {
        pending = (pending << bit_length) | code;
        pending_bits = static_cast<std::uint8_t>(pending_bits + bit_length);
        while (pending_bits >= 8) {
            pending_bits = static_cast<std::uint8_t>(pending_bits - 8);
            block.push_back(static_cast<char>((pending >> pending_bits) & 0xffU));
        }
        if (pending_bits == 0) {
            pending = 0;
        } else {
            pending &= (std::uint64_t{1} << pending_bits) - 1;
        }
    }
    if (pending_bits != 0) {
        const auto padding_bits = static_cast<std::uint8_t>(8 - pending_bits);
        const auto padding = (std::uint16_t{1} << padding_bits) - 1;
        block.push_back(static_cast<char>((pending << padding_bits) | padding));
    }
}

// Start the role-specific preface and leave the connection ready to receive the
// peer's first frame. Servers must consume the client magic first.
inline void begin_peer_input(http2_connection& conn) {
    conn.begin_connection();
    conn.consume_output(conn.pending_output().size());
    if (conn.role() == ruvia::detail::http2_role::server) {
        const auto result_value = conn.feed(ruvia::detail::http2_client_preface);
        if (result_value != ruvia::detail::http2_feed_result::accepted) {
            throw std::runtime_error("server rejected valid client preface");
        }
    }
}

// Feed the peer's empty non-ACK SETTINGS frame and drain the resulting ACK, leaving
// the connection ready for post-handshake frames.
inline void handshake(http2_connection& conn) {
    begin_peer_input(conn);
    char settings[9];
    ruvia::detail::http2_encode_frame_header(settings, 0, http2_frame_type::settings, 0, 0);
    const auto result_value = conn.feed(std::string_view(settings, sizeof(settings)));
    if (result_value != ruvia::detail::http2_feed_result::accepted || !conn.received_peer_settings()) {
        throw std::runtime_error("connection rejected valid initial SETTINGS");
    }
    conn.consume_output(conn.pending_output().size());
}

inline void begin_client(http2_connection& client) {
    client.begin_connection();
    client.consume_output(client.pending_output().size());
}

inline void apply_peer_max_concurrent_streams(http2_connection& client, std::uint32_t limit) {
    char settings[15];
    auto* out = ruvia::detail::http2_write_frame_header(settings, 6, http2_frame_type::settings, 0, 0);
    out = ruvia::detail::http2_write_settings_entry(
        out, ruvia::detail::http2_setting_id::max_concurrent_streams, limit);
    (void)out;
    (void)client.feed(std::string_view(settings, sizeof(settings)));
    client.consume_output(client.pending_output().size());
}

// Handshake but declare a small peer SETTINGS_INITIAL_WINDOW_SIZE so freshly created
// streams start with a tiny send window (to exercise flow-control backpressure).
inline void handshake_with_window(http2_connection& conn, std::uint32_t window) {
    begin_peer_input(conn);
    char s[9 + 6];
    ruvia::detail::http2_encode_frame_header(s, 6, http2_frame_type::settings, 0, 0);
    s[9] = 0;
    s[10] = 4;  // SETTINGS_INITIAL_WINDOW_SIZE
    s[11] = static_cast<char>((window >> 24) & 0xFF);
    s[12] = static_cast<char>((window >> 16) & 0xFF);
    s[13] = static_cast<char>((window >> 8) & 0xFF);
    s[14] = static_cast<char>(window & 0xFF);
    (void)conn.feed(std::string_view(s, sizeof(s)));
    conn.consume_output(conn.pending_output().size());
}

// Feed a complete GET on stream 1, drain its events and any output, leaving stream 1
// open (half-closed remote) and ready to receive a response.
inline void drive_get_request(http2_connection& conn, std::pmr::memory_resource* res) {
    std::pmr::string block(res);
    encode_get_request(block);
    const auto h = headers_frame(res, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.next_event().has_value()) {
    }
    conn.consume_output(conn.pending_output().size());
}

inline void drive_request(
    http2_connection& conn, std::pmr::memory_resource* res, std::string_view method) {
    std::pmr::string block(res);
    encode_request(block, method);
    const auto h = headers_frame(res, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.next_event().has_value()) {
    }
    conn.consume_output(conn.pending_output().size());
}

// Open stream 1 and let the peer reset it. `pinned` retains the aborted stream
// object to exercise the request-view lifetime branch; the wire state is closed
// in both cases.
inline void open_then_peer_reset(
    http2_connection& conn, std::pmr::memory_resource* resource, bool pinned) {
    std::pmr::string block(resource);
    encode_get_request(block);
    const auto head = headers_frame(resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(head.data(), head.size()));
    while (conn.next_event().has_value()) {
    }
    if (pinned) {
        conn.pin_stream(1);
    }

    char rst[9 + 4];
    ruvia::detail::http2_encode_frame_header(rst, 4, http2_frame_type::rst_stream, 0, 1);
    ruvia::detail::http2_write32(rst + 9, static_cast<std::uint32_t>(http2_error_code::cancel));
    (void)conn.feed(std::string_view(rst, sizeof(rst)));
    while (conn.next_event().has_value()) {
    }
    conn.consume_output(conn.pending_output().size());
}

// Open stream 1 and let this endpoint reset it. DATA that was already in flight
// before the peer observes our RST_STREAM can still arrive and must be minimally
// processed without sending another stream frame.
inline void open_then_local_reset(
    http2_connection& conn, std::pmr::memory_resource* resource, bool pinned = false) {
    std::pmr::string block(resource);
    encode_get_request(block);
    const auto head = headers_frame(resource, 1,
        ruvia::detail::http2_flag_end_headers | ruvia::detail::http2_flag_end_stream,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(head.data(), head.size()));
    while (conn.next_event().has_value()) {
    }
    if (pinned) {
        conn.pin_stream(1);
    }
    (void)conn.submit_reset(1, http2_error_code::cancel);
    conn.consume_output(conn.pending_output().size());  // flush the one legal reset
}

using ruvia::detail::http2_role;

// Byte shuttle between two cores (no sockets): move pending output of `from` into
// `to`, draining `to`'s events into the collectors first would lose them -- so the
// caller passes a per-hop event sink invoked after every feed.
template <typename on_event_type>
inline void shuttle_once(http2_connection& from, http2_connection& to, on_event_type&& on_event) {
    while (from.wants_write()) {
        const auto out = from.pending_output();
        std::pmr::string copy(out.data(), out.size(), std::pmr::get_default_resource());
        from.consume_output(out.size());
        (void)to.feed(std::string_view(copy.data(), copy.size()));
        while (const auto event = to.next_event()) {
            on_event(*event);
        }
    }
}

// Walk the outbound buffer frame-by-frame and return the error code of the first GOAWAY,
// or 0xffffffff if none is present.
inline std::uint32_t first_goaway_error(std::string_view out) {
    std::size_t pos = 0;
    while (pos + 9 <= out.size()) {
        const auto h = ruvia::detail::http2_parse_frame_header(out.substr(pos, 9));
        if (h.type_ == static_cast<std::uint8_t>(http2_frame_type::goaway) && h.length_ >= 8) {
            const auto* p = reinterpret_cast<const unsigned char*>(out.data() + pos + 9);
            return (static_cast<std::uint32_t>(p[4]) << 24) |
                   (static_cast<std::uint32_t>(p[5]) << 16) |
                   (static_cast<std::uint32_t>(p[6]) << 8) | static_cast<std::uint32_t>(p[7]);
        }
        pos += 9 + h.length_;
    }
    return 0xffffffffU;
}
constexpr std::uint32_t enhance_your_calm =
    static_cast<std::uint32_t>(ruvia::detail::http2_error_code::enhance_your_calm);

// Build a POST request head (no END_STREAM) with optional content-length; body follows.
inline std::pmr::string post_head_frame(
    std::pmr::memory_resource* resource, std::string_view content_length) {
    std::pmr::string block(resource);
    hpack_encoder::encode_header(block, ":method", "POST");
    hpack_encoder::encode_header(block, ":scheme", "https");
    hpack_encoder::encode_header(block, ":path", "/");
    hpack_encoder::encode_header(block, ":authority", "example.com");
    if (!content_length.empty()) {
        hpack_encoder::encode_header(block, "content-length", content_length);
    }
    return headers_frame(resource, 1, ruvia::detail::http2_flag_end_headers,
        std::string_view(block.data(), block.size()));
}

// Frame a DATA payload on `stream_id` with the given flags.

}  // namespace http2_connection_test

using namespace http2_connection_test;  // NOLINT(google-build-using-namespace)
