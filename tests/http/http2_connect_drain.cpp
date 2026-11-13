// Server-side CONNECT data before the accept/reject decision, graceful drain
// round trip, SETTINGS_ENABLE_CONNECT_PROTOCOL from a client, and extension frames
// on an established tunnel (RFC 9113 sections 5.5, 6.8, 8.5; RFC 8441 section 3;
// RFC 9298 section 5 as updated by RFC 9931).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/websocket_handshake.h"

#include "content_decoding_fixture.h"
#include "test_exceptions.h"

namespace {

constexpr std::uint8_t end_stream_flag = 0x1;
constexpr std::uint8_t end_headers_flag = 0x4;

void append_frame(std::string& wire, std::uint8_t type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()),
            static_cast<ruvia::http2_frame_type>(type), flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload = {}) {
    append_frame(wire, static_cast<std::uint8_t>(type), flags, stream_id, payload);
}

std::uint32_t read_u32(std::string_view bytes) {
    std::uint32_t value = 0;
    for (const auto byte : bytes.substr(0, 4)) {
        value = (value << 8) | static_cast<unsigned char>(byte);
    }
    return value;
}

struct wire_frame final {
    ruvia::http2_frame_header header_;
    std::string payload_;

    [[nodiscard]] bool is(ruvia::http2_frame_type type) const noexcept {
        return header_.type_ == static_cast<std::uint8_t>(type);
    }
};

std::vector<wire_frame> parse_frames(std::string_view output) {
    std::vector<wire_frame> frames;
    while (!output.empty()) {
        const auto header = ruvia::parse_http2_frame_header(std::span(output.data(), output.size()));
        if (!header || output.size() < ruvia::http2_frame_header_bytes + header->length_) {
            throw std::logic_error("invalid HTTP/2 test output");
        }
        frames.push_back({*header, std::string(output.substr(ruvia::http2_frame_header_bytes, header->length_))});
        output.remove_prefix(ruvia::http2_frame_header_bytes + header->length_);
    }
    return frames;
}

std::vector<wire_frame> take_frames(ruvia::http2_connection& connection) {
    return parse_frames(ruvia::test::drain_output(connection));
}

bool has_frame(const std::vector<wire_frame>& frames, ruvia::http2_frame_type type, std::uint32_t stream_id) {
    for (const auto& frame : frames) {
        if (frame.is(type) && frame.header_.stream_id_ == stream_id) {
            return true;
        }
    }
    return false;
}

// Last stream ids of the NO_ERROR GOAWAY frames; any other error code is
// reported as 0xffffffff so a comparison with expected ids fails.
std::vector<std::uint32_t> goaway_last_stream_ids(const std::vector<wire_frame>& frames) {
    std::vector<std::uint32_t> ids;
    for (const auto& frame : frames) {
        if (frame.is(ruvia::http2_frame_type::goaway)) {
            const bool no_error = read_u32(std::string_view(frame.payload_).substr(4)) == 0;
            ids.push_back(no_error ? read_u32(frame.payload_) & 0x7fffffffU : 0xffffffffU);
        }
    }
    return ids;
}

std::string connect_block(std::string_view protocol) {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
    if (protocol.empty()) {
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test:443");
        return block;
    }
    ruvia::hpack_encoder::encode_header(block, ":protocol", protocol);
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "proxy.test");
    if (protocol == "websocket") {
        ruvia::hpack_encoder::encode_header(block, ":path", "/chat");
        ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
    } else {
        ruvia::hpack_encoder::encode_header(block, ":path", "/.well-known/masque/udp/192.0.2.6/443/");
        ruvia::hpack_encoder::encode_header(block, "capsule-protocol", "?1");
    }
    return block;
}

std::string get_block() {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");
    return block;
}

// The peer's preface and SETTINGS, plus its ACK of ours so that the advertised
// 1 MiB stream window is in force (RFC 9113 §6.5.3).
std::string opening_wire() {
    std::string wire(ruvia::http2_client_preface);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    append_frame(wire, ruvia::http2_frame_type::settings, 0x1, 0, {});
    return wire;
}

// Drains every event of one fed request and keeps its head (the stream lease).
std::optional<ruvia::http2_request_head_event> take_request_head(ruvia::http2_connection& server) {
    std::optional<ruvia::http2_request_head_event> head;
    while (auto event = server.next_event()) {
        if (auto* request = event->request_head()) {
            head.emplace(std::move(*request));
        }
    }
    return head;
}

ruvia::http2_connection started_server() {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    return server;
}

}  // namespace

// RFC 9113 section 8.5 makes every DATA after a CONNECT request's HEADERS tunnel
// data, and RFC 9298 section 5 lets an HTTP/2 client send it before the 2xx.
TEST(Ruvia, http2_connect_data_before_accept_is_held_and_delivered_as_tunnel_data) {
    for (const std::string_view protocol : {std::string_view{}, std::string_view{"connect-udp"}}) {
        auto server = started_server();
        auto wire = opening_wire();
        append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block(protocol));
        const std::string_view capsule("\x00\x05\x00ping", 7);
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1, capsule);
        // Enough held bytes to cross the half-window WINDOW_UPDATE threshold.
        const std::string bulk(16384, 'e');
        std::string expected(capsule);
        for (int frame = 0; frame != 33; ++frame) {
            append_frame(wire, ruvia::http2_frame_type::data, 0, 1, bulk);
            expected.append(bulk);
        }
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

        std::optional<ruvia::http2_request_head_event> head;
        std::optional<ruvia::http2_received_data_credit> credit;
        std::string received;
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                EXPECT_TRUE(request->snapshot().connect_pending_);
                EXPECT_FALSE(request->snapshot().initial_end_stream_);
                head.emplace(std::move(*request));
            } else if (auto* data = event->tunnel_data()) {
                EXPECT_EQ(data->stream_id(), 1U);
                received.append(data->bytes());
                if (credit) {
                    credit->merge(data->take_credit());
                } else {
                    credit.emplace(data->take_credit());
                }
            } else {
                EXPECT_TRUE(event->stream_closed() == nullptr);
            }
        }
        EXPECT_TRUE(head.has_value());
        EXPECT_TRUE(received == expected);
        EXPECT_TRUE(credit.has_value() && credit->valid());
        if (!head || !credit) {
            return;
        }
        EXPECT_TRUE(!server.stream_aborted(1));
        EXPECT_TRUE(!server.connection_error().has_value());
        const auto before_accept = take_frames(server);
        EXPECT_TRUE(!has_frame(before_accept, ruvia::http2_frame_type::rst_stream, 1));
        // The held bytes keep their receive-window credit until the owner consumes them.
        EXPECT_TRUE(!has_frame(before_accept, ruvia::http2_frame_type::window_update, 1));
        EXPECT_TRUE(!has_frame(before_accept, ruvia::http2_frame_type::window_update, 0));

        ruvia::http_response accepted;
        accepted.status(ruvia::http_status::ok);
        EXPECT_TRUE(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
        credit.reset();
        const auto after_accept = take_frames(server);
        EXPECT_TRUE(has_frame(after_accept, ruvia::http2_frame_type::headers, 1));
        EXPECT_TRUE(has_frame(after_accept, ruvia::http2_frame_type::window_update, 1));
        EXPECT_TRUE(has_frame(after_accept, ruvia::http2_frame_type::window_update, 0));
        EXPECT_TRUE(!has_frame(after_accept, ruvia::http2_frame_type::rst_stream, 1));

        std::string more;
        append_frame(more, ruvia::http2_frame_type::data, end_stream_flag, 1, "tail");
        EXPECT_TRUE(server.feed(more) == ruvia::http2_feed_result::accepted);
        auto tail = server.next_event();
        EXPECT_TRUE(tail && tail->tunnel_data() != nullptr && tail->tunnel_data()->bytes() == "tail");
        const auto end = server.next_event();
        EXPECT_TRUE(end && end->tunnel_end() != nullptr);
        EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    }
}

// Early bytes that ended the request half are followed by tunnel_end on accept.
TEST(Ruvia, http2_connect_data_with_end_stream_before_accept_ends_tunnel_after_accept) {
    auto server = started_server();
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block({}));
    append_frame(wire, ruvia::http2_frame_type::data, end_stream_flag, 1, "hello");
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    std::optional<ruvia::http2_request_head_event> head;
    std::string received;
    while (auto event = server.next_event()) {
        if (auto* request = event->request_head()) {
            head.emplace(std::move(*request));
        } else if (auto* data = event->tunnel_data()) {
            received.append(data->bytes());
        } else {
            EXPECT_TRUE(event->tunnel_end() == nullptr && event->stream_closed() == nullptr);
        }
    }
    EXPECT_TRUE(received == "hello");
    EXPECT_TRUE(head.has_value());
    ruvia::http_response accepted;
    accepted.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
    const auto end = server.next_event();
    EXPECT_TRUE(end && end->tunnel_end() != nullptr);
    EXPECT_TRUE(server.stream_receive_status(1) == ruvia::http2_stream_receive_status::ended);
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
}

// After a non-2xx CONNECT response the optimistic bytes are discarded with their
// flow-control credit instead of resetting the stream.
TEST(Ruvia, http2_connect_data_after_rejection_is_discarded_with_credit) {
    auto server = started_server();
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block({}));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head_event = server.next_event();
    EXPECT_TRUE(head_event && head_event->request_head() != nullptr);
    if (!head_event || head_event->request_head() == nullptr) {
        return;
    }
    auto head = std::move(*head_event->request_head());
    ruvia::http_response rejected;
    rejected.status(ruvia::http_status::forbidden);
    const auto submitted = server.submit_response_head(
        1, rejected, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::connect, rejected));
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
    (void)take_frames(server);

    std::string late;
    const std::string bulk(16384, 'l');
    for (int frame = 0; frame != 33; ++frame) {
        append_frame(late, ruvia::http2_frame_type::data, 0, 1, bulk);
    }
    EXPECT_TRUE(server.feed(late) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.next_event().has_value());
    const auto frames = take_frames(server);
    EXPECT_TRUE(!has_frame(frames, ruvia::http2_frame_type::rst_stream, 1));
    EXPECT_TRUE(has_frame(frames, ruvia::http2_frame_type::window_update, 0));
    EXPECT_TRUE(has_frame(frames, ruvia::http2_frame_type::window_update, 1));
    EXPECT_TRUE(!server.connection_error().has_value());

    std::string finish;
    append_frame(finish, ruvia::http2_frame_type::data, end_stream_flag, 1, {});
    EXPECT_TRUE(server.feed(finish) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    EXPECT_TRUE(!server.connection_error().has_value());
    (void)server.release(std::move(head));
}

// RFC 6455 section 4.1 binds the WebSocket client to wait for the 2xx; it gives the
// recipient no reason to reset, so RFC 8441 DATA before the response is held under
// flow control as tunnel data like any other CONNECT.
TEST(Ruvia, http2_websocket_connect_data_before_response_is_held_as_tunnel_data) {
    auto server = started_server();
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block("websocket"));
    append_frame(wire, ruvia::http2_frame_type::data, 0, 1, "frame");
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    bool early_data = false;
    std::optional<ruvia::http2_request_head_event> head;
    while (auto event = server.next_event()) {
        EXPECT_TRUE(event->stream_closed() == nullptr);
        if (auto* request = event->request_head()) {
            head.emplace(std::move(*request));
        } else if (auto* data = event->tunnel_data()) {
            early_data = data->stream_id() == 1 && data->bytes() == "frame";
        }
    }
    EXPECT_TRUE(early_data);
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    EXPECT_TRUE(!server.connection_error().has_value());
    if (head) {
        (void)server.release(std::move(*head));
    }
}

// RFC 7838 section 4 and RFC 8336 section 2.1: a server MUST ignore ALTSVC, and
// ORIGIN on a nonzero stream; an established tunnel keeps them out of its bytes.
TEST(Ruvia, http2_connect_tunnel_ignores_alternative_service_and_origin_frames) {
    auto server = started_server();
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block({}));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    EXPECT_TRUE(head.has_value());
    if (!head) {
        return;
    }
    ruvia::http_response accepted;
    accepted.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
    (void)take_frames(server);
    while (server.next_event()) {
    }

    std::string extension;
    append_frame(extension, ruvia::http2_frame_type::alternative_service, 0, 1, std::string_view("\0\0h3", 4));
    append_frame(extension, ruvia::http2_frame_type::origin, 0, 1, {});
    append_frame(extension, ruvia::http2_frame_type::data, 0, 1, "after");
    EXPECT_TRUE(server.feed(extension) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error().has_value());
    EXPECT_TRUE(!server.stream_aborted(1));
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    std::string received;
    while (auto event = server.next_event()) {
        EXPECT_TRUE(event->stream_closed() == nullptr);
        if (auto* data = event->tunnel_data()) {
            received.append(data->bytes());
        }
    }
    EXPECT_EQ(received, std::string("after"));
    (void)server.release(std::move(*head));
}

// RFC 9113 section 6.8: the first GOAWAY advertises 2^31-1 so streams the client
// already sent are still served; the final boundary follows one PING round trip.
TEST(Ruvia, http2_server_graceful_drain_admits_in_flight_streams_until_ping_ack) {
    auto server = started_server();
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block());
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto first = take_request_head(server);
    EXPECT_TRUE(first.has_value());
    (void)take_frames(server);

    server.begin_drain();
    EXPECT_TRUE(server.draining());
    EXPECT_TRUE(!server.drain_final());
    const auto initial = take_frames(server);
    EXPECT_TRUE(goaway_last_stream_ids(initial) == std::vector<std::uint32_t>{0x7fffffffU});
    // The PING's ACK proves the client read the GOAWAY only if it follows it.
    std::optional<std::string> ping_payload;
    bool goaway_seen = false;
    for (const auto& frame : initial) {
        goaway_seen = goaway_seen || frame.is(ruvia::http2_frame_type::goaway);
        if (frame.is(ruvia::http2_frame_type::ping)) {
            EXPECT_TRUE(goaway_seen);
            EXPECT_EQ(frame.header_.flags_ & 0x1, 0);
            ping_payload = frame.payload_;
        }
    }
    EXPECT_TRUE(ping_payload.has_value());
    if (!ping_payload) {
        return;
    }

    std::string in_flight;
    append_frame(in_flight, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 3, get_block());
    EXPECT_TRUE(server.feed(in_flight) == ruvia::http2_feed_result::accepted);
    auto second = take_request_head(server);
    EXPECT_TRUE(second && second->stream_id() == 3);
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 3));

    std::string ack;
    append_frame(ack, ruvia::http2_frame_type::ping, 0x1, 0, *ping_payload);
    EXPECT_TRUE(server.feed(ack) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(server.drain_final());
    EXPECT_TRUE(goaway_last_stream_ids(take_frames(server)) == std::vector<std::uint32_t>{3});

    // RFC 9113 §6.8: a stream above the final boundary is discarded unanswered;
    // the GOAWAY already tells the client it was not processed.
    std::string discarded;
    append_frame(discarded, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 5, get_block());
    EXPECT_TRUE(server.feed(discarded) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.next_event().has_value());
    EXPECT_TRUE(take_frames(server).empty());
    EXPECT_TRUE(!server.connection_error().has_value());
}

// A client that withholds the PING ACK cannot keep the drain open indefinitely.
TEST(Ruvia, http2_server_graceful_drain_bounds_streams_opened_before_ping_ack) {
    auto server = started_server();
    const auto opening = opening_wire();
    EXPECT_TRUE(server.feed(opening) == ruvia::http2_feed_result::accepted);
    (void)take_frames(server);
    server.begin_drain();
    (void)take_frames(server);
    const auto block = get_block();
    std::uint32_t stream_id = 1;
    // The drain started at stream 0: 128 further peer streams (1..255) are admitted.
    for (; stream_id <= 255; stream_id += 2) {
        std::string request;
        append_frame(request, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, stream_id, block);
        EXPECT_TRUE(server.feed(request) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        EXPECT_TRUE(head.has_value());
        if (!head) {
            return;
        }
        ruvia::http_response response;
        const auto submitted = server.submit_response_head(stream_id, response,
            ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
        EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
        (void)server.release(std::move(*head));
        const auto frames = take_frames(server);
        EXPECT_TRUE(goaway_last_stream_ids(frames).empty());
        EXPECT_TRUE(!has_frame(frames, ruvia::http2_frame_type::rst_stream, stream_id));
    }
    EXPECT_TRUE(!server.drain_final());
    std::string beyond;
    append_frame(beyond, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, stream_id, block);
    EXPECT_TRUE(server.feed(beyond) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.next_event().has_value());
    EXPECT_TRUE(server.drain_final());
    const auto frames = take_frames(server);
    EXPECT_TRUE(goaway_last_stream_ids(frames) == std::vector<std::uint32_t>{255});
    EXPECT_TRUE(has_frame(frames, ruvia::http2_frame_type::rst_stream, stream_id));
}

// A client drains in one step: its boundary covers server-initiated streams only.
TEST(Ruvia, http2_client_graceful_drain_sends_final_goaway_at_once) {
    auto client = ruvia::http2_connection::client({});
    (void)ruvia::test::drain_output(client);
    client.begin_drain();
    EXPECT_TRUE(client.drain_final());
    const auto frames = take_frames(client);
    EXPECT_TRUE(goaway_last_stream_ids(frames) == std::vector<std::uint32_t>{0});
    for (const auto& frame : frames) {
        EXPECT_TRUE(!frame.is(ruvia::http2_frame_type::ping));
    }
}

// RFC 9113 section 6.8 asks for "at least one round-trip time", not for the PING
// ACK: the owner's time bound makes the boundary final through finish_drain().
TEST(Ruvia, http2_server_finish_drain_ends_round_trip_without_ping_ack) {
    auto server = started_server();
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block());
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto first = take_request_head(server);
    EXPECT_TRUE(first.has_value());
    (void)take_frames(server);
    server.begin_drain();
    EXPECT_TRUE(goaway_last_stream_ids(take_frames(server)) == std::vector<std::uint32_t>{0x7fffffffU});
    std::string in_flight;
    append_frame(in_flight, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 3, get_block());
    EXPECT_TRUE(server.feed(in_flight) == ruvia::http2_feed_result::accepted);
    auto second = take_request_head(server);
    EXPECT_TRUE(second && second->stream_id() == 3);
    EXPECT_TRUE(!server.drain_final());

    server.finish_drain();
    EXPECT_TRUE(server.drain_final());
    EXPECT_TRUE(goaway_last_stream_ids(take_frames(server)) == std::vector<std::uint32_t>{3});
    server.finish_drain();
    EXPECT_TRUE(take_frames(server).empty());

    std::string discarded;
    append_frame(discarded, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 5, get_block());
    EXPECT_TRUE(server.feed(discarded) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.next_event().has_value());
    EXPECT_TRUE(take_frames(server).empty());
    EXPECT_TRUE(!server.connection_error().has_value());
}

// A peer whose input ended can open no stream: finish_drain() from the open state
// sends the final GOAWAY at once, with no PING round trip.
TEST(Ruvia, http2_server_finish_drain_without_round_trip_sends_final_goaway) {
    auto server = started_server();
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block());
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    EXPECT_TRUE(head.has_value());
    (void)take_frames(server);
    server.finish_drain();
    EXPECT_TRUE(server.draining());
    EXPECT_TRUE(server.drain_final());
    const auto frames = take_frames(server);
    EXPECT_TRUE(goaway_last_stream_ids(frames) == std::vector<std::uint32_t>{1});
    for (const auto& frame : frames) {
        EXPECT_TRUE(!frame.is(ruvia::http2_frame_type::ping));
    }
}

// RFC 9113 section 6.8: "Receivers of a GOAWAY frame MUST NOT open additional
// streams on the connection", so a server answering a peer GOAWAY needs no round
// trip -- neither when the peer goes first nor during a round trip in progress.
TEST(Ruvia, http2_server_drain_after_peer_goaway_is_final_at_once) {
    for (const bool round_trip_started : {false, true}) {
        auto server = started_server();
        auto wire = opening_wire();
        append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block());
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        EXPECT_TRUE(head.has_value());
        (void)take_frames(server);
        if (round_trip_started) {
            server.begin_drain();
            (void)take_frames(server);
        }
        std::string goaway;
        append_frame(goaway, ruvia::http2_frame_type::goaway, 0, 0, std::string_view("\0\0\0\0\0\0\0\0", 8));
        EXPECT_TRUE(server.feed(goaway) == ruvia::http2_feed_result::accepted);
        while (server.next_event()) {
        }
        EXPECT_TRUE(server.drain_final());
        const auto frames = take_frames(server);
        EXPECT_TRUE(goaway_last_stream_ids(frames) == std::vector<std::uint32_t>{1});
        for (const auto& frame : frames) {
            EXPECT_TRUE(!frame.is(ruvia::http2_frame_type::ping));
        }
        EXPECT_TRUE(!server.connection_error().has_value());
    }
}

