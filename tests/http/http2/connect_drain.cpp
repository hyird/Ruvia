// Server-side CONNECT data before the accept/reject decision, graceful drain
// round trip, SETTINGS_ENABLE_CONNECT_PROTOCOL from a client, and extension frames
// on an established tunnel (RFC 9113 sections 5.5, 6.8, 8.5; RFC 8441 section 3;
// RFC 9298 section 5 as updated by RFC 9931).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_response.h"

#include "content_decoding_fixture.h"
#include "test_exceptions.h"

namespace {

constexpr std::uint8_t end_stream_flag = 0x1;
constexpr std::uint8_t end_headers_flag = 0x4;

void append_frame(std::string& wire, std::uint8_t type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), static_cast<ruvia::http2_frame_type>(type), flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload = {}) {
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
    const auto submitted = server.submit_response_head(1, rejected, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::connect, rejected));
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
        const auto submitted = server.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
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
        append_frame(wire, ruvia::http2_frame_type::data, end_stream_flag, 1, std::string_view("\x88\x82\x00\x00\x00\x00\x03\xe8", 8));
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
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1, std::string_view("\x88\x82\x00\x00\x00\x00\x03\xe8", 8));
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

void check_encoded_field(std::string_view output, std::string_view prefix, std::string_view expected_name, std::string_view expected_value) {
    EXPECT_EQ(output.substr(0, prefix.size()), prefix);
    ruvia::hpack_decoder decoder;
    std::size_t fields = 0;
    const auto result = decoder.decode(output.substr(prefix.size()), [&](std::string_view name, std::string_view value) {
        ++fields;
        EXPECT_EQ(name, expected_name);
        EXPECT_EQ(value, expected_value);
        return true;
    });
    EXPECT_TRUE(!result.has_value());
    EXPECT_EQ(fields, std::size_t{1});
}

void check_newest(ruvia::hpack_decoder& decoder, std::uint32_t index, std::string_view expected_name, std::string_view expected_value) {
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
        for (const auto kind : {alias_kind::name, alias_kind::value, alias_kind::disjoint, alias_kind::overlapping, alias_kind::identical, alias_kind::static_name, alias_kind::sensitive_name}) {
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
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}, static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1}) {
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
    EXPECT_TRUE(!decoder
            .decode(table_size_update(64), [](auto, auto) {
        return true;
    }).has_value());
    EXPECT_TRUE(decoder.decode(table_size_update(65), [](auto, auto) {
        return true;
    }) == ruvia::hpack_decode_error::compression_error);
    EXPECT_TRUE(decoder_is_poisoned(decoder));
}

TEST(Ruvia, hpack_public_lowered_capacity_requires_a_leading_update) {
    // RFC 7541 section 4.2: the first block after the decoder's maximum shrinks
    // must begin with a dynamic table size update.
    for (const bool signaled : {false, true}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder
                .decode(indexed_literal("a", "b"), [](auto, auto) {
            return true;
        }).has_value());
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
            EXPECT_TRUE(decoder.decode(indexed_field(62), [](auto, auto) {
                return true;
            }) == ruvia::hpack_decode_error::compression_error);
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
            EXPECT_TRUE(!decoder
                    .decode(block, [](auto, auto) {
                return true;
            }).has_value());
            block.assign(block.size(), '#');
        }
        {
            // Huffman-coded literal: the decoded bytes cannot borrow the input.
            std::string block;
            ruvia::hpack_encoder::encode_header(block, std::string(24, 'a'), std::string(25, 'b'));
            block[0] = static_cast<char>(0x40);
            EXPECT_TRUE(!decoder
                    .decode(std::string_view(block), [](auto, auto) {
                return true;
            }).has_value());
            std::fill(block.begin(), block.end(), '#');
        }
        check_newest(decoder, 62, std::string(24, 'a'), std::string(25, 'b'));
        check_newest(decoder, 63, std::string(24, 'n'), std::string(24, 'v'));
        // RFC 7541 Appendix C.4.1: ":authority: www.example.com" as a Huffman literal.
        constexpr std::string_view huffman_authority("\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 14);
        EXPECT_TRUE(!decoder
                .decode(huffman_authority, [](std::string_view name, std::string_view value) {
            return name == ":authority" && value == "www.example.com";
        }).has_value());
    }
}

TEST(Ruvia, hpack_public_empty_and_one_sided_fields_are_indexable) {
    ruvia::hpack_decoder decoder;
    const std::string long_bytes(49, 'x');
    const std::string block = indexed_literal("", "") + indexed_literal("", long_bytes) + indexed_literal(long_bytes, "");
    EXPECT_TRUE(!decoder
            .decode(block, [](auto, auto) {
        return true;
    }).has_value());
    check_newest(decoder, 62, long_bytes, "");
    check_newest(decoder, 63, "", long_bytes);
    check_newest(decoder, 64, "", "");
}