// RFC 8441 section 3: "Receipt of this parameter by a server does not have any
// impact", so a client may withdraw it. A value outside 0..1 is a connection
// error for either role, and a server may not withdraw it.
TEST(Ruvia, http2_enable_connect_protocol_values_are_validated_for_both_roles) {
    auto server = started_server();
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x01", 6));
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x00", 6));
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block());
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error().has_value());
    auto head = server.next_event();
    EXPECT_TRUE(head && head->request_head() != nullptr);
    EXPECT_TRUE(goaway_last_stream_ids(take_frames(server)).empty());
    while (server.next_event()) {
    }
    std::string invalid;
    append_frame(invalid, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x02", 6));
    EXPECT_TRUE(server.feed(invalid) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);

    for (const std::string_view value : {std::string_view("\x00\x00\x00\x02", 4), std::string_view("\x00\x00\x00\x00", 4)}) {
        auto client = ruvia::http2_connection::client({});
        (void)ruvia::test::drain_output(client);
        std::string from_server;
        append_frame(from_server, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x01", 6));
        std::string entry("\x00\x08", 2);
        entry.append(value);
        append_frame(from_server, ruvia::http2_frame_type::settings, 0, 0, entry);
        EXPECT_TRUE(client.feed(from_server) == ruvia::http2_feed_result::protocol_failure);
        EXPECT_TRUE(client.connection_error() == ruvia::http2_error_code::protocol_error);
    }
}

// RFC 9113 section 5.5: unknown frame types are discarded, including on a
// connected CONNECT stream; known non-stream-management frames still reset it.
TEST(Ruvia, http2_connected_tunnel_discards_unknown_extension_frames) {
    auto server = started_server();
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block({}));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head_event = server.next_event();
    EXPECT_TRUE(head_event && head_event->request_head() != nullptr);
    if (!head_event || head_event->request_head() == nullptr) {
        return;
    }
    auto head = std::move(*head_event->request_head());
    ruvia::http_response accepted;
    accepted.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
    (void)take_frames(server);

    std::string extension;
    append_frame(extension, std::uint8_t{0x21}, 0, 1, "opaque");
    append_frame(extension, ruvia::http2_frame_type::data, 0, 1, "after");
    EXPECT_TRUE(server.feed(extension) == ruvia::http2_feed_result::accepted);
    auto data = server.next_event();
    EXPECT_TRUE(data && data->tunnel_data() != nullptr && data->tunnel_data()->bytes() == "after");
    EXPECT_TRUE(!server.next_event().has_value());
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    EXPECT_TRUE(!server.stream_aborted(1));

    std::string headers;
    append_frame(headers, ruvia::http2_frame_type::headers, end_headers_flag, 1, get_block());
    EXPECT_TRUE(server.feed(headers) == ruvia::http2_feed_result::accepted);
    while (server.next_event()) {
    }
    EXPECT_TRUE(has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    (void)server.release(std::move(head));
}

// RFC 9110 section 8.6 makes Content-Length on CONNECT only a sender SHOULD NOT
// and RFC 9113 section 8.5 does not list it as malformed: a declared zero is
// accepted (as in HTTP/1), while a non-zero length claims unframed content.
TEST(Ruvia, http2_connect_accepts_zero_content_length_and_resets_nonzero) {
    for (const std::string_view protocol : {std::string_view{}, std::string_view{"connect-udp"}}) {
        for (const std::string_view length : {std::string_view{"0"}, std::string_view{"3"}}) {
            auto server = started_server();
            auto wire = opening_wire();
            auto block = connect_block(protocol);
            ruvia::hpack_encoder::encode_header(block, "content-length", length);
            append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, block);
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            const auto head = take_request_head(server);
            const auto frames = take_frames(server);
            const bool accepted = length == "0";
            EXPECT_EQ(head.has_value(), accepted);
            EXPECT_EQ(has_frame(frames, ruvia::http2_frame_type::rst_stream, 1), !accepted);
            EXPECT_TRUE(!server.connection_error().has_value());
        }
    }
}

// RFC 8441 extended CONNECT accepts Content-Length: 0 and early DATA(END_STREAM)
// carrying a WebSocket frame before the 200 handshake response is committed.
TEST(Ruvia, http2_websocket_connect_accepts_zero_content_length_and_early_data_with_end_stream) {
    {
        auto server = started_server();
        auto wire = opening_wire();
        auto block = connect_block("websocket");
        ruvia::hpack_encoder::encode_header(block, "content-length", "0");
        append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, block);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        EXPECT_TRUE(head.has_value());
        if (head) {
            const auto req = head->build_request();
            const auto val = ruvia::validate_http2_websocket_handshake(server, 1, req);
            EXPECT_TRUE(val.accepted() != nullptr);
            const auto sub = server.submit_websocket_handshake(1, req, val);
            EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(sub));
            (void)server.release(std::move(*head));
        }
    }
    {
        auto server = started_server();
        auto wire = opening_wire();
        auto block = connect_block("websocket");
        append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, block);
        append_frame(wire, ruvia::http2_frame_type::data, end_stream_flag, 1,
            std::string_view("\x88\x82\x00\x00\x00\x00\x03\xe8", 8));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::optional<ruvia::http2_request_head_event> head;
        bool saw_data = false;
        while (auto ev = server.next_event()) {
            if (auto* rh = ev->request_head()) {
                head.emplace(std::move(*rh));
            } else if (ev->tunnel_data()) {
                saw_data = true;
            }
        }
        EXPECT_TRUE(head.has_value());
        EXPECT_TRUE(saw_data);
        if (head) {
            const auto req = head->build_request();
            const auto val = ruvia::validate_http2_websocket_handshake(server, 1, req);
            EXPECT_TRUE(val.accepted() != nullptr);
            const auto sub = server.submit_websocket_handshake(1, req, val);
            EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(sub));
            auto end_ev = server.next_event();
            EXPECT_TRUE(end_ev && end_ev->tunnel_end() != nullptr);
            (void)server.release(std::move(*head));
        }
    }
    {
        // The same early data split across a data frame and an empty
        // DATA(END_STREAM) must reach the same handshake verdict.
        auto server = started_server();
        auto wire = opening_wire();
        auto block = connect_block("websocket");
        append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, block);
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1,
            std::string_view("\x88\x82\x00\x00\x00\x00\x03\xe8", 8));
        append_frame(wire, ruvia::http2_frame_type::data, end_stream_flag, 1, std::string_view{});
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::optional<ruvia::http2_request_head_event> head;
        bool saw_data = false;
        while (auto ev = server.next_event()) {
            if (auto* rh = ev->request_head()) {
                head.emplace(std::move(*rh));
            } else if (ev->tunnel_data()) {
                saw_data = true;
            }
        }
        EXPECT_TRUE(head.has_value());
        EXPECT_TRUE(saw_data);
        if (head) {
            const auto req = head->build_request();
            const auto val = ruvia::validate_http2_websocket_handshake(server, 1, req);
            EXPECT_TRUE(val.accepted() != nullptr);
            const auto sub = server.submit_websocket_handshake(1, req, val);
            EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(sub));
            auto end_ev = server.next_event();
            EXPECT_TRUE(end_ev && end_ev->tunnel_end() != nullptr);
            (void)server.release(std::move(*head));
        }
    }
}

namespace {

// RFC 7541 section 5.1 prefix integer, used to craft wire input.
void append_prefixed_integer(std::string& output, std::uint8_t flags, unsigned prefix_bits, std::uint32_t value) {
    const std::uint32_t prefix_limit = (1U << prefix_bits) - 1U;
    if (value < prefix_limit) {
        output.push_back(static_cast<char>(flags | value));
        return;
    }
    output.push_back(static_cast<char>(flags | prefix_limit));
    value -= prefix_limit;
    while (value >= 128) {
        output.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    output.push_back(static_cast<char>(value));
}

[[nodiscard]] std::string table_size_update(std::uint32_t bytes) {
    std::string output;
    append_prefixed_integer(output, 0x20, 5, bytes);
    return output;
}

[[nodiscard]] std::string indexed_field(std::uint32_t index) {
    std::string output;
    append_prefixed_integer(output, 0x80, 7, index);
    return output;
}

// Literal with incremental indexing and a new name (RFC 7541 section 6.2.1),
// without Huffman coding.
[[nodiscard]] std::string indexed_literal(std::string_view name, std::string_view value) {
    std::string output(1, static_cast<char>(0x40));
    append_prefixed_integer(output, 0x00, 7, static_cast<std::uint32_t>(name.size()));
    output.append(name);
    append_prefixed_integer(output, 0x00, 7, static_cast<std::uint32_t>(value.size()));
    output.append(value);
    return output;
}

void prepare_alias_output(std::string& output, bool grow) {
    output = "content-type|authorization|x-alias-name|abcdefghijklmnopqrstuvwxyz";
    output.append(192, 'v');
    if (grow) {
        output.resize(output.capacity(), '#');
    } else {
        output.reserve(output.size() * 4 + 128);
    }
}

void check_encoded_field(std::string_view output, std::string_view prefix,
    std::string_view expected_name, std::string_view expected_value) {
    EXPECT_EQ(output.substr(0, prefix.size()), prefix);
    ruvia::hpack_decoder decoder;
    std::size_t fields = 0;
    const auto result = decoder.decode(output.substr(prefix.size()),
        [&](std::string_view name, std::string_view value) {
            ++fields;
            EXPECT_EQ(name, expected_name);
            EXPECT_EQ(value, expected_value);
            return true;
        });
    EXPECT_TRUE(!result.has_value());
    EXPECT_EQ(fields, std::size_t{1});
}

void check_newest(ruvia::hpack_decoder& decoder,
    std::uint32_t index, std::string_view expected_name, std::string_view expected_value) {
    std::size_t fields = 0;
    const auto result = decoder.decode(indexed_field(index), [&](std::string_view name, std::string_view value) {
        EXPECT_EQ(name, expected_name);
        EXPECT_EQ(value, expected_value);
        ++fields;
        return true;
    });
    EXPECT_TRUE(!result.has_value());
    EXPECT_EQ(fields, std::size_t{1});
}

[[nodiscard]] bool decoder_is_poisoned(ruvia::hpack_decoder& decoder) {
    std::size_t fields = 0;
    const auto result = decoder.decode(std::string_view("\x82", 1), [&](auto, auto) {
        ++fields;
        return true;
    });
    return result == ruvia::hpack_decode_error::compression_error && fields == 0;
}

}  // namespace

TEST(Ruvia, hpack_public_encoder_accepts_fields_borrowed_from_output) {
    enum class alias_kind { name,
        value,
        disjoint,
        overlapping,
        identical,
        static_name,
        sensitive_name };
    for (const bool grow : {false, true}) {
        for (const auto kind : {alias_kind::name, alias_kind::value, alias_kind::disjoint,
                 alias_kind::overlapping, alias_kind::identical, alias_kind::static_name,
                 alias_kind::sensitive_name}) {
            {
                std::string output;
                prepare_alias_output(output, grow);
                const std::string prefix(output);
                const auto contents = std::string_view(output);
                const auto payload = contents.find("abcdefghijklmnopqrstuvwxyz");
                std::string_view name = "x-outside";
                std::string_view value = "outside-value";
                switch (kind) {
                    case alias_kind::name:
                        name = contents.substr(contents.find("x-alias-name"), 12);
                        break;
                    case alias_kind::value:
                        value = contents.substr(payload, 100);
                        break;
                    case alias_kind::disjoint:
                        name = contents.substr(contents.find("x-alias-name"), 12);
                        value = contents.substr(payload, 100);
                        break;
                    case alias_kind::overlapping:
                        name = contents.substr(payload, 40);
                        value = contents.substr(payload + 11, 80);
                        break;
                    case alias_kind::identical:
                        name = contents.substr(payload, 60);
                        value = name;
                        break;
                    case alias_kind::static_name:
                        name = contents.substr(0, 12);
                        value = contents.substr(payload, 100);
                        break;
                    case alias_kind::sensitive_name:
                        name = contents.substr(contents.find("authorization"), 13);
                        value = contents.substr(payload, 100);
                        break;
                }
                const std::string expected_name(name);
                const std::string expected_value(value);
                ruvia::hpack_encoder::encode_header(output, name, value);
                check_encoded_field(output, prefix, expected_name, expected_value);
            }
        }
    }
}

TEST(Ruvia, hpack_public_encoder_never_indexes_sensitive_fields) {
    // RFC 7541 section 7.1.3: credentials use the never-indexed literal form,
    // even when the static table holds an exact name/value match.
    for (const std::string_view name : {"authorization", "proxy-authorization", "cookie", "set-cookie"}) {
        std::string output;
        ruvia::hpack_encoder::encode_header(output, name, "");
        EXPECT_TRUE(!output.empty());
        EXPECT_EQ(static_cast<unsigned char>(output[0]) & 0xf0U, 0x10U);
        check_encoded_field(output, {}, name, "");
    }
}

TEST(Ruvia, hpack_public_table_size_rejects_out_of_range_integers_and_excess_updates) {
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
             static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1}) {
        EXPECT_TRUE(!ruvia::hpack_table_size::try_from_value(invalid).has_value());
        ruvia::hpack_decoder decoder;
        bool rejected_limit = false;
        try {
            decoder.set_max_dynamic_table_size(invalid);
        } catch (const std::invalid_argument&) {
            rejected_limit = true;
        }
        EXPECT_TRUE(rejected_limit);
    }
    EXPECT_TRUE(!ruvia::hpack_table_size::try_from_value((std::numeric_limits<std::uint64_t>::max)()).has_value());

    ruvia::hpack_decoder decoder;
    decoder.set_max_dynamic_table_size(64);
    EXPECT_TRUE(!decoder.decode(table_size_update(64), [](auto, auto) { return true; }).has_value());
    EXPECT_TRUE(decoder.decode(table_size_update(65), [](auto, auto) { return true; }) ==
                ruvia::hpack_decode_error::compression_error);
    EXPECT_TRUE(decoder_is_poisoned(decoder));
}

TEST(Ruvia, hpack_public_lowered_capacity_requires_a_leading_update) {
    // RFC 7541 section 4.2: the first block after the decoder's maximum shrinks
    // must begin with a dynamic table size update.
    for (const bool signaled : {false, true}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder.decode(indexed_literal("a", "b"), [](auto, auto) { return true; }).has_value());
        decoder.set_max_dynamic_table_size(0);
        std::string block = signaled ? table_size_update(0) : std::string();
        block.push_back(static_cast<char>(0x82));
        std::size_t fields = 0;
        const auto result = decoder.decode(block, [&](auto, auto) {
            ++fields;
            return true;
        });
        if (signaled) {
            EXPECT_TRUE(!result.has_value());
            EXPECT_EQ(fields, std::size_t{1});
            EXPECT_TRUE(decoder.decode(indexed_field(62), [](auto, auto) { return true; }) ==
                        ruvia::hpack_decode_error::compression_error);
        } else {
            EXPECT_TRUE(result == ruvia::hpack_decode_error::compression_error);
            EXPECT_EQ(fields, std::size_t{0});
        }
    }
}

TEST(Ruvia, hpack_public_encoder_preserves_the_output_terminator_as_input) {
    for (const bool empty_output : {false, true}) {
        std::string output(empty_output ? "" : "prefix");
        const std::string prefix(output);
        const std::string_view value(output.data() + output.size(), 1);
        ruvia::hpack_encoder::encode_header(output, "content-type", value);
        check_encoded_field(output, prefix, "content-type", std::string_view("\0", 1));
    }
}

TEST(Ruvia, hpack_public_dynamic_entries_own_fields_after_input_changes) {
    {
        ruvia::hpack_decoder decoder;
        {
            std::string block = indexed_literal(std::string(24, 'n'), std::string(24, 'v'));
            EXPECT_TRUE(!decoder.decode(block, [](auto, auto) { return true; }).has_value());
            block.assign(block.size(), '#');
        }
        {
            // Huffman-coded literal: the decoded bytes cannot borrow the input.
            std::string block;
            ruvia::hpack_encoder::encode_header(block, std::string(24, 'a'), std::string(25, 'b'));
            block[0] = static_cast<char>(0x40);
            EXPECT_TRUE(!decoder.decode(std::string_view(block), [](auto, auto) { return true; }).has_value());
            std::fill(block.begin(), block.end(), '#');
        }
        check_newest(decoder, 62, std::string(24, 'a'), std::string(25, 'b'));
        check_newest(decoder, 63, std::string(24, 'n'), std::string(24, 'v'));
        // RFC 7541 Appendix C.4.1: ":authority: www.example.com" as a Huffman literal.
        constexpr std::string_view huffman_authority(
            "\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 14);
        EXPECT_TRUE(!decoder.decode(huffman_authority, [](std::string_view name, std::string_view value) {
                                return name == ":authority" && value == "www.example.com";
                            })
                .has_value());
    }
}

TEST(Ruvia, hpack_public_empty_and_one_sided_fields_are_indexable) {
    ruvia::hpack_decoder decoder;
    const std::string long_bytes(49, 'x');
    const std::string block = indexed_literal("", "") + indexed_literal("", long_bytes) + indexed_literal(long_bytes, "");
    EXPECT_TRUE(!decoder.decode(block, [](auto, auto) { return true; }).has_value());
    check_newest(decoder, 62, long_bytes, "");
    check_newest(decoder, 63, "", long_bytes);
    check_newest(decoder, 64, "", "");
}

TEST(Ruvia, hpack_public_dynamic_entries_remain_indexable_across_evictions) {
    ruvia::hpack_decoder decoder;
    EXPECT_TRUE(!decoder.decode(table_size_update(120), [](auto, auto) { return true; }).has_value());
    const std::string name(40, 'n');
    const std::string value(40, 'v');
    const std::string insertion = indexed_literal(name, value);
    for (std::size_t i = 0; i < 200; ++i) {
        EXPECT_TRUE(!decoder.decode(insertion, [](auto, auto) { return true; }).has_value());
        check_newest(decoder, 62, name, value);
    }
}

TEST(Ruvia, hpack_public_leading_capacity_updates_apply_in_wire_order) {
    const std::array<std::array<std::uint32_t, 3>, 3> sequences{{{512, 64, 64}, {512, 64, 512}, {0, 0, 512}}};
    for (const auto& updates : sequences) {
        ruvia::hpack_decoder decoder;
        const std::string seed_value(64, 's');
        EXPECT_TRUE(!decoder.decode(indexed_literal("x-seed", seed_value), [](auto, auto) { return true; }).has_value());
        std::string block;
        for (const auto size : updates) {
            block += table_size_update(size);
        }
        block += indexed_literal("x", "fresh");
        std::size_t fields = 0;
        const auto decoded = decoder.decode(block, [&](std::string_view name, std::string_view value) {
            EXPECT_EQ(name, std::string_view("x"));
            EXPECT_EQ(value, std::string_view("fresh"));
            ++fields;
            return true;
        });
        EXPECT_TRUE(!decoded.has_value());
        EXPECT_EQ(fields, std::size_t{1});
        check_newest(decoder, 62, "x", "fresh");
        // Restoring capacity cannot restore an entry evicted at an intermediate minimum.
        EXPECT_TRUE(decoder.decode(indexed_field(63), [](auto, auto) { return true; }) ==
                    ruvia::hpack_decode_error::compression_error);
    }
}

TEST(Ruvia, hpack_public_capacity_updates_reject_excess_and_late_positions) {
    for (const bool late : {false, true}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder.decode(indexed_literal("x-seed", "kept"), [](auto, auto) { return true; }).has_value());
        const std::string invalid = late ? indexed_field(62) + table_size_update(0) : table_size_update(4097);
        EXPECT_TRUE(decoder.decode(invalid, [](auto, auto) { return true; }) ==
                    ruvia::hpack_decode_error::compression_error);
        EXPECT_TRUE(decoder_is_poisoned(decoder));
    }
}

TEST(Ruvia, hpack_public_callback_rejection_keeps_dynamic_table_consistent) {
    ruvia::hpack_decoder decoder;
    const auto rejected = decoder.decode(indexed_literal("a", "b"), [](auto, auto) { return false; });
    EXPECT_TRUE(rejected == ruvia::hpack_decode_error::callback_rejected);
    check_newest(decoder, 62, "a", "b");
}

TEST(Ruvia, hpack_public_compression_error_poisons_the_decoder) {
    for (const std::string_view failing : {std::string_view("\x80", 1), std::string_view("\x40\x05\x61", 3)}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder.decode(indexed_literal("a", "b"), [](auto, auto) { return true; }).has_value());
        std::string block = indexed_literal("c", "d");
        block.append(failing);
        EXPECT_TRUE(decoder.decode(block, [](auto, auto) { return true; }) ==
                    ruvia::hpack_decode_error::compression_error);
        EXPECT_TRUE(decoder_is_poisoned(decoder));
    }
}

TEST(Ruvia, hpack_public_long_dynamic_name_and_value_are_preserved) {
    ruvia::hpack_decoder decoder;
    const std::string name(300, 'n');
    const std::string value(300, 'v');
    EXPECT_TRUE(!decoder.decode(indexed_literal(name, value), [](auto, auto) { return true; }).has_value());
    check_newest(decoder, 62, name, value);
}

TEST(Ruvia, hpack_public_eviction_preserves_relative_indices) {
    ruvia::hpack_decoder decoder;
    const std::string long_name(48, 'n');
    std::string initial = table_size_update(1150);
    for (int i = 0; i < 36; ++i) {
        const char value = static_cast<char>('A' + i);
        initial += indexed_literal(i % 2 == 0 ? std::string_view("a") : std::string_view(long_name),
            std::string_view(&value, 1));
    }
    initial.push_back(static_cast<char>(0x82));
    EXPECT_TRUE(!decoder.decode(initial, [](auto, auto) { return true; }).has_value());
    std::string indices;
    for (std::uint32_t index = 62; index < 82; ++index) {
        indices += indexed_field(index);
    }
    int expected = 35;
    const auto result = decoder.decode(indices, [&](std::string_view name, std::string_view value) {
        const auto expected_name = expected % 2 == 0 ? std::string_view("a") : std::string_view(long_name);
        const bool matches = name == expected_name && value.size() == 1 && value[0] == 'A' + expected;
        --expected;
        return matches;
    });
    EXPECT_TRUE(!result.has_value());
    EXPECT_EQ(expected, 15);
    // An indexed name survives the eviction its own insertion causes.
    EXPECT_TRUE(!decoder.decode(std::string_view("\x7e\x01z", 3), [](auto, auto) { return true; }).has_value());
    check_newest(decoder, 62, long_name, "z");
}

TEST(Ruvia, hpack_public_decoder_move_preserves_dynamic_entries) {
    ruvia::hpack_decoder source_value;
    EXPECT_TRUE(!source_value.decode(indexed_literal("a", "b"), [](auto, auto) { return true; }).has_value());
    ruvia::hpack_decoder moved(std::move(source_value));
    ruvia::hpack_decoder destination;
    destination = std::move(moved);
    check_newest(destination, 62, "a", "b");
}

#if !defined(_MSC_VER)
TEST(Ruvia, hpack_public_callback_exception_propagates_across_remaining_fields) {
    ruvia::hpack_decoder decoder;

    std::string block(1, static_cast<char>(0x82));  // indexed :method: GET
    block += indexed_literal(std::string(100, 'x'), "y");

    bool saw_original = false;
    try {
        (void)decoder.decode(block, [&](std::string_view, std::string_view) -> bool {
            throw std::runtime_error("original callback failure");
        });
    } catch (const std::runtime_error& error) {
        saw_original = std::string_view(error.what()) == "original callback failure";
    }
    EXPECT_TRUE(saw_original);
}
#endif

TEST(Ruvia, hpack_public_callback_cannot_replace_the_active_block_or_its_capacity) {
    for (const bool resize : {false, true}) {
        ruvia::hpack_decoder decoder;
        ruvia::hpack_decoder independent;
        std::size_t callbacks = 0;
        std::size_t rejected = 0;
        const auto result = decoder.decode(indexed_literal("a", "b"), [&](std::string_view name, std::string_view value) {
            ++callbacks;
            try {
                if (resize) {
                    decoder.set_max_dynamic_table_size(0);
                } else {
                    (void)decoder.decode({}, [](auto, auto) { return true; });
                }
            } catch (const std::logic_error&) {
                ++rejected;
            }
            EXPECT_EQ(name, "a");
            EXPECT_EQ(value, "b");
            std::size_t independent_fields = 0;
            EXPECT_TRUE(!independent.decode(std::string_view("\x82", 1), [&](auto inner_name, auto inner_value) {
                                        ++independent_fields;
                                        EXPECT_EQ(inner_name, ":method");
                                        EXPECT_EQ(inner_value, "GET");
                                        return true;
                                    })
                    .has_value());
            EXPECT_EQ(independent_fields, std::size_t{1});
            return true;
        });
        EXPECT_TRUE(!result.has_value());
        EXPECT_EQ(callbacks, std::size_t{1});
        EXPECT_EQ(rejected, std::size_t{1});
        check_newest(decoder, 62, "a", "b");
    }
}

TEST(Ruvia, hpack_public_callback_state_error_is_rethrown_after_the_block) {
    for (const bool resize : {false, true}) {
        for (const bool malformed : {false, true}) {
            ruvia::hpack_decoder decoder;
            const auto accept = [](auto, auto) { return true; };
            EXPECT_TRUE(!decoder.decode(indexed_literal("s", "t"), accept).has_value());
            std::string block = indexed_literal("a", "b") + indexed_literal("c", "d");
            if (malformed) {
                block.push_back(static_cast<char>(0x80));
            }
            std::size_t callbacks = 0;
            bool state_error = false;
            try {
                (void)decoder.decode(block, [&](auto, auto) {
                    ++callbacks;
                    if (resize) {
                        decoder.set_max_dynamic_table_size(0);
                    } else {
                        (void)decoder.decode({}, accept);
                    }
                    return true;
                });
            } catch (const std::logic_error&) {
                state_error = true;
            }
            EXPECT_TRUE(state_error);
            EXPECT_EQ(callbacks, std::size_t{1});
            if (malformed) {
                EXPECT_TRUE(decoder_is_poisoned(decoder));
            } else {
                check_newest(decoder, 62, "c", "d");
                check_newest(decoder, 63, "a", "b");
            }
        }
    }
}

TEST(Ruvia, hpack_encoder_uses_canonical_wire_indices_for_every_static_entry) {
    ruvia::hpack_decoder decoder;
    // RFC 7541 Appendix A defines the 61 one-based static table indices.
    for (std::uint32_t index = 1; index <= 61; ++index) {
        const char indexed_field = static_cast<char>(0x80U | index);
        std::string encoded;
        std::string name;
        std::size_t fields_value = 0;
        const auto decoded = decoder.decode(std::string_view(&indexed_field, 1),
            [&](std::string_view field_name, std::string_view value) {
                ++fields_value;
                name.assign(field_name);
                ruvia::hpack_encoder::encode_header(encoded, field_name, value);
                return true;
            });
        EXPECT_TRUE(!decoded.has_value());
        EXPECT_EQ(fields_value, std::size_t{1});
        // Credentials are never indexed (RFC 7541 section 7.1.3), so their
        // static entries are referenced by name only.
        if (name == "authorization" || name == "proxy-authorization" || name == "cookie" || name == "set-cookie") {
            continue;
        }
        EXPECT_EQ(encoded, std::string_view(&indexed_field, 1));
    }
}

namespace http2_receive_credit_test {

std::size_t advertised_receive_window(std::string_view output) {
    output.remove_prefix(ruvia::http2_client_preface.size());
    std::uint32_t connection_window = 65535;
    std::uint32_t stream_window = 65535;
    while (!output.empty()) {
        const auto header = ruvia::parse_http2_frame_header(std::span(output.data(), output.size()));
        if (!header || output.size() < ruvia::http2_frame_header_bytes + header->length_) {
            throw std::logic_error("invalid initial HTTP/2 output");
        }
        const auto payload = output.substr(ruvia::http2_frame_header_bytes, header->length_);
        if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings)) {
            for (std::size_t index = 0; index + 6 <= payload.size(); index += 6) {
                if (payload[index] == 0 && payload[index + 1] == 4) {
                    stream_window = read_u32(payload.substr(index + 2));
                }
            }
        } else if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::window_update) && header->stream_id_ == 0) {
            connection_window += read_u32(payload) & 0x7fffffffU;
        }
        output.remove_prefix(ruvia::http2_frame_header_bytes + header->length_);
    }
    return std::min(connection_window, stream_window);
}

void append_body(std::string& wire, std::size_t bytes) {
    const std::string chunk(16384, 'x');
    while (bytes != 0) {
        const auto count = std::min(bytes, chunk.size());
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1,
            std::string_view(chunk).substr(0, count));
        bytes -= count;
    }
}

}  // namespace http2_receive_credit_test
using namespace http2_receive_credit_test;

// Acknowledging a credit for the whole advertised receive window restores
// exactly one window on the connection and on the stream.
TEST(Ruvia, http2_public_receive_credit_restores_exact_window) {
    auto client = ruvia::http2_connection::client({});
    const auto receive_window = advertised_receive_window(ruvia::test::drain_output(client));
    EXPECT_TRUE(receive_window != 0);
    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    (void)ruvia::test::drain_output(client);

    std::string wire;
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    // The client's advertised stream window applies once the peer ACKs it.
    append_frame(wire, ruvia::http2_frame_type::settings, 0x1, 0, {});
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":status", "200");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    append_body(wire, receive_window);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);

    std::optional<ruvia::http2_received_data_credit> credit;
    while (auto event = client.next_event()) {
        if (auto* body = event->message_body_chunk()) {
            if (credit) {
                credit->merge(body->take_credit());
            } else {
                credit.emplace(body->take_credit());
            }
        }
    }
    EXPECT_TRUE(credit && credit->valid());
    if (!credit) {
        return;
    }
    // Received bytes are credited only once their credit is destroyed.
    std::string prefix;
    append_frame(prefix, ruvia::http2_frame_type::settings, 0x1, 0, {});
    EXPECT_TRUE(ruvia::test::drain_output(client) == std::string_view(prefix));
    credit.reset();

    const auto output = ruvia::test::drain_output(client);
    std::string_view updates(output);
    std::size_t connection_updates = 0;
    std::size_t stream_updates = 0;
    while (!updates.empty()) {
        const auto header = ruvia::parse_http2_frame_header(std::span(updates.data(), updates.size()));
        EXPECT_TRUE(header.has_value());
        if (!header) {
            return;
        }
        EXPECT_TRUE(header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::window_update));
        EXPECT_EQ(header->length_, 4u);
        EXPECT_EQ(read_u32(updates.substr(ruvia::http2_frame_header_bytes)), receive_window);
        connection_updates += header->stream_id_ == 0;
        stream_updates += header->stream_id_ == 1;
        updates.remove_prefix(ruvia::http2_frame_header_bytes + header->length_);
    }
    EXPECT_EQ(connection_updates, std::size_t{1});
    EXPECT_EQ(stream_updates, std::size_t{1});
    EXPECT_TRUE(!client.wants_write());

    wire.clear();
    append_body(wire, receive_window + 1);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(client.connection_error() == ruvia::http2_error_code::flow_control_error);
}

namespace http2_request_pseudo_headers_test {

using pseudo_header = ruvia::http2_request_pseudo_header;

void encode_pseudo_header(std::string& block, pseudo_header header) {
    switch (header) {
        case pseudo_header::method:
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            break;
        case pseudo_header::scheme:
            ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
            break;
        case pseudo_header::authority:
            ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
            break;
        case pseudo_header::path:
            ruvia::hpack_encoder::encode_header(block, ":path", "/");
            break;
        case pseudo_header::protocol:
            ruvia::hpack_encoder::encode_header(block, ":protocol", "websocket");
            break;
    }
}

bool same_order(std::span<const pseudo_header> actual, std::span<const pseudo_header> expected) {
    return std::equal(actual.begin(), actual.end(), expected.begin(), expected.end());
}

}  // namespace http2_request_pseudo_headers_test
using namespace http2_request_pseudo_headers_test;

TEST(Ruvia, http2_request_pseudo_headers_preserve_every_regular_order) {
    std::array order{pseudo_header::method, pseudo_header::scheme, pseudo_header::authority, pseudo_header::path};
    do {
        auto server = ruvia::http2_connection::server();
        std::string block;
        for (const auto header : order) {
            encode_pseudo_header(block, header);
        }
        auto wire = opening_wire();
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        EXPECT_TRUE(head.has_value());
        if (head) {
            const auto view = head->request_view();
            EXPECT_TRUE(same_order(view.pseudo_header_order_, order));
            EXPECT_TRUE(view.method_ == "GET" && view.scheme_ == "https" && view.path_ == "/" && view.authority_ == "example.test");
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(Ruvia, http2_request_pseudo_headers_do_not_synthesize_authority_from_host) {
    auto server = ruvia::http2_connection::server();
    constexpr std::array order{pseudo_header::path, pseudo_header::method, pseudo_header::scheme};
    std::string block;
    for (const auto header : order) {
        encode_pseudo_header(block, header);
    }
    ruvia::hpack_encoder::encode_header(block, "host", "example.test");
    auto wire = opening_wire();
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    EXPECT_TRUE(head.has_value());
    if (head) {
        EXPECT_TRUE(head->request_view().authority_ == "example.test");
        EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, order));
    }
}

TEST(Ruvia, http2_request_pseudo_headers_huffman_dynamic_index_and_continuation) {
    // RFC 7541 C.4.1: Huffman authority inserted at dynamic index 62.
    const std::string block("\x82\x86\x84\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 17);
    for (std::size_t split = 0; split <= block.size(); ++split) {
        auto server = ruvia::http2_connection::server();
        auto first = opening_wire();
        append_frame(first, ruvia::http2_frame_type::headers, 0x1, 1, std::string_view(block).substr(0, split));
        (void)server.feed(first);
        EXPECT_TRUE(!server.next_event());
        std::string final;
        append_frame(final, ruvia::http2_frame_type::continuation, 0x4, 1, std::string_view(block).substr(split));
        EXPECT_TRUE(server.feed(final) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        constexpr std::array expected{pseudo_header::method, pseudo_header::scheme, pseudo_header::path, pseudo_header::authority};
        EXPECT_TRUE(head.has_value());
        if (head) {
            EXPECT_TRUE(head->request_view().authority_ == "www.example.com");
            EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, expected));
        }
        std::string next;
        append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, "\xbe\x84\x87\x82");
        EXPECT_TRUE(server.feed(next) == ruvia::http2_feed_result::accepted);
        auto indexed = take_request_head(server);
        constexpr std::array indexed_order{pseudo_header::authority, pseudo_header::path, pseudo_header::scheme, pseudo_header::method};
        EXPECT_TRUE(indexed.has_value());
        if (indexed) {
            EXPECT_TRUE(indexed->request_view().authority_ == "www.example.com");
            EXPECT_TRUE(same_order(indexed->request_view().pseudo_header_order_, indexed_order));
        }
        EXPECT_TRUE(!server.connection_error());
    }
}

TEST(Ruvia, http2_request_pseudo_headers_preserve_both_connect_forms) {
    for (const bool extended : {false, true}) {
        auto server = ruvia::http2_connection::server();
        (void)ruvia::test::drain_output(server);
        std::string block;
        if (extended) {
            encode_pseudo_header(block, pseudo_header::protocol);
            encode_pseudo_header(block, pseudo_header::path);
            encode_pseudo_header(block, pseudo_header::scheme);
        }
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test:443");
        ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
        if (extended) {
            ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
        }
        auto wire = opening_wire();
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        constexpr std::array extended_order{pseudo_header::protocol, pseudo_header::path, pseudo_header::scheme, pseudo_header::authority, pseudo_header::method};
        const auto expected = std::span(extended_order).subspan(extended ? 0 : 3);
        EXPECT_TRUE(head.has_value());
        if (head) {
            EXPECT_TRUE(head->snapshot().connect_pending_);
            EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, expected));
        }
    }
}

TEST(Ruvia, http2_invalid_pseudo_headers_do_not_publish_but_keep_dynamic_table_sync) {
    auto server = ruvia::http2_connection::server();
    auto wire = opening_wire();
    // A duplicate :method fails request semantics, but the later incremental
    // :path literal must still become dynamic index 62 (RFC 7541 section 2.3.2).
    const std::string malformed("\x82\x82\x44\x07/synced", 11);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, malformed);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!take_request_head(server));
    EXPECT_TRUE(server.stream_aborted(1));
    std::string indexed("\xbe\x87\x82", 3);
    encode_pseudo_header(indexed, pseudo_header::authority);
    std::string next;
    append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, indexed);
    EXPECT_TRUE(server.feed(next) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    constexpr std::array expected{pseudo_header::path, pseudo_header::scheme, pseudo_header::method, pseudo_header::authority};
    EXPECT_TRUE(head.has_value());
    if (head) {
        EXPECT_EQ(head->stream_id(), std::uint32_t{3});
        EXPECT_TRUE(head->request_view().path_ == "/synced");
        EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, expected));
    }
    EXPECT_TRUE(!server.connection_error());
}

TEST(Ruvia, http2_request_pseudo_header_lease_survives_reset_move_and_later_input) {
    {
        std::optional<ruvia::http2_request_head_event> lease;
        ruvia::http2_server_request_view saved;
        constexpr std::array expected{pseudo_header::authority, pseudo_header::path, pseudo_header::method, pseudo_header::scheme};
        {
            auto server = ruvia::http2_connection::server({});
            std::string block;
            for (const auto header : expected) {
                encode_pseudo_header(block, header);
            }
            auto wire = opening_wire();
            append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto received = take_request_head(server);
            EXPECT_TRUE(received.has_value());
            if (!received) {
                return;
            }
            lease.emplace(std::move(*received));
            saved = lease->request_view();
            EXPECT_TRUE(same_order(saved.pseudo_header_order_, expected));
            auto moved = std::move(server);
            std::string next;
            append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, block);
            EXPECT_TRUE(moved.feed(next) == ruvia::http2_feed_result::accepted);
            auto following = take_request_head(moved);
            EXPECT_TRUE(following.has_value());
            EXPECT_TRUE(same_order(saved.pseudo_header_order_, expected));
            EXPECT_TRUE(same_order(lease->request_view().pseudo_header_order_, expected));
            moved = ruvia::http2_connection::server({});
        }
        EXPECT_TRUE(saved.authority_ == "example.test");
        EXPECT_TRUE(same_order(saved.pseudo_header_order_, expected));
        EXPECT_TRUE(same_order(lease->request_view().pseudo_header_order_, expected));
    }
}

TEST(Ruvia, http2_locally_synthesized_push_has_no_received_pseudo_header_order) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"})));
    relay.transfer(client, server);
    auto parent = take_request_head(server);
    EXPECT_TRUE(parent.has_value());
    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/asset"});
    EXPECT_TRUE(pushed.index() == 0);
    if (pushed.index() == 0) {
        EXPECT_TRUE(std::get<0>(pushed).request_view().pseudo_header_order_.empty());
        EXPECT_TRUE(std::get<0>(pushed).request_view().path_ == "/asset");
    }
}

TEST(Ruvia, http2_request_path_rejects_empty_and_defers_scheme_validation_to_finish) {
    // RFC 9113 §8.3.1: :path must be non-empty on wire; non-http(s) schemes
    // accept non-origin-form :path in finish_request_head.
    {
        auto server = ruvia::http2_connection::server();
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "custom");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "opaque-path");
        auto wire = opening_wire();
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        EXPECT_EQ(server.feed(wire), ruvia::http2_feed_result::accepted);
        const auto head = take_request_head(server);
        ASSERT_TRUE(head.has_value());
        EXPECT_EQ(head->request_view().path_, "opaque-path");
    }
    {
        auto server = ruvia::http2_connection::server();
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "custom");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "");
        auto wire = opening_wire();
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        EXPECT_EQ(server.feed(wire), ruvia::http2_feed_result::accepted);
        EXPECT_FALSE(take_request_head(server).has_value());
        EXPECT_TRUE(server.stream_aborted(1));
    }
}