TEST(Ruvia, hpack_public_dynamic_entries_remain_indexable_across_evictions) {
    ruvia::hpack_decoder decoder;
    EXPECT_TRUE(!decoder
            .decode(table_size_update(120), [](auto, auto) {
        return true;
    }).has_value());
    const std::string name(40, 'n');
    const std::string value(40, 'v');
    const std::string insertion = indexed_literal(name, value);
    for (std::size_t i = 0; i < 200; ++i) {
        EXPECT_TRUE(!decoder
                .decode(insertion, [](auto, auto) {
            return true;
        }).has_value());
        check_newest(decoder, 62, name, value);
    }
}

TEST(Ruvia, hpack_public_leading_capacity_updates_apply_in_wire_order) {
    const std::array<std::array<std::uint32_t, 3>, 3> sequences{{{512, 64, 64}, {512, 64, 512}, {0, 0, 512}}};
    for (const auto& updates : sequences) {
        ruvia::hpack_decoder decoder;
        const std::string seed_value(64, 's');
        EXPECT_TRUE(!decoder
                .decode(indexed_literal("x-seed", seed_value), [](auto, auto) {
            return true;
        }).has_value());
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
        EXPECT_TRUE(decoder.decode(indexed_field(63), [](auto, auto) {
            return true;
        }) == ruvia::hpack_decode_error::compression_error);
    }
}

TEST(Ruvia, hpack_public_capacity_updates_reject_excess_and_late_positions) {
    for (const bool late : {false, true}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder
                .decode(indexed_literal("x-seed", "kept"), [](auto, auto) {
            return true;
        }).has_value());
        const std::string invalid = late ? indexed_field(62) + table_size_update(0) : table_size_update(4097);
        EXPECT_TRUE(decoder.decode(invalid, [](auto, auto) {
            return true;
        }) == ruvia::hpack_decode_error::compression_error);
        EXPECT_TRUE(decoder_is_poisoned(decoder));
    }
}

TEST(Ruvia, hpack_public_callback_rejection_keeps_dynamic_table_consistent) {
    ruvia::hpack_decoder decoder;
    const auto rejected = decoder.decode(indexed_literal("a", "b"), [](auto, auto) {
        return false;
    });
    EXPECT_TRUE(rejected == ruvia::hpack_decode_error::callback_rejected);
    check_newest(decoder, 62, "a", "b");
}

TEST(Ruvia, hpack_public_compression_error_poisons_the_decoder) {
    for (const std::string_view failing : {std::string_view("\x80", 1), std::string_view("\x40\x05\x61", 3)}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder
                .decode(indexed_literal("a", "b"), [](auto, auto) {
            return true;
        }).has_value());
        std::string block = indexed_literal("c", "d");
        block.append(failing);
        EXPECT_TRUE(decoder.decode(block, [](auto, auto) {
            return true;
        }) == ruvia::hpack_decode_error::compression_error);
        EXPECT_TRUE(decoder_is_poisoned(decoder));
    }
}

TEST(Ruvia, hpack_public_long_dynamic_name_and_value_are_preserved) {
    ruvia::hpack_decoder decoder;
    const std::string name(300, 'n');
    const std::string value(300, 'v');
    EXPECT_TRUE(!decoder
            .decode(indexed_literal(name, value), [](auto, auto) {
        return true;
    }).has_value());
    check_newest(decoder, 62, name, value);
}

TEST(Ruvia, hpack_public_eviction_preserves_relative_indices) {
    ruvia::hpack_decoder decoder;
    const std::string long_name(48, 'n');
    std::string initial = table_size_update(1150);
    for (int i = 0; i < 36; ++i) {
        const char value = static_cast<char>('A' + i);
        initial += indexed_literal(i % 2 == 0 ? std::string_view("a") : std::string_view(long_name), std::string_view(&value, 1));
    }
    initial.push_back(static_cast<char>(0x82));
    EXPECT_TRUE(!decoder
            .decode(initial, [](auto, auto) {
        return true;
    }).has_value());
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
    EXPECT_TRUE(!decoder
            .decode(std::string_view("\x7e\x01z", 3), [](auto, auto) {
        return true;
    }).has_value());
    check_newest(decoder, 62, long_name, "z");
}