namespace http2_receive_metadata_test {

template <typename value_type>
concept settings_borrowable = requires(value_type&& value) {
    std::forward<value_type>(value).settings();
};
static_assert(settings_borrowable<const ruvia::http2_frame_metadata&>);
static_assert(!settings_borrowable<ruvia::http2_frame_metadata>);

enum class received_kind : std::uint8_t { settings,
    window_update,
    priority,
    request_head };

struct received_record final {
    received_kind kind_{received_kind::settings};
    std::array<ruvia::http2_setting_entry, 8> settings_{};
    std::size_t settings_count_{0};
    bool checked_bounds_{false};
    std::uint32_t stream_id_{0};
    std::uint32_t value_{0};
    std::uint16_t weight_{0};
    bool exclusive_{false};
    bool request_matches_{false};
    std::array<ruvia::http2_request_pseudo_header, 5> pseudo_order_{};
    std::size_t pseudo_count_{0};
};

// 按 next_event() 顺序记录元数据与请求头事件，其余事件不在本文件观察范围内。
// 请求头租约保留到日志析构，使对应流保持存活。
struct received_log final {
    std::array<received_record, 16> records_{};
    std::size_t count_{0};
    bool overflow_{false};
    std::vector<ruvia::http2_request_head_event> requests_;

    void drain(ruvia::http2_connection& connection) {
        while (auto event = connection.next_event()) {
            append(*event);
        }
    }

private:
    void append(ruvia::http2_event& event) {
        const auto* settings = event.received_settings();
        const auto* window = event.received_window_update();
        const auto* priority = event.received_priority();
        auto* request = event.request_head();
        if (settings == nullptr && window == nullptr && priority == nullptr && request == nullptr) {
            return;
        }
        if (count_ == records_.size()) {
            overflow_ = true;
            return;
        }
        auto& record = records_[count_++];
        if (settings != nullptr) {
            record.kind_ = received_kind::settings;
            record.settings_count_ = settings->size();
            if (record.settings_count_ > record.settings_.size()) {
                overflow_ = true;
                return;
            }
            for (std::size_t index = 0; index != record.settings_count_; ++index) {
                record.settings_[index] = settings->at(index);
            }
            try {
                (void)settings->at(settings->size());
            } catch (const std::out_of_range&) {
                record.checked_bounds_ = true;
            }
        } else if (window != nullptr) {
            record.kind_ = received_kind::window_update;
            record.stream_id_ = window->stream_id_;
            record.value_ = window->increment_;
        } else if (priority != nullptr) {
            record.kind_ = received_kind::priority;
            record.stream_id_ = priority->stream_id_;
            record.value_ = priority->stream_dependency_;
            record.weight_ = priority->weight_;
            record.exclusive_ = priority->exclusive_;
        } else {
            record.kind_ = received_kind::request_head;
            record.stream_id_ = request->stream_id();
            const auto view = request->request_view();
            record.request_matches_ = view.method_ == "GET" && view.scheme_ == "https" &&
                                      view.authority_ == "example.test" && view.path_ == "/metadata";
            record.pseudo_count_ = view.pseudo_header_order_.size();
            if (record.pseudo_count_ > record.pseudo_order_.size()) {
                overflow_ = true;
                return;
            }
            for (std::size_t index = 0; index != record.pseudo_count_; ++index) {
                record.pseudo_order_[index] = view.pseudo_header_order_[index];
            }
            requests_.push_back(std::move(*request));
        }
    }
};

void append_setting(std::string& payload, std::uint16_t id, std::uint32_t value) {
    const std::array<char, 6> entry{static_cast<char>(id >> 8), static_cast<char>(id),
        static_cast<char>(value >> 24), static_cast<char>(value >> 16),
        static_cast<char>(value >> 8), static_cast<char>(value)};
    payload.append(entry.data(), entry.size());
}

std::string window_payload(std::uint32_t increment) {
    const std::array<char, 4> bytes{static_cast<char>(increment >> 24), static_cast<char>(increment >> 16),
        static_cast<char>(increment >> 8), static_cast<char>(increment)};
    return std::string(bytes.data(), bytes.size());
}

std::string priority_payload(std::uint32_t dependency, unsigned weight, bool exclusive) {
    auto payload = window_payload(dependency | (exclusive ? 0x80000000U : 0));
    payload.push_back(static_cast<char>(weight - 1));
    return payload;
}

std::string request_block() {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/metadata");
    return block;
}

std::string server_preface() {
    std::string wire(ruvia::http2_client_preface);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
    return wire;
}

}  // namespace http2_receive_metadata_test
using namespace http2_receive_metadata_test;

TEST(Ruvia, http2_receive_frame_metadata_off_produces_no_metadata_events) {
    for (const auto role : {ruvia::http2_role::server, ruvia::http2_role::client}) {
        auto connection = role == ruvia::http2_role::server ? ruvia::http2_connection::server()
                                                            : ruvia::http2_connection::client();
        std::string settings;
        append_setting(settings, 3, 13);
        append_setting(settings, 0xfefe, 1);
        std::string wire = role == ruvia::http2_role::server ? std::string(ruvia::http2_client_preface) : std::string{};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
        append_frame(wire, ruvia::http2_frame_type::priority, 0, 7, priority_payload(0, 16, false));
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(5));
        if (role == ruvia::http2_role::server) {
            const auto block = request_block();
            auto headers = priority_payload(0, 32, true);
            headers.append(block.data(), block.size());
            append_frame(wire, ruvia::http2_frame_type::headers, 0x24, 1, headers);
            append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, window_payload(5));
        }
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        received_log log;
        log.drain(connection);
        EXPECT_TRUE(!log.overflow_);
        if (role == ruvia::http2_role::server) {
            EXPECT_EQ(log.count_, 1u);
            EXPECT_TRUE(log.records_[0].kind_ == received_kind::request_head);
            EXPECT_TRUE(log.records_[0].request_matches_);
        } else {
            EXPECT_EQ(log.count_, 0u);
        }
        EXPECT_TRUE(!connection.connection_error());
    }
}

// nghttp2 folds a repeated known identifier into its first position with the
// last value, keeps unknown identifiers in wire order, and owns the decoded
// entries, so the event outlives the input.
TEST(Ruvia, http2_receive_settings_fold_known_duplicates_and_keep_unknown_entries) {
    for (const auto role : {ruvia::http2_role::server, ruvia::http2_role::client}) {
        const ruvia::http2_connection_options options{.receive_frame_metadata_ = true};
        auto connection = role == ruvia::http2_role::server ? ruvia::http2_connection::server(options)
                                                            : ruvia::http2_connection::client(options);
        std::string payload;
        append_setting(payload, 0xfefe, 0xabcdef01);
        append_setting(payload, 3, 13);
        append_setting(payload, 3, 7);
        append_setting(payload, 0xfefe, 0);
        std::string wire = role == ruvia::http2_role::server ? std::string(ruvia::http2_client_preface) : std::string{};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, payload);
        append_frame(wire, ruvia::http2_frame_type::settings, 1, 0);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        wire.assign(wire.size(), '#');
        received_log log;
        log.drain(connection);
        EXPECT_EQ(log.count_, 2u);
        EXPECT_TRUE(!log.overflow_);
        const auto& first = log.records_[0];
        EXPECT_TRUE(first.kind_ == received_kind::settings);
        EXPECT_EQ(first.settings_count_, 3u);
        EXPECT_TRUE(first.checked_bounds_);
        EXPECT_EQ(static_cast<std::uint16_t>(first.settings_[0].id_), 0xfefe);
        EXPECT_EQ(first.settings_[0].value_, 0xabcdef01u);
        EXPECT_TRUE(first.settings_[1].id_ == ruvia::http2_setting_id::max_concurrent_streams);
        EXPECT_EQ(first.settings_[1].value_, 7u);
        EXPECT_EQ(static_cast<std::uint16_t>(first.settings_[2].id_), 0xfefe);
        EXPECT_EQ(first.settings_[2].value_, 0u);
        EXPECT_TRUE(log.records_[1].kind_ == received_kind::settings);
        EXPECT_EQ(log.records_[1].settings_count_, 0u);
        EXPECT_TRUE(log.records_[1].checked_bounds_);
    }
    const ruvia::http2_received_settings empty;
    bool threw = false;
    try {
        (void)empty.at((std::numeric_limits<std::size_t>::max)());
    } catch (const std::out_of_range&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, http2_receive_invalid_later_setting_does_not_publish_partial_frame) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    std::string wire = server_preface();
    std::string payload;
    append_setting(payload, 3, 19);
    append_setting(payload, 4, 0x80000000U);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, payload);
    append_frame(wire, ruvia::http2_frame_type::priority, 0, 7, priority_payload(0, 1, false));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::flow_control_error);
    received_log log;
    log.drain(server);
    EXPECT_EQ(log.count_, 1u);
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
    EXPECT_EQ(log.records_[0].settings_count_, 0u);
}

TEST(Ruvia, http2_receive_byte_fragments_publish_only_complete_frames) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    received_log log;
    std::string wire = server_preface();
    const auto settings_end = wire.size();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(5));
    const auto first_window_end = wire.size();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(11));
    for (std::size_t index = 0; index != wire.size(); ++index) {
        const auto status = server.feed(std::string_view(wire).substr(index, 1));
        EXPECT_TRUE(status == ruvia::http2_feed_result::accepted);
        log.drain(server);
        const auto expected = static_cast<std::size_t>(index + 1 >= settings_end) +
                              static_cast<std::size_t>(index + 1 >= first_window_end) +
                              static_cast<std::size_t>(index + 1 == wire.size());
        EXPECT_EQ(log.count_, expected);
    }
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
    EXPECT_TRUE(log.records_[1].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[1].value_, 5u);
    EXPECT_TRUE(log.records_[2].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[2].value_, 11u);
    EXPECT_TRUE(!server.connection_error());
}

TEST(Ruvia, http2_receive_padded_headers_priority_precedes_continued_request_and_later_metadata) {
    for (const bool continued : {false, true}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
        received_log log;
        auto block = request_block();
        const auto split = continued ? block.size() / 2 : block.size();
        std::string payload(1, '\x03');
        payload += priority_payload(0, 256, true);
        payload.append(block.data(), split);
        payload.append(3, '\0');
        std::string wire = server_preface();
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(3));
        append_frame(wire, ruvia::http2_frame_type::headers, continued ? 0x29 : 0x2d, 1, payload);
        if (continued) {
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            log.drain(server);
            EXPECT_EQ(log.count_, 3u);
            EXPECT_TRUE(server.header_block_in_progress());
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::continuation, 4, 1, std::string_view(block).substr(split));
        }
        // A PRIORITY frame publishes nothing (RFC 9113 §5.3.2 deprecates it).
        append_frame(wire, ruvia::http2_frame_type::priority, 0, 5, priority_payload(0, 1, false));
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(9));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        EXPECT_EQ(log.count_, 5u);
        EXPECT_TRUE(!log.overflow_);
        const std::array kinds{received_kind::settings, received_kind::window_update, received_kind::priority,
            received_kind::request_head, received_kind::window_update};
        for (std::size_t index = 0; index != kinds.size(); ++index) {
            EXPECT_TRUE(log.records_[index].kind_ == kinds[index]);
        }
        EXPECT_EQ(log.records_[1].value_, 3u);
        EXPECT_EQ(log.records_[2].stream_id_, 1u);
        EXPECT_EQ(log.records_[2].value_, 0u);
        EXPECT_EQ(log.records_[2].weight_, 256u);
        EXPECT_TRUE(log.records_[2].exclusive_);
        const auto& request = log.records_[3];
        EXPECT_TRUE(request.request_matches_);
        EXPECT_EQ(request.stream_id_, 1u);
        EXPECT_EQ(request.pseudo_count_, 4u);
        const std::array expected{ruvia::http2_request_pseudo_header::method, ruvia::http2_request_pseudo_header::scheme,
            ruvia::http2_request_pseudo_header::authority, ruvia::http2_request_pseudo_header::path};
        for (std::size_t index = 0; index != expected.size(); ++index) {
            EXPECT_TRUE(request.pseudo_order_[index] == expected[index]);
        }
        EXPECT_EQ(log.records_[4].value_, 9u);
        EXPECT_TRUE(!server.connection_error());
    }
}

TEST(Ruvia, http2_receive_window_updates_only_report_applied_credit) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    received_log log;
    auto block = request_block();
    std::string wire = server_preface();
    append_frame(wire, ruvia::http2_frame_type::headers, 5, 3, block);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    log.drain(server);
    EXPECT_EQ(log.count_, 2u);
    EXPECT_TRUE(log.records_[1].kind_ == received_kind::request_head);
    log.count_ = 0;
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, window_payload(0));  // 跳过的闭流：忽略。
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 3, window_payload(17));
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(23));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    log.drain(server);
    EXPECT_EQ(log.count_, 2u);
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[0].stream_id_, 3u);
    EXPECT_EQ(log.records_[0].value_, 17u);
    EXPECT_TRUE(log.records_[1].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[1].stream_id_, 0u);
    EXPECT_EQ(log.records_[1].value_, 23u);
    EXPECT_TRUE(!server.connection_error());
    // nghttp2 treats a zero increment on a live stream as a connection error.
    log.count_ = 0;
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 3, window_payload(0));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    log.drain(server);
    EXPECT_EQ(log.count_, 0u);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
}

TEST(Ruvia, http2_receive_rejected_connection_window_and_malformed_priority_are_not_published) {
    for (const auto type : {ruvia::http2_frame_type::window_update, ruvia::http2_frame_type::priority}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
        std::string wire = server_preface();
        append_frame(wire, type, 0, type == ruvia::http2_frame_type::window_update ? 0 : 1, window_payload(0));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        received_log log;
        log.drain(server);
        EXPECT_EQ(log.count_, 1u);
        EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
        EXPECT_TRUE(server.connection_error() == (type == ruvia::http2_frame_type::window_update
                                                         ? ruvia::http2_error_code::protocol_error
                                                         : ruvia::http2_error_code::frame_size_error));
    }
}

TEST(Ruvia, http2_receive_control_metadata_needs_no_dynamic_allocation) {
    // PRIORITY frames are deprecated (RFC 9113 §5.3.2) and publish nothing.
    for (const bool publish : {false, true}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = publish});
        received_log log;
        const auto preface = server_preface();
        EXPECT_TRUE(server.feed(preface) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        log.count_ = 0;
        std::string priority;
        append_frame(priority, ruvia::http2_frame_type::priority, 0, 13, priority_payload(5, 19, false));
        std::string window;
        append_frame(window, ruvia::http2_frame_type::window_update, 0, 0, window_payload(7));
        EXPECT_TRUE(server.feed(priority) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        EXPECT_TRUE(server.feed(window) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        EXPECT_EQ(log.count_, publish ? 1u : 0u);
        EXPECT_TRUE(!server.connection_error());
    }
}

TEST(Ruvia, http2_receive_malformed_headers_do_not_publish_priority_or_request) {
    // Invalid padding and a payload too short for the PRIORITY fields are both
    // reported by nghttp2 as connection PROTOCOL_ERROR.
    for (const bool missing_priority : {false, true}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
        std::string wire = server_preface();
        std::string payload(1, missing_priority ? '\0' : '\x08');
        payload += priority_payload(0, 1, false);
        if (missing_priority) {
            payload.pop_back();
        }
        append_frame(wire, ruvia::http2_frame_type::headers, 0x2d, 1, payload);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        received_log log;
        log.drain(server);
        EXPECT_EQ(log.count_, 1u);
        EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
        EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
    }
}

TEST(Ruvia, http2_receive_window_overflow_does_not_publish_rejected_credit) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    std::string wire = server_preface();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(0x7fffffffU));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    received_log log;
    log.drain(server);
    EXPECT_EQ(log.count_, 1u);
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::flow_control_error);
}

namespace http2_receive_metadata_test {
struct wire_observation_log final {
    std::vector<ruvia::http2_frame_metadata> frames_;
    struct field final {
        std::uint64_t first_;
        std::uint64_t frame_;
        std::size_t index_;
        std::string name_;
        std::string value_;
    };
    std::vector<field> fields_;
    std::size_t begins_{0};
    std::size_t ends_{0};

    static void frame(void* context, const ruvia::http2_frame_metadata& metadata) noexcept {
        static_cast<wire_observation_log*>(context)->frames_.push_back(metadata);
    }
    static void header(void* context, const ruvia::http2_header_metadata& metadata) noexcept {
        auto& log = *static_cast<wire_observation_log*>(context);
        switch (metadata.phase_) {
            case ruvia::http2_header_observation_phase::begin:
                ++log.begins_;
                break;
            case ruvia::http2_header_observation_phase::end:
                ++log.ends_;
                break;
            case ruvia::http2_header_observation_phase::field:
                log.fields_.push_back({metadata.first_frame_observation_sequence_, metadata.frame_observation_sequence_,
                    metadata.field_index_, std::string(metadata.name_), std::string(metadata.value_)});
                break;
        }
    }
};
}  // namespace http2_receive_metadata_test
using namespace http2_receive_metadata_test;

TEST(Ruvia, http2_wire_observers_preserve_raw_settings_priority_and_fragmented_hpack) {
    for (const auto fragment : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
        wire_observation_log observed;
        auto server = ruvia::http2_connection::server({.frame_observer_ = &wire_observation_log::frame,
            .header_observer_ = &wire_observation_log::header,
            .observer_context_ = &observed});
        std::string settings;
        append_setting(settings, 3, 17);
        append_setting(settings, 0xfafa, 123);
        append_setting(settings, 3, 29);
        append_setting(settings, 1, 1024);
        append_setting(settings, 1, 4096);
        std::string wire(ruvia::http2_client_preface);
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(7340033));
        append_frame(wire, ruvia::http2_frame_type::priority, 0, 3, priority_payload(0, 256, true));
        // RFC 7541 C.4.1: Huffman-coded authority inserted at dynamic index 62.
        const std::string_view block("\x82\x86\x84\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 17);
        std::string payload(1, '\x03');
        payload += priority_payload(3, 42, false);
        payload += block.substr(0, 6);
        payload.append(3, '\0');
        append_frame(wire, ruvia::http2_frame_type::headers, 0x29, 1, payload);
        append_frame(wire, ruvia::http2_frame_type::continuation, 0x4, 1, block.substr(6));
        // A second block uses that dynamic-table entry in a different order.
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 3,
            std::string_view("\xbe\x84\x87\x82", 4));
        received_log requests;
        for (std::size_t cursor = 0; cursor < wire.size(); cursor += fragment) {
            EXPECT_TRUE(server.feed(std::string_view(wire).substr(cursor, fragment)) ==
                        ruvia::http2_feed_result::accepted);
            requests.drain(server);
        }
        ASSERT_EQ(observed.frames_.size(), 6u);
        for (std::size_t i = 0; i < observed.frames_.size(); ++i) {
            EXPECT_EQ(observed.frames_[i].observation_sequence_, i);
        }
        const auto& raw = observed.frames_[0];
        ASSERT_EQ(raw.settings().size(), 5u);
        EXPECT_TRUE(raw.settings_complete_);
        EXPECT_EQ(static_cast<std::uint16_t>(raw.settings()[1].id_), 0xfafa);
        EXPECT_EQ(raw.settings()[0].value_, 17u);
        EXPECT_EQ(raw.settings()[2].value_, 29u);
        EXPECT_EQ(raw.settings()[3].value_, 1024u);
        EXPECT_EQ(raw.settings()[4].value_, 4096u);
        ASSERT_TRUE(observed.frames_[1].window_increment_);
        EXPECT_EQ(*observed.frames_[1].window_increment_, 7340033u);
        ASSERT_TRUE(observed.frames_[2].priority_);
        EXPECT_EQ(observed.frames_[2].priority_->weight_, 256u);
        EXPECT_TRUE(observed.frames_[2].priority_->exclusive_);
        ASSERT_TRUE(observed.frames_[3].priority_);
        EXPECT_EQ(observed.frames_[3].priority_->stream_dependency_, 3u);
        EXPECT_EQ(observed.frames_[3].priority_->weight_, 42u);
        EXPECT_TRUE(observed.frames_[4].type_ == ruvia::http2_frame_type::continuation);
        ASSERT_EQ(observed.fields_.size(), 8u);
        EXPECT_EQ(observed.fields_[0].name_, ":method");
        EXPECT_EQ(observed.fields_[1].name_, ":scheme");
        EXPECT_EQ(observed.fields_[2].name_, ":path");
        EXPECT_EQ(observed.fields_[3].name_, ":authority");
        EXPECT_EQ(observed.fields_[3].value_, "www.example.com");
        EXPECT_EQ(observed.fields_[3].first_, 3u);
        EXPECT_EQ(observed.fields_[3].frame_, 4u);
        EXPECT_EQ(observed.fields_[4].name_, ":authority");
        EXPECT_EQ(observed.fields_[5].name_, ":path");
        EXPECT_EQ(observed.fields_[6].name_, ":scheme");
        EXPECT_EQ(observed.fields_[7].name_, ":method");
        EXPECT_EQ(observed.fields_[7].index_, 3u);
        EXPECT_EQ(observed.begins_, 2u);
        EXPECT_EQ(observed.ends_, 2u);
        EXPECT_FALSE(server.connection_error().has_value());
    }
}