TEST(Ruvia, hpack_public_decoder_move_preserves_dynamic_entries) {
    ruvia::hpack_decoder source_value;
    EXPECT_TRUE(!source_value
            .decode(indexed_literal("a", "b"), [](auto, auto) {
        return true;
    }).has_value());
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
                    (void)decoder.decode({}, [](auto, auto) {
                        return true;
                    });
                }
            } catch (const std::logic_error&) {
                ++rejected;
            }
            EXPECT_EQ(name, "a");
            EXPECT_EQ(value, "b");
            std::size_t independent_fields = 0;
            EXPECT_TRUE(!independent
                    .decode(std::string_view("\x82", 1), [&](auto inner_name, auto inner_value) {
                ++independent_fields;
                EXPECT_EQ(inner_name, ":method");
                EXPECT_EQ(inner_value, "GET");
                return true;
            }).has_value());
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
            const auto accept = [](auto, auto) {
                return true;
            };
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
        const char wire_byte = static_cast<char>(0x80U | index);
        std::string encoded;
        std::string name;
        std::size_t fields_value = 0;
        const auto decoded = decoder.decode(std::string_view(&wire_byte, 1), [&](std::string_view field_name, std::string_view value) {
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
        EXPECT_EQ(encoded, std::string_view(&wire_byte, 1));
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
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1, std::string_view(chunk).substr(0, count));
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
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"})));
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
concept settings_borrowable = requires(value_type&& value) { std::forward<value_type>(value).settings(); };
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
            record.request_matches_ = view.method_ == "GET" && view.scheme_ == "https" && view.authority_ == "example.test" && view.path_ == "/metadata";
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
    const std::array<char, 6> entry{static_cast<char>(id >> 8), static_cast<char>(id), static_cast<char>(value >> 24), static_cast<char>(value >> 16), static_cast<char>(value >> 8), static_cast<char>(value)};
    payload.append(entry.data(), entry.size());
}

std::string window_payload(std::uint32_t increment) {
    const std::array<char, 4> bytes{static_cast<char>(increment >> 24), static_cast<char>(increment >> 16), static_cast<char>(increment >> 8), static_cast<char>(increment)};
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
        auto connection = role == ruvia::http2_role::server ? ruvia::http2_connection::server() : ruvia::http2_connection::client();
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
        auto connection = role == ruvia::http2_role::server ? ruvia::http2_connection::server(options) : ruvia::http2_connection::client(options);
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
        const auto expected = static_cast<std::size_t>(index + 1 >= settings_end) + static_cast<std::size_t>(index + 1 >= first_window_end) + static_cast<std::size_t>(index + 1 == wire.size());
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
        const std::array kinds{received_kind::settings, received_kind::window_update, received_kind::priority, received_kind::request_head, received_kind::window_update};
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
        const std::array expected{ruvia::http2_request_pseudo_header::method, ruvia::http2_request_pseudo_header::scheme, ruvia::http2_request_pseudo_header::authority, ruvia::http2_request_pseudo_header::path};
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
        EXPECT_TRUE(server.connection_error() == (type == ruvia::http2_frame_type::window_update ? ruvia::http2_error_code::protocol_error : ruvia::http2_error_code::frame_size_error));
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
                log.fields_.push_back({metadata.first_frame_observation_sequence_, metadata.frame_observation_sequence_, metadata.field_index_, std::string(metadata.name_), std::string(metadata.value_)});
                break;
        }
    }
};
}  // namespace http2_receive_metadata_test
using namespace http2_receive_metadata_test;

TEST(Ruvia, http2_wire_observers_preserve_raw_settings_priority_and_fragmented_hpack) {
    for (const auto fragment : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
        wire_observation_log observed;
        auto server = ruvia::http2_connection::server({.frame_observer_ = &wire_observation_log::frame, .header_observer_ = &wire_observation_log::header, .observer_context_ = &observed});
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
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 3, std::string_view("\xbe\x84\x87\x82", 4));
        received_log requests;
        for (std::size_t cursor = 0; cursor < wire.size(); cursor += fragment) {
            EXPECT_TRUE(server.feed(std::string_view(wire).substr(cursor, fragment)) == ruvia::http2_feed_result::accepted);
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
        for (const auto& field : std::array{std::pair{":protocol", "websocket"}, std::pair{":path", "/chat"}, std::pair{":scheme", "https"}, std::pair{":authority", "example.test:443"}, std::pair{":method", "CONNECT"}}) {
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
        const ruvia::http2_connection_options options{.frame_observer_ = &wire_observation_log::frame, .observer_context_ = &observed};
        auto connection = role == ruvia::http2_role::server ? ruvia::http2_connection::server(options) : ruvia::http2_connection::client(options);
        std::string wire = role == ruvia::http2_role::server ? std::string(ruvia::http2_client_preface) : std::string{};
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
            auto client = ruvia::http2_connection::client({.receive_origin_advertisements_ = true, .frame_observer_ = observe ? &wire_observation_log::frame : nullptr, .observer_context_ = &observed});
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
                ASSERT_TRUE(client.feed(std::string_view(wire).substr(cursor, fragment)) == ruvia::http2_feed_result::accepted);
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