TEST(Ruvia, http2_header_observer_sees_extended_connect_order_before_semantic_rejection) {
    for (const bool malformed : {false, true}) {
        wire_observation_log observed;
        auto server = ruvia::http2_connection::server({.header_observer_ = &wire_observation_log::header, .observer_context_ = &observed});
        std::string block;
        for (const auto& field : std::array{
                 std::pair{":protocol", "websocket"}, std::pair{":path", "/chat"},
                 std::pair{":scheme", "https"}, std::pair{":authority", "example.test:443"},
                 std::pair{":method", "CONNECT"}}) {
            ruvia::hpack_encoder::encode_header(block, field.first, field.second);
        }
        if (malformed) {
            ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
        }
        ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
        auto wire = server_preface();
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        ASSERT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        bool request_received = false;
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                request_received = true;
                const auto order = request->request_view().pseudo_header_order_;
                ASSERT_EQ(order.size(), 5u);
                EXPECT_TRUE(order[0] == ruvia::http2_request_pseudo_header::protocol);
                EXPECT_TRUE(order[4] == ruvia::http2_request_pseudo_header::method);
            }
        }
        EXPECT_EQ(request_received, !malformed);
        ASSERT_EQ(observed.fields_.size(), 6u);
        EXPECT_EQ(observed.fields_[0].name_, ":protocol");
        EXPECT_EQ(observed.fields_[4].name_, ":method");
        EXPECT_EQ(observed.fields_[5].name_, malformed ? ":method" : "sec-websocket-version");
        EXPECT_EQ(observed.fields_[0].first_, 1u);
        EXPECT_EQ(observed.frames_.size(), 0u);
    }
}

TEST(Ruvia, http2_frame_observer_includes_unknown_extensions_without_advertisement_semantics) {
    for (const auto role : {ruvia::http2_role::server, ruvia::http2_role::client}) {
        wire_observation_log observed;
        const ruvia::http2_connection_options options{
            .frame_observer_ = &wire_observation_log::frame, .observer_context_ = &observed};
        auto connection = role == ruvia::http2_role::server
                              ? ruvia::http2_connection::server(options)
                              : ruvia::http2_connection::client(options);
        std::string wire = role == ruvia::http2_role::server
                               ? std::string(ruvia::http2_client_preface)
                               : std::string{};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
        append_frame(wire, static_cast<ruvia::http2_frame_type>(0xee), 0, 0, "unknown");
        // ORIGIN is not enabled, and servers never process ALTSVC. These
        // malformed opaque payloads must remain ignored with observation on.
        append_frame(wire, ruvia::http2_frame_type::origin, 0, 0, "not an origin list");
        if (role == ruvia::http2_role::server) {
            append_frame(wire, ruvia::http2_frame_type::alternative_service, 0, 0, "x");
        }
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(19));
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        EXPECT_FALSE(connection.connection_error().has_value());
        while (auto event = connection.next_event()) {
            EXPECT_EQ(event->origin_advertisement(), nullptr);
            EXPECT_EQ(event->alternative_service_advertisement(), nullptr);
        }
        ASSERT_EQ(observed.frames_.size(), role == ruvia::http2_role::server ? 5u : 4u);
        EXPECT_EQ(static_cast<unsigned>(observed.frames_[1].type_), 0xeeu);
        EXPECT_EQ(observed.frames_.back().observation_sequence_, observed.frames_.size() - 1);
        ASSERT_TRUE(observed.frames_.back().window_increment_);
        EXPECT_EQ(*observed.frames_.back().window_increment_, 19u);
    }
}

TEST(Ruvia, http2_frame_observation_does_not_weaken_mandatory_protocol_rejection) {
    wire_observation_log observed;
    auto server = ruvia::http2_connection::server({.frame_observer_ = &wire_observation_log::frame, .observer_context_ = &observed});
    auto wire = server_preface();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(0));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
    ASSERT_EQ(observed.frames_.size(), 2u);
    ASSERT_TRUE(observed.frames_[1].window_increment_);
    EXPECT_EQ(*observed.frames_[1].window_increment_, 0u);
}

TEST(Ruvia, http2_frame_observer_preserves_enabled_origin_and_alternative_service_decoding) {
    for (const bool observe : {false, true}) {
        for (const auto fragment : {std::size_t{1}, std::size_t{4096}}) {
            wire_observation_log observed;
            auto client = ruvia::http2_connection::client({.receive_origin_advertisements_ = true,
                .frame_observer_ = observe ? &wire_observation_log::frame : nullptr,
                .observer_context_ = &observed});
            const std::array<std::string_view, 1> origins{"https://example.test"};
            auto origin_frame = ruvia::encode_http2_origin_frame(origins);
            auto alternative_frame = ruvia::encode_http2_alternative_service_frame(0, origins[0], "h3=\":443\"");
            ASSERT_TRUE(std::holds_alternative<std::vector<char>>(origin_frame));
            ASSERT_TRUE(std::holds_alternative<std::vector<char>>(alternative_frame));
            const auto& origin_bytes = std::get<std::vector<char>>(origin_frame);
            const auto& alternative_bytes = std::get<std::vector<char>>(alternative_frame);
            std::string wire;
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
            wire.append(origin_bytes.data(), origin_bytes.size());
            append_frame(wire, static_cast<ruvia::http2_frame_type>(0xee), 0, 0, "discard me");
            wire.append(alternative_bytes.data(), alternative_bytes.size());
            std::size_t origin_count = 0;
            std::size_t alternative_count = 0;
            for (std::size_t cursor = 0; cursor < wire.size(); cursor += fragment) {
                ASSERT_TRUE(client.feed(std::string_view(wire).substr(cursor, fragment)) ==
                            ruvia::http2_feed_result::accepted);
                while (auto event = client.next_event()) {
                    if (const auto* origin = event->origin_advertisement()) {
                        ++origin_count;
                        ASSERT_EQ(origin->origins_.size(), 1u);
                        EXPECT_TRUE(origin->origins_[0] == origins[0]);
                    }
                    if (const auto* alternative = event->alternative_service_advertisement()) {
                        ++alternative_count;
                        EXPECT_EQ(alternative->stream_id_, 0u);
                        EXPECT_TRUE(alternative->origin_ == origins[0]);
                        EXPECT_TRUE(alternative->field_value_ == "h3=\":443\"");
                    }
                }
            }
            EXPECT_EQ(origin_count, 1u);
            EXPECT_EQ(alternative_count, 1u);
            EXPECT_FALSE(client.connection_error().has_value());
            ASSERT_EQ(observed.frames_.size(), observe ? 4u : 0u);
            if (observe) {
                EXPECT_TRUE(observed.frames_[1].type_ == ruvia::http2_frame_type::origin);
                EXPECT_EQ(observed.frames_[1].observation_sequence_, 1u);
                EXPECT_TRUE(observed.frames_[3].type_ == ruvia::http2_frame_type::alternative_service);
                EXPECT_EQ(observed.frames_[3].observation_sequence_, 3u);
            }
        }
    }
}

namespace {

void submit_buffered_response(
    ruvia::http2_connection& server, std::uint32_t stream_id, const ruvia::http_response& response) {
    const auto head = server.submit_response_head(
        stream_id, response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(head));
    if (const auto* plan = std::get_if<ruvia::http_buffered_response_write_plan>(&head)) {
        if (plan->send_body()) {
            EXPECT_TRUE(server.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
        }
    }
}
}  // namespace

TEST(Ruvia, http2_push_promise_fragmentation_response_and_detached_metadata) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    relay.transfer(client, server);
    auto request = server.next_event();
    EXPECT_TRUE(request && request->request_head());
    auto request_end = server.next_event();
    EXPECT_TRUE(request_end && request_end->message_end());
    const std::string value(20'000, 'x');
    const std::array fields_value{ruvia::http_header_view{"x-long", value}};
    const auto push = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/asset", .headers_ = fields_value});
    EXPECT_TRUE((push.index() == 0));
    EXPECT_EQ(std::get<0>(push).stream_id(), 2u);
    relay.transfer(server, client);
    auto promise = client.next_event();
    EXPECT_TRUE(promise && promise->push_promise());
    EXPECT_TRUE(promise->push_promise()->request_.path_ == "/asset");
    EXPECT_TRUE(promise->push_promise()->request_.headers_[0].value() == value);
    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    response.body("asset");
    submit_buffered_response(server, 2, response);
    relay.transfer(server, client);
    auto head = client.next_event();
    EXPECT_TRUE(head && head->response_head());
    EXPECT_EQ(head->response_head()->stream_id(), 2u);
    auto data = client.next_event();
    EXPECT_TRUE(data && data->message_body_chunk());
    EXPECT_TRUE(data->message_body_chunk()->bytes() == "asset");
    (void)data->message_body_chunk()->take_credit();
    auto end = client.next_event();
    EXPECT_TRUE(end && end->message_end());
    EXPECT_TRUE(promise->push_promise()->request_.headers_[0].value() == value);
    EXPECT_TRUE(!client.connection_error());
}

TEST(Ruvia, http2_push_metadata_outlives_input_and_connection_for_complete_and_fragmented_blocks) {
    ruvia::test::output_relay relay;
    for (const std::size_t value_bytes : {std::size_t{1000}, std::size_t{20'000}}) {
        {
            std::optional<ruvia::http2_event> promise;
            const std::string value(value_bytes, 'q');
            {
                auto client = ruvia::http2_connection::client({.enable_push_ = true});
                auto server = ruvia::http2_connection::server();
                relay.transfer(client, server);
                relay.transfer(server, client);
                relay.transfer(client, server);
                const auto parent = client.submit_request_head(
                    ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
                EXPECT_TRUE(std::holds_alternative<std::uint32_t>(parent));
                relay.transfer(client, server);
                auto request = server.next_event();
                auto end = server.next_event();
                const std::array fields{ruvia::http_header_view{"x-owned", value}};
                const auto pushed = server.submit_push_request(1,
                    {.authority_ = "example.test", .path_ = "/owned", .headers_ = fields});
                EXPECT_TRUE(pushed.index() == 0);
                std::string wire = ruvia::test::drain_output(server);
                const auto first = ruvia::parse_http2_frame_header(std::span(wire.data(), wire.size()));
                EXPECT_TRUE(first && ((first->flags_ & 4U) != 0) == (value_bytes == 1000));
                EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
                auto received = client.next_event();
                EXPECT_TRUE(received && received->push_promise());
                if (received) {
                    promise.emplace(std::move(*received));
                }
                wire.assign(wire.size(), '\0');
            }
            EXPECT_TRUE(promise && promise->push_promise());
            if (promise && promise->push_promise()) {
                const auto& request = promise->push_promise()->request_;
                EXPECT_EQ(request.method_, "GET");
                EXPECT_EQ(request.scheme_, "https");
                EXPECT_EQ(request.authority_, "example.test");
                EXPECT_EQ(request.path_, "/owned");
                EXPECT_TRUE(request.headers_.size() == 1 &&
                            request.headers_[0].name() == "x-owned" && request.headers_[0].value() == value);
            }
        }
    }
}

TEST(Ruvia, http2_parent_reset_cancels_fragmented_push_and_keeps_hpack_state) {
    ruvia::test::output_relay relay;
    // Include an empty final CONTINUATION and cuts inside the indexed field.
    const std::string block(
        "\x82\x87\x01\x01"
        "a"
        "\x84\x40\x01"
        "x"
        "\x01"
        "y",
        11);
    for (const std::size_t split : {std::size_t{3}, std::size_t{8}, block.size()}) {
        auto client = ruvia::http2_connection::client({.enable_push_ = true});
        auto server = ruvia::http2_connection::server();
        relay.transfer(client, server);
        relay.transfer(server, client);
        relay.transfer(client, server);
        const auto parent = client.submit_request_head(
            ruvia::http2_regular_request_head_view{.authority_ = "a"});
        const auto other = client.submit_request_head(
            ruvia::http2_regular_request_head_view{.authority_ = "a"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(parent) && std::holds_alternative<std::uint32_t>(other));
        if (!std::holds_alternative<std::uint32_t>(parent) || !std::holds_alternative<std::uint32_t>(other)) {
            continue;
        }
        const auto parent_id = std::get<std::uint32_t>(parent);
        const auto other_id = std::get<std::uint32_t>(other);
        (void)ruvia::test::drain_output(client);
        const auto make_frame = [&](ruvia::http2_frame_type type, std::uint8_t flags,
                                    std::uint32_t stream_id, std::string_view payload) {
            std::string frame(ruvia::http2_frame_header_bytes, '\0');
            EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(frame.data(), frame.size()),
                static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
            frame.append(payload);
            return frame;
        };
        std::string payload("\0\0\0\2", 4);
        payload.append(block.data(), split);
        const auto promise_start = make_frame(ruvia::http2_frame_type::push_promise, 0, parent_id, payload);
        EXPECT_TRUE(client.feed(promise_start) == ruvia::http2_feed_result::accepted);
        EXPECT_TRUE(client.header_block_in_progress());
        EXPECT_TRUE(client.submit_reset(parent_id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        // A PUSH_PROMISE whose associated stream closed before its block ended
        // is discarded, so the stream it reserved is cancelled with the parent.
        const auto reset_wire = ruvia::test::drain_output(client);
        EXPECT_EQ(reset_wire.size(), 2 * (ruvia::http2_frame_header_bytes + 4));
        const auto reset = ruvia::parse_http2_frame_header(std::span<const char>(reset_wire.data(), reset_wire.size()));
        EXPECT_TRUE(reset && reset->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream) &&
                    reset->stream_id_ == parent_id);
        if (reset_wire.size() == 2 * (ruvia::http2_frame_header_bytes + 4)) {
            const auto cancel = std::string_view(reset_wire).substr(ruvia::http2_frame_header_bytes + 4);
            const auto promised = ruvia::parse_http2_frame_header(std::span<const char>(cancel.data(), cancel.size()));
            EXPECT_TRUE(promised && promised->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream) &&
                        promised->stream_id_ == 2u);
            EXPECT_EQ(static_cast<unsigned char>(cancel.back()), static_cast<unsigned char>(ruvia::http2_error_code::cancel));
        }
        EXPECT_TRUE(!client.wants_write());
        const auto promise_end = make_frame(ruvia::http2_frame_type::continuation, 4, parent_id,
            std::string_view(block).substr(split));
        EXPECT_TRUE(client.feed(promise_end) == ruvia::http2_feed_result::accepted);
        EXPECT_TRUE(!client.header_block_in_progress());
        EXPECT_TRUE(!client.next_event());
        const std::string pushed_block(1, static_cast<char>(0x88));
        const auto pushed_response = make_frame(ruvia::http2_frame_type::headers, 5, 2, pushed_block);
        EXPECT_TRUE(client.feed(pushed_response) == ruvia::http2_feed_result::accepted);
        EXPECT_TRUE(!client.next_event());
        // Index 62 was introduced by the push, despite cancellation of its parent.
        const std::string indexed_block("\x88\xbe", 2);
        const auto other_response = make_frame(ruvia::http2_frame_type::headers, 5, other_id, indexed_block);
        EXPECT_TRUE(client.feed(other_response) == ruvia::http2_feed_result::accepted);
        auto other_head = client.next_event();
        EXPECT_TRUE(other_head && other_head->response_head() && other_head->response_head()->stream_id() == other_id);
        if (other_head && other_head->response_head()) {
            const auto fields = other_head->response_head()->head().headers();
            EXPECT_TRUE(fields.size() == 1 && fields[0].name() == "x" && fields[0].value() == "y");
        }
        auto other_end = client.next_event();
        EXPECT_TRUE(other_end && other_end->message_end());
        EXPECT_TRUE(!client.connection_error());
        EXPECT_TRUE(!client.wants_write());
    }
}

TEST(Ruvia, http2_push_disabled_peer_and_client_cancellation) {
    ruvia::test::output_relay relay;
    for (const bool enabled : {false, true}) {
        auto client = ruvia::http2_connection::client({.enable_push_ = enabled});
        auto server = ruvia::http2_connection::server({});
        relay.transfer(client, server);
        relay.transfer(server, client);
        const auto submitted = client.submit_request_head(
            ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        relay.transfer(client, server);
        auto request = server.next_event();
        auto end = server.next_event();
        const auto push = server.submit_push_request(1, {.authority_ = "example.test"});
        if (!enabled) {
            EXPECT_TRUE((push.index() != 0) && std::get<1>(push) == ruvia::http2_push_submit_error::push_disabled);
            continue;
        }
        EXPECT_TRUE((push.index() == 0));
        relay.transfer(server, client);
        EXPECT_TRUE(client.next_event()->push_promise());
        EXPECT_TRUE(client.submit_reset(std::get<0>(push).stream_id(), ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        relay.transfer(client, server);
        auto closed = server.next_event();
        EXPECT_TRUE(closed && closed->stream_closed());
        EXPECT_EQ(closed->stream_closed()->stream_id(), std::get<0>(push).stream_id());
    }
}

TEST(Ruvia, http2_push_promise_unsafe_method_resets_promised_stream_without_goaway) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    relay.transfer(client, server);

    // Prepare HPACK block with unsafe method (POST)
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/upload");

    std::string frame;
    frame.resize(ruvia::http2_frame_header_bytes + 4 + block.size());
    const auto payload_len = static_cast<std::uint32_t>(4 + block.size());
    EXPECT_TRUE(ruvia::encode_http2_frame_header(
        std::span(frame.data(), ruvia::http2_frame_header_bytes),
        payload_len,
        ruvia::http2_frame_type::push_promise,
        0x4 /* END_HEADERS */,
        1 /* associated_stream_id */));

    // Promised stream ID = 2
    frame[ruvia::http2_frame_header_bytes + 0] = 0x00;
    frame[ruvia::http2_frame_header_bytes + 1] = 0x00;
    frame[ruvia::http2_frame_header_bytes + 2] = 0x00;
    frame[ruvia::http2_frame_header_bytes + 3] = 0x02;
    std::memcpy(frame.data() + ruvia::http2_frame_header_bytes + 4, block.data(), block.size());

    // Feed to client
    const auto feed_status = client.feed(frame);
    EXPECT_TRUE(feed_status == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.connection_error());

    // Client should have emitted RST_STREAM on promised stream 2
    const auto pending = ruvia::test::drain_output(client);
    EXPECT_TRUE(pending.size() >= ruvia::http2_frame_header_bytes + 4);
    const auto parsed_value = ruvia::parse_http2_frame_header(std::span(pending.data(), pending.size()));
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_EQ(parsed_value->type_, static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
    EXPECT_EQ(parsed_value->stream_id_, 2u);
    EXPECT_EQ(parsed_value->length_, 4u);
}

TEST(Ruvia, http2_push_reservation_does_not_consume_peer_concurrency_until_response_headers) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    relay.transfer(client, server);
    auto head = server.next_event();
    auto end = server.next_event();
    const auto set_limit = [&](std::uint32_t limit) {
        std::array<char, 15> frame{};
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(frame).first(9), 6, ruvia::http2_frame_type::settings, 0, 0));
        frame[10] = 3;
        for (std::size_t i = 0; i < 4; ++i) {
            frame[11 + i] = static_cast<char>(limit >> (24 - i * 8));
        }
        EXPECT_TRUE(server.feed(std::string_view(frame.data(), frame.size())) == ruvia::http2_feed_result::accepted);
        (void)ruvia::test::drain_output(server);
    };
    // MCS = 0 admits no pushed stream at all, so no promise is made (RFC 9113 8.4).
    set_limit(0);
    const auto refused = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/zero"});
    EXPECT_TRUE(refused.index() == 1 && std::get<1>(refused) == ruvia::http2_push_submit_error::stream_limit);
    set_limit(1);
    const auto first = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/one"});
    const auto second = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/two"});
    EXPECT_TRUE((first.index() == 0) && (second.index() == 0));
    if ((first.index() != 0) || (second.index() != 0)) {
        return;
    }
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(server.submit_streaming_response_head(std::get<0>(first).stream_id(), ruvia::http_response{})));
    const auto second_blocked = server.submit_streaming_response_head(std::get<0>(second).stream_id(), ruvia::http_response{});
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(second_blocked) &&
                std::get<ruvia::http2_response_head_submit_error>(second_blocked) ==
                    ruvia::http2_response_head_submit_error::peer_stream_limit_reached);
    EXPECT_TRUE(server.submit_data(std::get<0>(first).stream_id(), {}, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(server.submit_streaming_response_head(std::get<0>(second).stream_id(), ruvia::http_response{})));
}

TEST(Ruvia, http2_push_response_events_survive_a_reset_in_the_same_input_batch) {
    ruvia::test::output_relay relay;
    {
        auto client = ruvia::http2_connection::client({.enable_push_ = true});
        auto server = ruvia::http2_connection::server({});
        relay.transfer(client, server);
        relay.transfer(server, client);
        relay.transfer(client, server);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        relay.transfer(client, server);
        auto request = server.next_event();
        auto request_end = server.next_event();
        const auto push = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/reset"});
        EXPECT_TRUE(push.index() == 0);
        if (push.index() != 0) {
            return;
        }
        const auto id = std::get<0>(push).stream_id();
        EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(server.submit_streaming_response_head(id, ruvia::http_response{})));
        EXPECT_TRUE(server.submit_reset(id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        relay.transfer(server, client);
        auto promise = client.next_event();
        auto head = client.next_event();
        auto closed = client.next_event();
        EXPECT_TRUE(promise && promise->push_promise());
        EXPECT_TRUE(head && head->response_head() && head->response_head()->stream_id() == id);
        EXPECT_TRUE(closed && closed->stream_closed() && closed->stream_closed()->stream_id() == id);
        EXPECT_TRUE(!client.next_event());
        if (promise && promise->push_promise()) {
            EXPECT_EQ(promise->push_promise()->request_.path_, "/reset");
        }
        ruvia::http_response response;
        response.body("parent");
        submit_buffered_response(server, 1, response);
        relay.transfer(server, client);
        auto parent_head = client.next_event();
        auto body = client.next_event();
        EXPECT_TRUE(parent_head && parent_head->response_head() && parent_head->response_head()->stream_id() == 1);
        EXPECT_TRUE(body && body->message_body_chunk() && body->message_body_chunk()->bytes() == "parent");
        if (body && body->message_body_chunk()) {
            (void)body->message_body_chunk()->take_credit();
        }
        auto end = client.next_event();
        EXPECT_TRUE(end && end->message_end());
        EXPECT_TRUE(!client.connection_error());
        EXPECT_TRUE(!server.connection_error());
        EXPECT_TRUE(request && request->request_head());
        if (request && request->request_head()) {
            EXPECT_TRUE(server.release(std::move(*request->request_head())) == ruvia::http2_server_request_release_status::released);
        }
    }
}

TEST(Ruvia, http2_push_finish_releases_peer_concurrency_while_its_request_lease_is_retained) {
    ruvia::test::output_relay relay;
    for (const bool with_trailers : {false, true}) {
        auto client = ruvia::http2_connection::client({.enable_push_ = true});
        auto server = ruvia::http2_connection::server();
        relay.transfer(client, server);
        relay.transfer(server, client);
        relay.transfer(client, server);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        relay.transfer(client, server);
        auto request = server.next_event();
        auto request_end = server.next_event();
        std::array<char, 15> settings{};
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(settings).first(9), 6, ruvia::http2_frame_type::settings, 0, 0));
        settings[10] = 3;
        settings[14] = 1;
        EXPECT_TRUE(server.feed(std::string_view(settings.data(), settings.size())) == ruvia::http2_feed_result::accepted);
        auto first = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/one"});
        const auto second = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/two"});
        EXPECT_TRUE(first.index() == 0 && second.index() == 0);
        if (first.index() != 0 || second.index() != 0) {
            continue;
        }
        const auto id = std::get<0>(first).stream_id();
        const auto committed = server.submit_streaming_response_head(id, ruvia::http_response{},
            with_trailers ? ruvia::http_response_trailer_intent::present : ruvia::http_response_trailer_intent::none);
        EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
        const auto second_blocked = server.submit_streaming_response_head(std::get<0>(second).stream_id(), ruvia::http_response{});
        EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(second_blocked) &&
                    std::get<ruvia::http2_response_head_submit_error>(second_blocked) ==
                        ruvia::http2_response_head_submit_error::peer_stream_limit_reached);
        const std::array fields_value{ruvia::http_header_view{"x-final", "done"}};
        const auto trailers = ruvia::validate_http_response_trailers(
            with_trailers ? std::span<const ruvia::http_header_view>(fields_value) : std::span<const ruvia::http_header_view>{});
        EXPECT_TRUE(server.finish_response(id, trailers) == ruvia::http2_finish_status::accepted);
        EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(server.submit_streaming_response_head(std::get<0>(second).stream_id(), ruvia::http_response{})));
        EXPECT_EQ(std::get<0>(first).request_view().path_, "/one");
        EXPECT_TRUE(server.release(std::move(std::get<0>(first))) == ruvia::http2_server_request_release_status::released);
    }
}

// RFC 9113 section 8.4: SETTINGS_MAX_CONCURRENT_STREAMS = 0 prevents every pushed
// response, so no promise is made and nothing reaches the wire.
TEST(Ruvia, http2_push_refused_up_front_when_peer_concurrency_admits_no_pushed_stream) {
    ruvia::test::output_relay relay;
    for (const std::uint8_t limit : {std::uint8_t{0}, std::uint8_t{1}}) {
        auto client = ruvia::http2_connection::client({.enable_push_ = true});
        auto server = ruvia::http2_connection::server();
        relay.transfer(client, server);
        relay.transfer(server, client);
        relay.transfer(client, server);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        relay.transfer(client, server);
        auto request = server.next_event();
        auto request_end = server.next_event();
        std::array<char, 15> settings{};
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(settings).first(9), 6, ruvia::http2_frame_type::settings, 0, 0));
        settings[10] = 3;
        settings[14] = static_cast<char>(limit);
        EXPECT_TRUE(server.feed(std::string_view(settings.data(), settings.size())) == ruvia::http2_feed_result::accepted);
        (void)ruvia::test::drain_output(server);
        std::optional<ruvia::http2_request_head_event> opened;
        if (limit != 0) {
            auto first = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/one"});
            EXPECT_TRUE(first.index() == 0);
            if (first.index() != 0) {
                continue;
            }
            const auto committed =
                server.submit_streaming_response_head(std::get<0>(first).stream_id(), ruvia::http_response{});
            EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
            opened.emplace(std::move(std::get<0>(first)));
            (void)ruvia::test::drain_output(server);
        }
        const auto refused = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/two"});
        EXPECT_TRUE(refused.index() == 1 && std::get<1>(refused) == ruvia::http2_push_submit_error::stream_limit);
        EXPECT_TRUE(ruvia::test::drain_output(server).empty());
        EXPECT_TRUE(!server.connection_error().has_value());
        if (opened) {
            (void)server.release(std::move(*opened));
        }
    }
}

TEST(Ruvia, http2_server_goaway_releases_unprocessed_local_push_streams) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    relay.transfer(client, server);
    auto head = server.next_event();
    auto end = server.next_event();
    const auto push = server.submit_push_request(1, {.authority_ = "example.test"});
    EXPECT_TRUE((push.index() == 0));
    const std::array<char, 17> goaway{0, 0, 8, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(server.feed(std::string_view(goaway.data(), goaway.size())) == ruvia::http2_feed_result::accepted);
    auto draining = server.next_event();
    auto closed = server.next_event();
    EXPECT_TRUE(draining && draining->goaway());
    EXPECT_TRUE(closed && closed->stream_closed() && closed->stream_closed()->stream_id() == std::get<0>(push).stream_id());
    const auto closed_head = server.submit_streaming_response_head(std::get<0>(push).stream_id(), ruvia::http_response{});
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_head) &&
                std::get<ruvia::http2_response_head_submit_error>(closed_head) ==
                    ruvia::http2_response_head_submit_error::closed);
}

TEST(Ruvia, http2_push_repeated_response_and_cancellation_preserve_promises) {
    ruvia::test::output_relay relay;
    {
        auto client = ruvia::http2_connection::client({.enable_push_ = true});
        auto server = ruvia::http2_connection::server({});
        relay.transfer(client, server);
        relay.transfer(server, client);
        relay.transfer(client, server);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        relay.transfer(client, server);
        auto request = server.next_event();
        auto request_end = server.next_event();
        const auto first = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/retained"});
        EXPECT_TRUE(first.index() == 0);
        relay.transfer(server, client);
        auto retained = client.next_event();
        EXPECT_TRUE(retained && retained->push_promise());
        EXPECT_TRUE(client.submit_reset(std::get<0>(first).stream_id(), ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        relay.transfer(client, server);
        auto closed = server.next_event();
        closed.reset();
        for (std::size_t i = 0; i < 32; ++i) {
            {
                const auto push = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/asset"});
                EXPECT_TRUE(push.index() == 0);
                relay.transfer(server, client);
                auto promise = client.next_event();
                EXPECT_TRUE(promise && promise->push_promise());
                if (i % 2 == 0) {
                    EXPECT_TRUE(client.submit_reset(std::get<0>(push).stream_id(), ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
                    relay.transfer(client, server);
                    auto canceled = server.next_event();
                    EXPECT_TRUE(canceled && canceled->stream_closed());
                } else {
                    ruvia::http_response response;
                    response.body("asset");
                    submit_buffered_response(server, std::get<0>(push).stream_id(), response);
                    relay.transfer(server, client);
                    auto head = client.next_event();
                    auto data = client.next_event();
                    EXPECT_TRUE(data && data->message_body_chunk());
                    if (data && data->message_body_chunk()) {
                        (void)data->message_body_chunk()->take_credit();
                    }
                    auto end = client.next_event();
                    EXPECT_TRUE(head && head->response_head());
                    EXPECT_TRUE(end && end->message_end());
                }
            }
            EXPECT_TRUE(!client.next_event());
            EXPECT_TRUE(!server.next_event());

            EXPECT_TRUE(retained->push_promise()->request_.path_ == "/retained");
        }
        std::string request_path(16 * 1024, 'p');
        request_path.front() = '/';
        EXPECT_TRUE(!server.wants_write());
        const auto retry = server.submit_push_request(1, {.authority_ = "example.test", .path_ = request_path});
        EXPECT_TRUE(retry.index() == 0);
        relay.transfer(server, client);
        auto retried_promise = client.next_event();
        EXPECT_TRUE(retried_promise && retried_promise->push_promise());
        if (retried_promise && retried_promise->push_promise()) {
            const auto& retry_path = retried_promise->push_promise()->request_.path_;
            EXPECT_TRUE(std::string_view(retry_path.data(), retry_path.size()) == request_path);
        }
    }
}

TEST(Ruvia, http2_push_rejects_incomplete_parent_and_discards_promises_after_goaway) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    // 1. Server rejects submit_push_request while parent HEADERS is awaiting CONTINUATION.
    std::string req_block;
    ruvia::hpack_encoder::encode_header(req_block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(req_block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(req_block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(req_block, ":path", "/incomplete");
    std::string partial_headers(ruvia::http2_frame_header_bytes, '\0');
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(partial_headers).first(9),
        static_cast<std::uint32_t>(req_block.size()), ruvia::http2_frame_type::headers, 0x1 /* END_STREAM only */, 1));
    partial_headers.append(req_block);
    EXPECT_TRUE(server.feed(partial_headers) == ruvia::http2_feed_result::accepted);
    const auto push_incomplete = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/a"});
    EXPECT_TRUE(push_incomplete.index() == 1 &&
                std::get<1>(push_incomplete) == ruvia::http2_push_submit_error::invalid_state);
    std::array<char, 9> empty_cont{};
    EXPECT_TRUE(ruvia::encode_http2_frame_header(empty_cont, 0, ruvia::http2_frame_type::continuation, 0x4, 1));
    EXPECT_TRUE(server.feed(std::string_view(empty_cont.data(), empty_cont.size())) == ruvia::http2_feed_result::accepted);
    (void)server.next_event();
    (void)server.next_event();

    // 2. The server declines to push on a CONNECT stream, but the client must
    // accept a peer PUSH_PROMISE while the CONNECT response is still pending
    // (RFC 9113 section 6.6 allows any open peer-initiated stream).
    auto connect_client = ruvia::http2_connection::client({.enable_push_ = true});
    auto connect_server = ruvia::http2_connection::server({});
    relay.transfer(connect_client, connect_server);
    relay.transfer(connect_server, connect_client);
    relay.transfer(connect_client, connect_server);
    const auto connect_id = connect_client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect_id));
    relay.transfer(connect_client, connect_server);
    auto connect_req = connect_server.next_event();
    EXPECT_TRUE(connect_req && connect_req->request_head());
    const auto push_on_connect = connect_server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/a"});
    EXPECT_TRUE(push_on_connect.index() == 1 &&
                std::get<1>(push_on_connect) == ruvia::http2_push_submit_error::invalid_state);
    if (connect_req && connect_req->request_head()) {
        (void)connect_server.release(std::move(*connect_req->request_head()));
    }

    std::string push_block;
    ruvia::hpack_encoder::encode_header(push_block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(push_block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(push_block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(push_block, ":path", "/pushed");
    std::string bad_promise(ruvia::http2_frame_header_bytes + 4, '\0');
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(bad_promise).first(9),
        static_cast<std::uint32_t>(4 + push_block.size()), ruvia::http2_frame_type::push_promise, 0x4, 1));
    bad_promise[12] = 2;
    bad_promise.append(push_block);
    EXPECT_TRUE(connect_client.feed(bad_promise) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!connect_client.connection_error());
    const auto pending_connect_push = connect_client.next_event();
    EXPECT_TRUE(pending_connect_push && pending_connect_push->push_promise());

    // 3. A client that sent GOAWAY discards a later PUSH_PROMISE unanswered (RFC
    //    9113 §6.8: the GOAWAY already refuses it) while still updating HPACK.
    auto drain_client = ruvia::http2_connection::client({.enable_push_ = true});
    auto drain_server = ruvia::http2_connection::server({});
    relay.transfer(drain_client, drain_server);
    relay.transfer(drain_server, drain_client);
    relay.transfer(drain_client, drain_server);
    const auto parent_id = drain_client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(parent_id));
    relay.transfer(drain_client, drain_server);
    drain_client.begin_drain();
    (void)ruvia::test::drain_output(drain_client);

    std::string dyn_push_block;
    ruvia::hpack_encoder::encode_header(dyn_push_block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(dyn_push_block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(dyn_push_block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(dyn_push_block, ":path", "/refused");
    // Literal Header Field with Incremental Indexing (0x40): "x-dyn" -> "kept"
    dyn_push_block.append("\x40\x05x-dyn\x04kept", 12);
    std::string drain_promise(ruvia::http2_frame_header_bytes + 4, '\0');
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(drain_promise).first(9),
        static_cast<std::uint32_t>(4 + dyn_push_block.size()), ruvia::http2_frame_type::push_promise, 0x4, 1));
    drain_promise[12] = 2;
    drain_promise.append(dyn_push_block);
    EXPECT_TRUE(drain_client.feed(drain_promise) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!drain_client.next_event());
    EXPECT_TRUE(ruvia::test::drain_output(drain_client).empty());

    // Response on stream 1 referencing dynamic table index 62 (0xbe) decodes "x-dyn: kept".
    std::string resp_block("\x88\xbe", 2);
    std::string resp_frame(ruvia::http2_frame_header_bytes, '\0');
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(resp_frame).first(9),
        static_cast<std::uint32_t>(resp_block.size()), ruvia::http2_frame_type::headers, 0x5, 1));
    resp_frame.append(resp_block);
    EXPECT_TRUE(drain_client.feed(resp_frame) == ruvia::http2_feed_result::accepted);
    const auto resp_head = drain_client.next_event();
    EXPECT_TRUE(resp_head && resp_head->response_head());
    if (resp_head && resp_head->response_head()) {
        const auto headers = resp_head->response_head()->head().headers();
        EXPECT_EQ(headers.size(), std::size_t{1});
        if (!headers.empty()) {
            EXPECT_EQ(headers[0].name(), "x-dyn");
            EXPECT_EQ(headers[0].value(), "kept");
        }
    }
}

TEST(Ruvia, http2_server_goaway_ignores_completed_push_stream_retained_by_lease) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    relay.transfer(client, server);
    auto req_head = server.next_event();
    auto req_end = server.next_event();

    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/completed"});
    EXPECT_TRUE(pushed.index() == 0);
    if (pushed.index() != 0) {
        return;
    }
    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    response.body("done");
    submit_buffered_response(server, std::get<0>(pushed).stream_id(), response);

    const std::array<char, 17> goaway{0, 0, 8, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(server.feed(std::string_view(goaway.data(), goaway.size())) == ruvia::http2_feed_result::accepted);
    const auto draining = server.next_event();
    EXPECT_TRUE(draining && draining->goaway());
    EXPECT_TRUE(!server.next_event());
    EXPECT_TRUE(server.release(std::move(std::get<0>(pushed))) == ruvia::http2_server_request_release_status::released);
    if (req_head && req_head->request_head()) {
        EXPECT_TRUE(server.release(std::move(*req_head->request_head())) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_reserved_push_does_not_crowd_out_max_concurrent_client_streams) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    std::vector<ruvia::http2_request_head_event> held_heads;
    const auto first = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(first));
    relay.transfer(client, server);
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            held_heads.push_back(std::move(*rh));
        }
    }
    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/pushed"});
    EXPECT_TRUE(pushed.index() == 0);
    (void)ruvia::test::drain_output(server);

    for (int i = 0; i < 127; ++i) {
        const auto sub = client.submit_request_head(
            ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(sub));
        relay.transfer(client, server);
        while (auto ev = server.next_event()) {
            if (auto* rh = ev->request_head()) {
                held_heads.push_back(std::move(*rh));
            }
        }
    }
    EXPECT_EQ(held_heads.size(), std::size_t{128});
    EXPECT_TRUE(ruvia::test::drain_output(server).empty());
    (void)server.release(std::move(std::get<0>(pushed)));
    for (auto& head : held_heads) {
        (void)server.release(std::move(head));
    }
}

TEST(Ruvia, http2_goaway_closes_more_reserved_pushes_than_the_concurrency_limit) {
    // Reserved push promises do not occupy a concurrency slot, so a GOAWAY
    // boundary can cover more live streams than SETTINGS_MAX_CONCURRENT_STREAMS.
    // Closing them must stay inside the stream table's capacity.
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto parent = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(parent));
    relay.transfer(client, server);

    std::vector<ruvia::http2_request_head_event> held_heads;
    while (auto event_value = server.next_event()) {
        if (auto* head = event_value->request_head()) {
            held_heads.push_back(std::move(*head));
        }
    }
    EXPECT_EQ(held_heads.size(), std::size_t{1});

    constexpr std::size_t push_count = 300;
    for (std::size_t index = 0; index < push_count; ++index) {
        auto pushed = server.submit_push_request(
            1, {.authority_ = "example.test", .path_ = "/asset"});
        EXPECT_TRUE(pushed.index() == 0);
        if (pushed.index() != 0) {
            return;
        }
        held_heads.push_back(std::move(std::get<0>(pushed)));
        (void)ruvia::test::drain_output(server);
    }

    const std::array<char, 17> goaway{0, 0, 8, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(server.feed(std::string_view(goaway.data(), goaway.size())) ==
                ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error());
    const auto draining = server.next_event();
    EXPECT_TRUE(draining && draining->goaway());
    std::size_t closed_count = 0;
    while (const auto event_value = server.next_event()) {
        EXPECT_TRUE(event_value->stream_closed() != nullptr);
        if (event_value->stream_closed() != nullptr) {
            EXPECT_EQ(event_value->stream_closed()->stream_id(), 2U + 2U * closed_count);
            ++closed_count;
        }
    }
    EXPECT_EQ(closed_count, push_count);
    for (auto& head : held_heads) {
        (void)server.release(std::move(head));
    }
}

TEST(Ruvia, http2_push_promise_on_connected_tunnel_is_a_stream_error) {
    // RFC 9113 section 8.5: once CONNECT is established, any frame other than
    // DATA/RST_STREAM/WINDOW_UPDATE/PRIORITY is a stream error, not a
    // connection error. The promised stream is cancelled as well.
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto connect_id = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect_id));
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> connect_head;
    while (auto event_value = server.next_event()) {
        if (auto* head = event_value->request_head()) {
            connect_head.emplace(std::move(*head));
        }
    }
    EXPECT_TRUE(connect_head.has_value());
    if (!connect_head) {
        return;
    }
    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(1, response) ==
                ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    while (client.next_event()) {
    }
    EXPECT_TRUE(!client.connection_error());

    std::string push_block;
    ruvia::hpack_encoder::encode_header(push_block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(push_block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(push_block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(push_block, ":path", "/pushed");
    std::string promise(ruvia::http2_frame_header_bytes + 4, '\0');
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(promise).first(9),
        static_cast<std::uint32_t>(4 + push_block.size()), ruvia::http2_frame_type::push_promise, 0x4, 1));
    promise[12] = 2;
    promise.append(push_block);
    EXPECT_TRUE(client.feed(promise) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.connection_error());

    const auto closed = client.next_event();
    EXPECT_TRUE(closed && closed->stream_closed() && closed->stream_closed()->stream_id() == 1);
    EXPECT_TRUE(!client.next_event());
    const auto output = ruvia::test::drain_output(client);
    EXPECT_EQ(output.size(), 2 * (ruvia::http2_frame_header_bytes + 4));
    if (output.size() == 2 * (ruvia::http2_frame_header_bytes + 4)) {
        const auto associated = ruvia::parse_http2_frame_header(std::span(output).first(9));
        EXPECT_TRUE(associated && associated->type_ ==
                                      static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        EXPECT_EQ(associated->stream_id_, 1u);
        EXPECT_EQ(static_cast<unsigned char>(output[12]), static_cast<unsigned char>(ruvia::http2_error_code::protocol_error));
        const auto promised = ruvia::parse_http2_frame_header(
            std::span(output).subspan(ruvia::http2_frame_header_bytes + 4).first(9));
        EXPECT_TRUE(promised && promised->type_ ==
                                    static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        EXPECT_EQ(promised->stream_id_, 2u);
        EXPECT_EQ(static_cast<unsigned char>(output.back()), static_cast<unsigned char>(ruvia::http2_error_code::cancel));
    }
    (void)server.release(std::move(*connect_head));
}

// A5H2-01: Releasing >128 push stream leases while GOAWAY stream_closed events
// are still queued in events_ defers up to http2_stream_table::max_streams
// releases without calling std::terminate().
TEST(Ruvia, http2_push_releasing_more_than_max_concurrent_streams_leases_before_draining_events_succeeds) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto parent = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(parent));
    relay.transfer(client, server);
    std::vector<ruvia::http2_request_head_event> held_heads;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            held_heads.push_back(std::move(*rh));
        }
    }
    for (std::size_t i = 0; i < 130; ++i) {
        auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/asset"});
        EXPECT_TRUE(pushed.index() == 0);
        if (pushed.index() == 0) {
            held_heads.push_back(std::move(std::get<0>(pushed)));
        }
        (void)ruvia::test::drain_output(server);
    }
    const std::array<char, 17> goaway{0, 0, 8, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(server.feed(std::string_view(goaway.data(), goaway.size())) ==
                ruvia::http2_feed_result::accepted);
    // Release all 131 held leases BEFORE draining server.next_event()!
    held_heads.clear();
    std::size_t closed_events = 0;
    while (auto ev = server.next_event()) {
        if (ev->stream_closed()) {
            ++closed_events;
        }
    }
    EXPECT_EQ(closed_events, std::size_t{130});
}

// A5H2-07: Remote PUSH_PROMISE reservations are capped below
// http2_stream_table::max_streams so max_concurrent_streams (128) slots remain
// available for locally initiated client requests.
TEST(Ruvia, http2_push_remote_promise_flood_does_not_starve_client_request_submission) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto sub1 = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(sub1));
    const auto parent_id = std::get<std::uint32_t>(sub1);
    (void)ruvia::test::drain_output(client);

    std::string pblock;
    ruvia::hpack_encoder::encode_header(pblock, ":method", "GET");
    ruvia::hpack_encoder::encode_header(pblock, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(pblock, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(pblock, ":path", "/p");
    for (std::uint32_t i = 1; i <= 511; ++i) {
        const std::uint32_t pid = i * 2;
        std::string payload(4, '\0');
        payload[0] = static_cast<char>((pid >> 24) & 0x7f);
        payload[1] = static_cast<char>((pid >> 16) & 0xff);
        payload[2] = static_cast<char>((pid >> 8) & 0xff);
        payload[3] = static_cast<char>(pid & 0xff);
        payload.append(pblock);
        std::string wire(ruvia::http2_frame_header_bytes, '\0');
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(wire.data(), wire.size()),
            static_cast<std::uint32_t>(payload.size()), ruvia::http2_frame_type::push_promise, 0x4, parent_id));
        wire.append(payload);
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        while (client.next_event()) {
        }
    }

    const auto sub2 = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(sub2));
}

TEST(Ruvia, http2_push_request_view_accepts_named_owners_and_copies_before_owner_mutation) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(
        client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"})));
    relay.transfer(client, server);
    auto request_event = server.next_event();
    EXPECT_TRUE(request_event && request_event->request_head());
    auto request_end = server.next_event();
    EXPECT_TRUE(request_end && request_end->message_end());
    std::string method("GET");
    std::string scheme("https");
    std::string authority("example.test");
    std::string path("/owned-asset");
    const ruvia::http_push_request_view request{
        .method_ = method,
        .scheme_ = scheme,
        .authority_ = authority,
        .path_ = path,
    };
    const auto push = server.submit_push_request(1, request);
    EXPECT_TRUE(push.index() == 0);
    method.assign("PUT");
    scheme.assign("http");
    authority.assign("other.test");
    path.assign("/mutated");
    if (push.index() == 0) {
        EXPECT_EQ(std::get<0>(push).request_view().path_, std::string_view("/owned-asset"));
    }
    relay.transfer(server, client);
    const auto promise = client.next_event();
    EXPECT_TRUE(promise && promise->push_promise());
    if (promise && promise->push_promise()) {
        EXPECT_EQ(promise->push_promise()->request_.path_, std::string_view("/owned-asset"));
        EXPECT_EQ(promise->push_promise()->request_.authority_, std::string_view("example.test"));
    }
}

TEST(Ruvia, http2_connection_origin_and_alternative_service_events_are_owned_and_role_checked) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.receive_origin_advertisements_ = true});
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    const std::array<std::string_view, 1> origins{"https://example.test"};
    EXPECT_TRUE(server.submit_origin_advertisement(origins) == ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(server.submit_alternative_service_advertisement(0, origins[0], "h3=\":443\"") == ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    auto origin = client.next_event();
    auto alt = client.next_event();
    EXPECT_TRUE(origin && origin->origin_advertisement() && origin->origin_advertisement()->origins_[0] == origins[0]);
    EXPECT_TRUE(alt && alt->alternative_service_advertisement() && alt->alternative_service_advertisement()->field_value_ == "h3=\":443\"");
    EXPECT_TRUE(client.submit_origin_advertisement(origins) == ruvia::http2_submit_status::invalid_state);
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_TRUE(server.submit_alternative_service_advertisement(0, origins[0], "clear") == ruvia::http2_submit_status::accepted);
        relay.transfer(server, client);
        EXPECT_TRUE(client.next_event()->alternative_service_advertisement());
    }
    EXPECT_TRUE(alt->alternative_service_advertisement()->field_value_ == "h3=\":443\"");
}
TEST(Ruvia, http2_connection_ignores_origin_without_authenticated_origin_context) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    EXPECT_TRUE(server.submit_origin_advertisement(std::array<std::string_view, 1>{"https://example.test"}) == ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    EXPECT_TRUE(!client.next_event());
}

TEST(Ruvia, http2_connection_server_rejects_client_only_options) {
    const auto rejected = [](ruvia::http2_connection_options options) {
        try {
            static_cast<void>(ruvia::http2_connection::server(options));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    EXPECT_TRUE(rejected({.enable_push_ = true}));
    EXPECT_TRUE(rejected({.receive_origin_advertisements_ = true}));
    EXPECT_TRUE(!rejected({}));
}

TEST(Ruvia, http2_connection_advertisement_validates_stream_state_and_ignores_tunnel_and_reserved_frames) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true, .receive_origin_advertisements_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    // 1. Server submit_alternative_service_advertisement rejects incomplete head, CONNECT, reserved push, and closed stream.
    (void)client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    (void)ruvia::test::drain_output(client);
    std::string partial_block;
    ruvia::hpack_encoder::encode_header(partial_block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(partial_block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(partial_block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(partial_block, ":path", "/");
    std::string partial_frame(ruvia::http2_frame_header_bytes, '\0');
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(partial_frame).first(9),
        static_cast<std::uint32_t>(partial_block.size()), ruvia::http2_frame_type::headers, 0x1, 1));
    partial_frame.append(partial_block);
    EXPECT_TRUE(server.feed(partial_frame) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::invalid_state);
    std::array<char, 9> cont{};
    EXPECT_TRUE(ruvia::encode_http2_frame_header(cont, 0, ruvia::http2_frame_type::continuation, 0x4, 1));
    EXPECT_TRUE(server.feed(std::string_view(cont.data(), cont.size())) == ruvia::http2_feed_result::accepted);
    auto req1 = server.next_event();
    (void)server.next_event();
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::accepted);

    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/pushed"});
    EXPECT_TRUE(pushed.index() == 0);
    // Pushed stream in reserved (local) state cannot send ALTSVC before response headers open it.
    // An interim response is a response HEADERS frame too (RFC 9113 §8.4.2): the
    // stream is half-closed (remote) afterwards and the server may advertise on it.
    std::uint32_t pushed_id = 0;
    if (pushed.index() == 0) {
        pushed_id = std::get<0>(pushed).stream_id();
        EXPECT_TRUE(server.submit_alternative_service_advertisement(pushed_id, "", "h3=\":443\"") ==
                    ruvia::http2_submit_status::invalid_state);
        EXPECT_TRUE(server.submit_interim_response_head(pushed_id, ruvia::http_interim_response_head(ruvia::http_status::early_hints)) ==
                    ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.submit_alternative_service_advertisement(pushed_id, "", "h3=\":443\"") ==
                    ruvia::http2_submit_status::accepted);
        (void)server.release(std::move(std::get<0>(pushed)));
    }

    ruvia::http_response resp;
    resp.status(ruvia::http_status::no_content);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        server.submit_response_head(1, resp, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, resp))));
    // Stream 1 is now closed (retained only by req1 lease); ALTSVC submission must be rejected.
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::invalid_state);
    if (req1 && req1->request_head()) {
        (void)server.release(std::move(*req1->request_head()));
    }
    relay.transfer(server, client);
    bool pushed_altsvc_seen = false;
    while (auto event = client.next_event()) {
        if (const auto* advertisement = event->alternative_service_advertisement();
            advertisement != nullptr && advertisement->stream_id_ == pushed_id) {
            pushed_altsvc_seen = advertisement->field_value_ == "h3=\":443\"";
        }
    }
    EXPECT_TRUE(pushed_altsvc_seen);
    EXPECT_TRUE(!client.connection_error());

    // 2. Open CONNECT tunnel ignores ALTSVC on stream (RFC 7838 section 4 / RFC 9113
    // section 5.5 discard) and rejects server submission.
    (void)client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    const auto connect_submitted = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect_submitted));
    const auto connect_id = std::get<std::uint32_t>(connect_submitted);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> req3_lease;
    std::optional<ruvia::http2_request_head_event> connect_lease;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            if (rh->stream_id() == connect_id) {
                EXPECT_TRUE(server.submit_alternative_service_advertisement(connect_id, "", "h3=\":443\"") ==
                            ruvia::http2_submit_status::invalid_state);
                ruvia::http_response ok_resp;
                ok_resp.status(ruvia::http_status::ok);
                EXPECT_TRUE(server.submit_connect_response_head(connect_id, ok_resp) == ruvia::http2_submit_status::accepted);
                EXPECT_TRUE(server.submit_alternative_service_advertisement(connect_id, "", "h3=\":443\"") ==
                            ruvia::http2_submit_status::invalid_state);
                connect_lease.emplace(std::move(*rh));
            } else {
                req3_lease.emplace(std::move(*rh));
            }
        }
    }
    relay.transfer(server, client);
    while (client.next_event()) {
    }

    std::string altsvc_on_tunnel(ruvia::http2_frame_header_bytes + 8, '\0');
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(altsvc_on_tunnel).first(9),
        8, ruvia::http2_frame_type::alternative_service, 0, connect_id));
    EXPECT_TRUE(client.feed(altsvc_on_tunnel) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.connection_error());
    while (auto event = client.next_event()) {
        EXPECT_TRUE(event->stream_closed() == nullptr);
    }
    EXPECT_TRUE(!client.stream_aborted(connect_id));
    // No RST_STREAM (or any other frame) answers the ignored frame.
    EXPECT_TRUE(ruvia::test::drain_output(client).empty());

    // 3. ALTSVC on a reserved push stream is an extension frame the stream cannot
    //    use (RFC 7838 section 4, RFC 9113 section 5.5): ignored without a reply.
    auto pushed2 = server.submit_push_request(3, {.authority_ = "example.test", .path_ = "/reserved"});
    EXPECT_TRUE(pushed2.index() == 0);
    if (pushed2.index() == 0) {
        const auto pushed2_id = std::get<0>(pushed2).stream_id();
        relay.transfer(server, client);
        while (client.next_event()) {
        }
        std::string altsvc_on_reserved(ruvia::http2_frame_header_bytes + 8, '\0');
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(altsvc_on_reserved).first(9),
            8, ruvia::http2_frame_type::alternative_service, 0, pushed2_id));
        EXPECT_TRUE(client.feed(altsvc_on_reserved) == ruvia::http2_feed_result::accepted);
        EXPECT_TRUE(!client.connection_error());
        EXPECT_TRUE(!client.next_event().has_value());
        EXPECT_TRUE(ruvia::test::drain_output(client).empty());
        (void)server.release(std::move(std::get<0>(pushed2)));
    }
    if (connect_lease) {
        (void)server.release(std::move(*connect_lease));
    }
    if (req3_lease) {
        (void)server.release(std::move(*req3_lease));
    }
}

TEST(Ruvia, http2_connection_ignores_valid_altsvc_on_tunnel_idle_and_closed_streams_and_origin_on_reserved_push) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client(
        {.enable_push_ = true, .receive_origin_advertisements_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto connect_submitted = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect_submitted));
    const auto connect_id = std::get<std::uint32_t>(connect_submitted);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> connect_req;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            connect_req.emplace(std::move(*rh));
        }
    }
    ruvia::http_response ok_resp;
    ok_resp.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(connect_id, ok_resp) ==
                ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    while (client.next_event()) {
    }

    const auto altsvc_tunnel =
        std::get<0>(ruvia::encode_http2_alternative_service_frame(connect_id, "", "h3=\":443\"", 16384));
    const auto altsvc_idle =
        std::get<0>(ruvia::encode_http2_alternative_service_frame(99, "", "h3=\":443\"", 16384));
    EXPECT_TRUE(client.feed(std::string_view(altsvc_tunnel.data(), altsvc_tunnel.size())) ==
                ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.next_event().has_value());
    EXPECT_TRUE(client.feed(std::string_view(altsvc_idle.data(), altsvc_idle.size())) ==
                ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.next_event().has_value());
    EXPECT_TRUE(!client.connection_error().has_value());

    const auto get_submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(get_submitted));
    const auto get_id = std::get<std::uint32_t>(get_submitted);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> get_req;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            get_req.emplace(std::move(*rh));
        }
    }
    auto pushed = server.submit_push_request(get_id, {.authority_ = "example.test", .path_ = "/pushed"});
    EXPECT_TRUE(pushed.index() == 0);
    const auto push_id = std::get<0>(pushed).stream_id();
    relay.transfer(server, client);
    while (client.next_event()) {
    }

    // Server ignores client-sent ALTSVC on reserved(local) push stream (RFC 7838 section 4).
    std::string client_altsvc;
    std::array<char, 9> hdr{};
    EXPECT_TRUE(ruvia::encode_http2_frame_header(hdr, 11, ruvia::http2_frame_type::alternative_service, 0, push_id));
    client_altsvc.append(hdr.data(), hdr.size());
    client_altsvc.append("\0\0h3=\":443\"", 11);
    EXPECT_TRUE(server.feed(client_altsvc) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error().has_value());

    // Client ignores server-sent ORIGIN on reserved(remote) push stream (RFC 8336 section 2.2).
    std::string server_origin;
    EXPECT_TRUE(ruvia::encode_http2_frame_header(hdr, 0, ruvia::http2_frame_type::origin, 0, push_id));
    server_origin.append(hdr.data(), hdr.size());
    EXPECT_TRUE(client.feed(server_origin) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.connection_error().has_value());

    (void)server.release(std::move(std::get<0>(pushed)));
    if (get_req) {
        (void)server.release(std::move(*get_req));
    }
    if (connect_req) {
        (void)server.release(std::move(*connect_req));
    }
}

TEST(Ruvia, http2_priority_update_client_submission_and_server_event) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    EXPECT_TRUE(client.submit_priority_update(1, {.urgency_ = 0, .incremental_ = true}) == ruvia::http2_submit_status::accepted);
    relay.transfer(client, server);
    const auto event = server.next_event();
    EXPECT_TRUE(event && event->priority_update());
    EXPECT_EQ(event->priority_update()->element_id_, 1u);
    EXPECT_TRUE(event->priority_update()->fields_.urgency_ == 0);
    EXPECT_TRUE(server.submit_priority_update(1, {}) == ruvia::http2_submit_status::invalid_state);
    EXPECT_TRUE(client.submit_priority_update(2, {}) == ruvia::http2_submit_status::invalid_state);
}
TEST(Ruvia, http2_priority_update_idle_stream_budget_and_wrong_peer_role) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    for (std::uint32_t i = 0; i < 128; ++i) {
        EXPECT_TRUE(client.submit_priority_update(i * 2 + 1, {.urgency_ = 3}) == ruvia::http2_submit_status::accepted);
        relay.transfer(client, server);
        EXPECT_TRUE(server.next_event()->priority_update());
    }
    EXPECT_TRUE(client.submit_priority_update(257, {}) == ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(relay.feed(client, server) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
}

TEST(Ruvia, http2_priority_update_rejects_invalid_ids_even_with_malformed_values) {
    ruvia::test::output_relay relay;
    for (const char id : {char{0}, char{2}}) {
        auto client = ruvia::http2_connection::client();
        auto server = ruvia::http2_connection::server();
        relay.transfer(client, server);
        relay.transfer(server, client);
        relay.transfer(client, server);
        std::array<char, ruvia::http2_frame_header_bytes + 6> frame{};
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(frame).first(ruvia::http2_frame_header_bytes),
            6, ruvia::http2_frame_type::priority_update, 0, 0));
        frame[ruvia::http2_frame_header_bytes + 3] = id;
        frame[ruvia::http2_frame_header_bytes + 4] = 'u';
        frame[ruvia::http2_frame_header_bytes + 5] = '=';
        EXPECT_TRUE(server.feed(std::string_view(frame.data(), frame.size())) == ruvia::http2_feed_result::protocol_failure);
        EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
    }
}

TEST(Ruvia, http2_priority_update_zero_length_on_client_yields_protocol_error) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    // Empty PRIORITY_UPDATE frame on stream 0 sent to client
    std::array<char, ruvia::http2_frame_header_bytes> frame{};
    EXPECT_TRUE(ruvia::encode_http2_frame_header(
        std::span(frame.data(), frame.size()),
        0 /* length */,
        ruvia::http2_frame_type::priority_update,
        0 /* flags */,
        0 /* stream_id */));

    const auto feed_status = client.feed(std::string_view(frame.data(), frame.size()));
    EXPECT_TRUE(feed_status == ruvia::http2_feed_result::protocol_failure);
    // RFC 9218 §7.1: client receiving PRIORITY_UPDATE MUST treat as PROTOCOL_ERROR,
    // not FRAME_SIZE_ERROR even though length is 0 (< 4).
    EXPECT_TRUE(client.connection_error() == ruvia::http2_error_code::protocol_error);
}

TEST(Ruvia, http2_priority_update_on_idle_stream_is_attached_to_snapshot_and_cleaned_up_on_release) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    EXPECT_TRUE(client.submit_priority_update(1, {.urgency_ = 1, .incremental_ = true}) ==
                ruvia::http2_submit_status::accepted);
    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/prio"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    relay.transfer(client, server);

    std::optional<ruvia::http2_request_head_event> req_head;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            req_head.emplace(std::move(*rh));
        }
    }
    EXPECT_TRUE(req_head.has_value());
    if (req_head) {
        EXPECT_TRUE(req_head->snapshot().priority_update_.has_value());
        if (req_head->snapshot().priority_update_) {
            EXPECT_EQ(req_head->snapshot().priority_update_->request_priority().urgency_, std::uint8_t{1});
            EXPECT_TRUE(req_head->snapshot().priority_update_->request_priority().incremental_);
        }
        ruvia::http_response resp;
        resp.status(ruvia::http_status::no_content);
        (void)server.submit_response_head(
            1, resp, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, resp));
        (void)server.release(std::move(*req_head));
    }
}

namespace {

using ruvia::http2_cleartext_preface_probe;
using ruvia::http2_client_preface;
using ruvia::probe_http2_cleartext_preface;

}  // namespace

TEST(Ruvia, http2_cleartext_preface_accepts_only_prior_knowledge) {
    EXPECT_TRUE(probe_http2_cleartext_preface("GET / HTTP/1.1\r\n") ==
                http2_cleartext_preface_probe::http1);
    EXPECT_TRUE(probe_http2_cleartext_preface("") == http2_cleartext_preface_probe::http1);
    EXPECT_TRUE(probe_http2_cleartext_preface("PRI * HT") == http2_cleartext_preface_probe::need_more_preface);
    EXPECT_TRUE(probe_http2_cleartext_preface(http2_client_preface) ==
                http2_cleartext_preface_probe::complete_preface);
    EXPECT_TRUE(probe_http2_cleartext_preface("PRI /not-http2\r\n") ==
                http2_cleartext_preface_probe::drop_connection);
}

TEST(Ruvia, frame_public_codec_preserves_wire_bytes_and_buffer_boundaries) {
    std::array<char, 11> storage{};
    storage.fill('#');
    auto output = std::span(storage).subspan(1, ruvia::http2_frame_header_bytes);
    EXPECT_TRUE(ruvia::encode_http2_frame_header(output, 0xabcdef,
        static_cast<ruvia::http2_frame_type>(0xfe), 0x81, 0x7f123456));
    const std::array<unsigned char, 9> expected{0xab, 0xcd, 0xef, 0xfe, 0x81, 0x7f, 0x12, 0x34, 0x56};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(static_cast<unsigned char>(output[i]), expected[i]);
    }
    EXPECT_EQ(storage.front(), '#');
    EXPECT_EQ(storage.back(), '#');
    // A received reserved bit is ignored; unknown frame types remain available
    // to the connection's extension handling rather than being rejected here.
    output[5] = static_cast<char>(0xff);
    const auto decoded = ruvia::parse_http2_frame_header(output);
    EXPECT_TRUE(decoded.has_value());
    if (decoded) {
        EXPECT_EQ(decoded->length_, std::uint32_t{0xabcdef});
        EXPECT_EQ(decoded->type_, std::uint8_t{0xfe});
        EXPECT_EQ(decoded->flags_, std::uint8_t{0x81});
        EXPECT_EQ(decoded->stream_id_, std::uint32_t{0x7f123456});
    }
}

TEST(Ruvia, frame_public_codec_rejects_invalid_input_without_writing) {
    std::array<char, ruvia::http2_frame_header_bytes> output{};
    output.fill('#');
    const auto unchanged = output;
    for (std::size_t size = 0; size < output.size(); ++size) {
        const auto short_buffer = std::span(output).first(size);
        EXPECT_TRUE(!ruvia::encode_http2_frame_header(short_buffer, 0, ruvia::http2_frame_type::data, 0, 1));
        EXPECT_TRUE(!ruvia::parse_http2_frame_header(short_buffer));
        EXPECT_EQ(output, unchanged);
    }
    EXPECT_TRUE(!ruvia::encode_http2_frame_header(output, 0x1000000, ruvia::http2_frame_type::data, 0, 1));
    EXPECT_EQ(output, unchanged);
    EXPECT_TRUE(!ruvia::encode_http2_frame_header(output, 0, ruvia::http2_frame_type::data, 0, 0x80000000));
    EXPECT_EQ(output, unchanged);
}

TEST(Ruvia, http_connection_advertisement_origin_frames_preserve_results) {
    const std::array<std::string_view, 2> origins{"https://example.test", "https://other.test"};
    const auto retained = ruvia::encode_http2_origin_frame(origins, 16384);
    ASSERT_EQ(retained.index(), 0u);
    auto parsed_value = ruvia::decode_http_origin_advertisement(std::span(std::get<0>(retained)).subspan(9));
    ASSERT_EQ(parsed_value.index(), 0u);
    ASSERT_EQ(std::get<0>(parsed_value).origins_.size(), 2u);
    for (std::size_t i = 0; i < 32; ++i) {
        const auto frame = ruvia::encode_http2_origin_frame(origins, 16384);
        ASSERT_EQ(frame.index(), 0u);
        const auto result_value = ruvia::decode_http_origin_advertisement(
            std::span(std::get<0>(frame)).subspan(ruvia::http2_frame_header_bytes));
        ASSERT_EQ(result_value.index(), 0u);
        EXPECT_EQ(std::get<0>(result_value).origins_.front(), origins.front());
        EXPECT_EQ(std::get<0>(parsed_value).origins_.back(), origins.back());
    }
}

TEST(Ruvia, http_connection_advertisement_alternative_service_respects_protocol_payload_limit) {
    const std::string field_value(ruvia::http2_max_frame_size - 1, 'a');
    for (const auto maximum : {ruvia::http2_max_frame_size, (std::numeric_limits<std::uint32_t>::max)()}) {
        const auto result_value = ruvia::encode_http2_alternative_service_frame(1, "", field_value, maximum);
        ASSERT_NE(result_value.index(), 0u);
        EXPECT_EQ(std::get<1>(result_value), ruvia::http_connection_advertisement_error::limit);
    }
}

TEST(Ruvia, http_connection_advertisement_encoders_honor_exact_payload_budgets) {
    const std::array<std::string_view, 2> origins{"https://one.test", "https://two.test"};
    const auto payload_bytes = 4 + origins[0].size() + origins[1].size();
    const auto http2_frame = ruvia::encode_http2_origin_frame(origins, static_cast<std::uint32_t>(payload_bytes));
    ASSERT_EQ(http2_frame.index(), 0u);
    const auto header_value = ruvia::parse_http2_frame_header(std::get<0>(http2_frame));
    ASSERT_TRUE(header_value.has_value());
    EXPECT_EQ(header_value->length_, payload_bytes);
    EXPECT_EQ(std::get<0>(http2_frame).size(), payload_bytes + ruvia::http2_frame_header_bytes);

    const std::array<std::string_view, 0> no_origins{};
    const auto empty_http2 = ruvia::encode_http2_origin_frame(no_origins, 0);
    ASSERT_EQ(empty_http2.index(), 0u);
    EXPECT_EQ(std::get<0>(empty_http2).size(), ruvia::http2_frame_header_bytes);

    for (const auto stream_id : {0u, 1u}) {
        const std::string_view origin = stream_id == 0 ? origins[0] : "";
        const std::string_view value = "clear";
        const auto size = static_cast<std::uint32_t>(2 + origin.size() + value.size());
        const auto result_value = ruvia::encode_http2_alternative_service_frame(stream_id, origin, value, size);
        ASSERT_EQ(result_value.index(), 0u);
        const auto decoded = ruvia::decode_http2_alternative_service(stream_id,
            std::span(std::get<0>(result_value)).subspan(ruvia::http2_frame_header_bytes));
        ASSERT_EQ(decoded.index(), 0u);
        EXPECT_EQ(std::get<0>(decoded).origin_, origin);
        EXPECT_EQ(std::get<0>(decoded).field_value_, value);

        const auto limited = ruvia::encode_http2_alternative_service_frame(stream_id, origin, value, size - 1);
        ASSERT_NE(limited.index(), 0u);
        EXPECT_EQ(std::get<1>(limited), ruvia::http_connection_advertisement_error::limit);
    }
    const auto http2_limited = ruvia::encode_http2_origin_frame(origins,
        static_cast<std::uint32_t>(payload_bytes - 1));
    ASSERT_NE(http2_limited.index(), 0u);
    EXPECT_EQ(std::get<1>(http2_limited), ruvia::http_connection_advertisement_error::limit);
}

TEST(Ruvia, http_connection_advertisement_invalid_entries_truncation_and_alternative_service_association) {
    const std::array<char, 5> invalid{0, 3, 'b', 'a', 'd'};
    const auto ignored = ruvia::decode_http_origin_advertisement(invalid);
    EXPECT_TRUE((ignored.index() == 0) && std::get<0>(ignored).origins_.empty());
    EXPECT_TRUE((ruvia::decode_http_origin_advertisement(std::span(invalid).first(4)).index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_origin_frame(std::array<std::string_view, 1>{"https://example.test/path"}).index() != 0));
    const auto encoded = ruvia::encode_http2_alternative_service_frame(0, "https://example.test", "h3=\":443\"; ma=60");
    ASSERT_EQ(encoded.index(), 0u);
    const auto parsed_value = ruvia::decode_http2_alternative_service(0, std::span(std::get<0>(encoded)).subspan(9));
    EXPECT_TRUE((parsed_value.index() == 0) && std::get<0>(parsed_value).origin_ == "https://example.test" && std::get<0>(parsed_value).field_value_ == "h3=\":443\"; ma=60");
    EXPECT_TRUE((ruvia::encode_http2_alternative_service_frame(1, "https://example.test", "clear").index() != 0));
    EXPECT_TRUE((ruvia::decode_http2_alternative_service(1, std::span(std::get<0>(encoded)).subspan(9)).index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_alternative_service_frame(0, "", "clear").index() != 0));
    EXPECT_TRUE((ruvia::encode_http2_alternative_service_frame(1, "", "clear\r\n").index() != 0));
}

TEST(Ruvia, http_connection_advertisement_copy_and_move_semantics) {
    static_assert(std::is_nothrow_move_constructible_v<ruvia::http_origin_advertisement>);
    static_assert(std::is_nothrow_move_constructible_v<ruvia::http_alternative_service_advertisement>);

    ruvia::http_origin_advertisement source;
    source.origins_.emplace_back("https://source.example.test");
    ruvia::http_alternative_service_advertisement service;
    service.origin_ = "https://source.example.test";
    service.field_value_ = "h3=\":443\"; ma=86400; persist=1";

    const auto* origins_storage = source.origins_.data();
    const auto copy = source;
    EXPECT_EQ(copy.origins_, source.origins_);
    const auto service_copy = service;
    EXPECT_EQ(service_copy.origin_, service.origin_);
    EXPECT_EQ(service_copy.field_value_, service.field_value_);

    auto moved = std::move(source);
    EXPECT_EQ(moved.origins_.data(), origins_storage);
    ASSERT_EQ(moved.origins_.size(), 1u);
    EXPECT_EQ(moved.origins_[0], "https://source.example.test");
}

namespace {

// A 2xx response head carrying `fields` (CRLF-terminated lines), as an RFC 8441
// server accepts the WebSocket with.
ruvia::http_client_response_head accepted_head(std::string_view fields) {
    ruvia::http1_client_request_writer writer;
    std::array<char, 512> request_buffer{};
    auto prepared = writer.prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), {.method_ = "GET", .target_ = "/ws"}, request_buffer);
    ruvia::http1_client_response_parser parser(
        std::move(std::get<ruvia::prepared_http1_client_request>(prepared)).take_exchange_state());
    auto parsed = parser.parse("HTTP/1.1 200 OK\r\n" + std::string(fields) + "\r\n");
    if (parsed.parsed() == nullptr) {
        throw std::runtime_error("expected parsed response");
    }
    return std::move(*parsed.parsed()).take_head();
}

// The server's selection on an open Extended CONNECT stream.
std::variant<ruvia::websocket_client_negotiation_result_view, ruvia::websocket_client_handshake_error> open_stream_selection(
    const ruvia::websocket_client_negotiation& negotiation, const ruvia::http_client_response_head& head) {
    return ruvia::validate_extended_connect_websocket_client_response(negotiation, head, true);
}

template <typename head_type>
concept ws_client_negotiation_accepts_response_head =
    requires(const ruvia::websocket_client_negotiation& negotiation, head_type&& head) {
        ruvia::validate_extended_connect_websocket_client_response(negotiation, std::forward<head_type>(head), true);
    };

static_assert(ws_client_negotiation_accepts_response_head<const ruvia::http_client_response_head&>);
static_assert(!ws_client_negotiation_accepts_response_head<ruvia::http_client_response_head>);

}  // namespace

TEST(Ruvia, websocket_h2_client_negotiation_ignores_rfc6455_key_exchange_fields) {
    // RFC 8441 section 5: :protocol supersedes Sec-WebSocket-Accept, which an
    // Extended CONNECT client does not process.
    for (const auto& [name, value] : std::array<std::pair<std::string_view, std::string_view>, 2>{{
             {"sec-websocket-accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="},
             {"sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ=="},
         }}) {
        auto connection = ruvia::http2_connection::client({});
        const auto append_frame = [&](std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
                                      std::uint32_t stream_id, std::string_view payload) {
            std::array<char, ruvia::http2_frame_header_bytes> header{};
            EXPECT_TRUE(ruvia::encode_http2_frame_header(
                header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
            wire.append(header.data(), header.size());
            wire.append(payload);
        };
        std::string settings_wire;
        constexpr std::array<char, 6> enable_connect_protocol{0x00, 0x08, 0x00, 0x00, 0x00, 0x01};
        append_frame(settings_wire, ruvia::http2_frame_type::settings, 0, 0,
            std::string_view(enable_connect_protocol.data(), enable_connect_protocol.size()));
        EXPECT_TRUE(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }

        ruvia::websocket_client_negotiation negotiation({});
        const auto submitted =
            ruvia::submit_http2_websocket_client_request(connection, negotiation, "https", "example.test", "/ws");
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        std::string block;
        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
        ruvia::hpack_encoder::encode_header(block, name, value);
        std::string wire;
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, std::get<std::uint32_t>(submitted), block);
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = connection.next_event();
        const auto* head = event ? event->response_head() : nullptr;
        EXPECT_TRUE(head != nullptr);
        if (head != nullptr) {
            EXPECT_TRUE(ruvia::validate_extended_connect_websocket_client_response(negotiation, head->head(), true).index() == 0);
        }
    }
    {
        auto connection = ruvia::http2_connection::client({});
        std::string settings_wire;
        constexpr std::array<char, 6> enable_connect_protocol{0x00, 0x08, 0x00, 0x00, 0x00, 0x01};
        std::array<char, ruvia::http2_frame_header_bytes> header{};
        EXPECT_TRUE(ruvia::encode_http2_frame_header(
            header, static_cast<std::uint32_t>(enable_connect_protocol.size()),
            ruvia::http2_frame_type::settings, 0, 0));
        settings_wire.append(header.data(), header.size());
        settings_wire.append(enable_connect_protocol.data(), enable_connect_protocol.size());
        EXPECT_TRUE(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }
        ruvia::websocket_client_negotiation negotiation({});
        const std::array<ruvia::http_header_view, 1> borrowed_headers{{{"x-trace", "borrowed"}}};
        const auto submitted = ruvia::submit_http2_websocket_client_request(
            connection, negotiation, "https", "example.test", "/ws", borrowed_headers);
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    }

    // RFC 9113 section 8.2.2: connection-specific header fields in HTTP/2
    // responses are rejected as forbidden_field.
    ruvia::websocket_client_negotiation ws_neg({});
    for (const auto& [name, value] : std::array<std::pair<std::string_view, std::string_view>, 6>{{
             {"connection", "keep-alive"},
             {"upgrade", "websocket"},
             {"keep-alive", "timeout=5"},
             {"proxy-connection", "keep-alive"},
             {"transfer-encoding", "chunked"},
             {"te", "trailers"},
         }}) {
        const auto head = accepted_head(std::string(name) + ": " + std::string(value) + "\r\n");
        const auto res = open_stream_selection(ws_neg, head);
        EXPECT_TRUE(std::holds_alternative<ruvia::websocket_client_handshake_error>(res) &&
                    std::get<ruvia::websocket_client_handshake_error>(res) ==
                        ruvia::websocket_client_handshake_error::forbidden_field);
    }
}

// RFC 8441 section 5: END_STREAM after the 2xx is the server's orderly tunnel
// close; only END_STREAM on the response HEADERS leaves no WebSocket stream.
// The fact is fixed by the HEADERS frame even when the DATA that ends the
// stream arrives in the same feed().
TEST(Ruvia, websocket_h2_client_response_head_reports_end_stream_only_from_headers) {
    for (const bool end_on_headers : {false, true}) {
        auto connection = ruvia::http2_connection::client({});
        const auto append_frame = [&](std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
                                      std::uint32_t stream_id, std::string_view payload) {
            std::array<char, ruvia::http2_frame_header_bytes> header{};
            EXPECT_TRUE(ruvia::encode_http2_frame_header(
                header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
            wire.append(header.data(), header.size());
            wire.append(payload);
        };
        std::string settings_wire;
        constexpr std::array<char, 6> enable_connect_protocol{0x00, 0x08, 0x00, 0x00, 0x00, 0x01};
        append_frame(settings_wire, ruvia::http2_frame_type::settings, 0, 0,
            std::string_view(enable_connect_protocol.data(), enable_connect_protocol.size()));
        EXPECT_TRUE(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }
        ruvia::websocket_client_negotiation negotiation({});
        const auto submitted =
            ruvia::submit_http2_websocket_client_request(connection, negotiation, "https", "example.test", "/ws");
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        std::string block;
        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
        std::string wire;
        append_frame(wire, ruvia::http2_frame_type::headers, end_on_headers ? 0x5 : 0x4, stream_id, block);
        if (!end_on_headers) {
            // The server's Close frame (1000, "bye") with END_STREAM.
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, stream_id, std::string_view("\x88\x05\x03\xe8"
                                                                                               "bye",
                                                                                  7));
        }
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = connection.next_event();
        const auto* head = event ? event->response_head() : nullptr;
        EXPECT_TRUE(head != nullptr);
        if (head == nullptr) {
            return;
        }
        EXPECT_EQ(head->stream_ended(), end_on_headers);
        const auto negotiated =
            ruvia::validate_extended_connect_websocket_client_response(negotiation, head->head(), !head->stream_ended());
        if (end_on_headers) {
            EXPECT_TRUE(std::holds_alternative<ruvia::websocket_client_handshake_error>(negotiated) &&
                        std::get<ruvia::websocket_client_handshake_error>(negotiated) ==
                            ruvia::websocket_client_handshake_error::stream_closed);
        } else {
            EXPECT_TRUE(negotiated.index() == 0);
            auto data = connection.next_event();
            EXPECT_TRUE(data && data->tunnel_data() != nullptr);
            auto end = connection.next_event();
            EXPECT_TRUE(end && end->tunnel_end() != nullptr);
        }
    }
}
