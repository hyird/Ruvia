// Server-side CONNECT data before the accept/reject decision, graceful drain
// round trip, SETTINGS_ENABLE_CONNECT_PROTOCOL from a client, and extension frames
// on an established tunnel (RFC 9113 sections 5.5, 6.8, 8.5; RFC 8441 section 3;
// RFC 9298 section 5 as updated by RFC 9931).
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_response.h"

#include "output_drain.h"
#include "test_harness.h"

namespace {

constexpr std::uint8_t end_stream_flag = 0x1;
constexpr std::uint8_t end_headers_flag = 0x4;

void append_frame(std::pmr::string& wire, std::uint8_t type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()),
            static_cast<ruvia::http2_frame_type>(type), flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

void append_frame(std::pmr::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload) {
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
    return parse_frames(ruvia::testing::drain_output(connection));
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

std::pmr::string connect_block(std::pmr::memory_resource* resource, std::string_view protocol) {
    std::pmr::string block(resource);
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

std::pmr::string get_block(std::pmr::memory_resource* resource) {
    std::pmr::string block(resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");
    return block;
}

std::pmr::string opening_wire(std::pmr::memory_resource* resource) {
    std::pmr::string wire(ruvia::http2_client_preface, resource);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
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

ruvia::http2_connection started_server(std::pmr::memory_resource* resource) {
    auto server = ruvia::http2_connection::server({.resource_ = resource});
    (void)ruvia::testing::drain_output(server);
    return server;
}

}  // namespace

// RFC 9113 section 8.5 makes every DATA after a CONNECT request's HEADERS tunnel
// data, and RFC 9298 section 5 lets an HTTP/2 client send it before the 2xx.
RUVIA_TEST(http2_connect_data_before_accept_is_held_and_delivered_as_tunnel_data) {
    for (const std::string_view protocol : {std::string_view{}, std::string_view{"connect-udp"}}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto server = started_server(&resource);
        auto wire = opening_wire(&resource);
        append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block(&resource, protocol));
        const std::string_view capsule("\x00\x05\x00ping", 7);
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1, capsule);
        // Enough held bytes to cross the half-window WINDOW_UPDATE threshold.
        const std::string bulk(16384, 'e');
        std::string expected(capsule);
        for (int frame = 0; frame != 33; ++frame) {
            append_frame(wire, ruvia::http2_frame_type::data, 0, 1, bulk);
            expected.append(bulk);
        }
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

        std::optional<ruvia::http2_request_head_event> head;
        std::optional<ruvia::http2_received_data_credit> credit;
        std::string received;
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                RUVIA_CHECK(request->snapshot().connect_pending_);
                head.emplace(std::move(*request));
            } else if (auto* data = event->tunnel_data()) {
                RUVIA_CHECK_EQ(data->stream_id(), 1U);
                received.append(data->bytes());
                if (credit) {
                    credit->merge(data->take_credit());
                } else {
                    credit.emplace(data->take_credit());
                }
            } else {
                RUVIA_CHECK(event->stream_closed() == nullptr);
            }
        }
        RUVIA_CHECK(head.has_value());
        RUVIA_CHECK(received == expected);
        RUVIA_CHECK(credit.has_value() && credit->valid());
        if (!head || !credit) {
            return;
        }
        RUVIA_CHECK(!server.stream_aborted(1));
        RUVIA_CHECK(!server.connection_error().has_value());
        const auto before_accept = take_frames(server);
        RUVIA_CHECK(!has_frame(before_accept, ruvia::http2_frame_type::rst_stream, 1));
        // The held bytes keep their receive-window credit until the owner consumes them.
        RUVIA_CHECK(!has_frame(before_accept, ruvia::http2_frame_type::window_update, 1));
        RUVIA_CHECK(!has_frame(before_accept, ruvia::http2_frame_type::window_update, 0));

        ruvia::http_response accepted(&resource);
        accepted.status(ruvia::http_status::ok);
        RUVIA_CHECK(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
        RUVIA_CHECK(server.acknowledge(std::move(*credit)) ==
                    ruvia::http2_received_data_acknowledge_status::acknowledged);
        const auto after_accept = take_frames(server);
        RUVIA_CHECK(has_frame(after_accept, ruvia::http2_frame_type::headers, 1));
        RUVIA_CHECK(has_frame(after_accept, ruvia::http2_frame_type::window_update, 1));
        RUVIA_CHECK(has_frame(after_accept, ruvia::http2_frame_type::window_update, 0));
        RUVIA_CHECK(!has_frame(after_accept, ruvia::http2_frame_type::rst_stream, 1));

        std::pmr::string more(&resource);
        append_frame(more, ruvia::http2_frame_type::data, end_stream_flag, 1, "tail");
        RUVIA_CHECK(server.feed(more) == ruvia::http2_feed_result::accepted);
        auto tail = server.next_event();
        RUVIA_CHECK(tail && tail->tunnel_data() != nullptr && tail->tunnel_data()->bytes() == "tail");
        const auto end = server.next_event();
        RUVIA_CHECK(end && end->tunnel_end() != nullptr);
        RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    }
}

// Early bytes that ended the request half are followed by tunnel_end on accept.
RUVIA_TEST(http2_connect_data_with_end_stream_before_accept_ends_tunnel_after_accept) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = started_server(&resource);
    auto wire = opening_wire(&resource);
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block(&resource, {}));
    append_frame(wire, ruvia::http2_frame_type::data, end_stream_flag, 1, "hello");
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    std::optional<ruvia::http2_request_head_event> head;
    std::string received;
    while (auto event = server.next_event()) {
        if (auto* request = event->request_head()) {
            head.emplace(std::move(*request));
        } else if (auto* data = event->tunnel_data()) {
            received.append(data->bytes());
        } else {
            RUVIA_CHECK(event->tunnel_end() == nullptr && event->stream_closed() == nullptr);
        }
    }
    RUVIA_CHECK(received == "hello");
    RUVIA_CHECK(head.has_value());
    ruvia::http_response accepted(&resource);
    accepted.status(ruvia::http_status::ok);
    RUVIA_CHECK(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
    const auto end = server.next_event();
    RUVIA_CHECK(end && end->tunnel_end() != nullptr);
    RUVIA_CHECK(server.stream_receive_status(1) == ruvia::http2_stream_receive_status::ended);
    RUVIA_CHECK(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
}

// After a non-2xx CONNECT response the optimistic bytes are discarded with their
// flow-control credit instead of resetting the stream.
RUVIA_TEST(http2_connect_data_after_rejection_is_discarded_with_credit) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = started_server(&resource);
    auto wire = opening_wire(&resource);
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block(&resource, {}));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head_event = server.next_event();
    RUVIA_CHECK(head_event && head_event->request_head() != nullptr);
    if (!head_event || head_event->request_head() == nullptr) {
        return;
    }
    auto head = std::move(*head_event->request_head());
    ruvia::http_response rejected(&resource);
    rejected.status(ruvia::http_status::forbidden);
    const auto submitted = server.submit_response_head(
        1, rejected, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::connect, rejected));
    RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
    (void)take_frames(server);

    std::pmr::string late(&resource);
    const std::string bulk(16384, 'l');
    for (int frame = 0; frame != 33; ++frame) {
        append_frame(late, ruvia::http2_frame_type::data, 0, 1, bulk);
    }
    RUVIA_CHECK(server.feed(late) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.next_event().has_value());
    const auto frames = take_frames(server);
    RUVIA_CHECK(!has_frame(frames, ruvia::http2_frame_type::rst_stream, 1));
    RUVIA_CHECK(has_frame(frames, ruvia::http2_frame_type::window_update, 0));
    RUVIA_CHECK(has_frame(frames, ruvia::http2_frame_type::window_update, 1));
    RUVIA_CHECK(!server.connection_error().has_value());

    std::pmr::string finish(&resource);
    append_frame(finish, ruvia::http2_frame_type::data, end_stream_flag, 1, {});
    RUVIA_CHECK(server.feed(finish) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    RUVIA_CHECK(!server.connection_error().has_value());
    (void)server.release(std::move(head));
}

// RFC 6455 section 4.1 binds the WebSocket client to wait for the 2xx; it gives the
// recipient no reason to reset, so RFC 8441 DATA before the response is held under
// flow control as tunnel data like any other CONNECT.
RUVIA_TEST(http2_websocket_connect_data_before_response_is_held_as_tunnel_data) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = started_server(&resource);
    auto wire = opening_wire(&resource);
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block(&resource, "websocket"));
    append_frame(wire, ruvia::http2_frame_type::data, 0, 1, "frame");
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    bool early_data = false;
    std::optional<ruvia::http2_request_head_event> head;
    while (auto event = server.next_event()) {
        RUVIA_CHECK(event->stream_closed() == nullptr);
        if (auto* request = event->request_head()) {
            head.emplace(std::move(*request));
        } else if (auto* data = event->tunnel_data()) {
            early_data = data->stream_id() == 1 && data->bytes() == "frame";
        }
    }
    RUVIA_CHECK(early_data);
    RUVIA_CHECK(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    RUVIA_CHECK(!server.connection_error().has_value());
    if (head) {
        (void)server.release(std::move(*head));
    }
}

// RFC 7838 section 4 and RFC 8336 section 2.1: a server MUST ignore ALTSVC, and
// ORIGIN on a nonzero stream; an established tunnel keeps them out of its bytes.
RUVIA_TEST(http2_connect_tunnel_ignores_alternative_service_and_origin_frames) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = started_server(&resource);
    auto wire = opening_wire(&resource);
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block(&resource, {}));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    RUVIA_CHECK(head.has_value());
    if (!head) {
        return;
    }
    ruvia::http_response accepted(&resource);
    accepted.status(ruvia::http_status::ok);
    RUVIA_CHECK(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
    (void)take_frames(server);
    while (server.next_event()) {
    }

    std::pmr::string extension(&resource);
    append_frame(extension, ruvia::http2_frame_type::alternative_service, 0, 1, std::string_view("\0\0h3", 4));
    append_frame(extension, ruvia::http2_frame_type::origin, 0, 1, {});
    append_frame(extension, ruvia::http2_frame_type::data, 0, 1, "after");
    RUVIA_CHECK(server.feed(extension) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    RUVIA_CHECK(!server.stream_aborted(1));
    RUVIA_CHECK(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    std::string received;
    while (auto event = server.next_event()) {
        RUVIA_CHECK(event->stream_closed() == nullptr);
        if (auto* data = event->tunnel_data()) {
            received.append(data->bytes());
        }
    }
    RUVIA_CHECK_EQ(received, std::string("after"));
    (void)server.release(std::move(*head));
}

// RFC 9113 section 6.8: the first GOAWAY advertises 2^31-1 so streams the client
// already sent are still served; the final boundary follows one PING round trip.
RUVIA_TEST(http2_server_graceful_drain_admits_in_flight_streams_until_ping_ack) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = started_server(&resource);
    auto wire = opening_wire(&resource);
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block(&resource));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto first = take_request_head(server);
    RUVIA_CHECK(first.has_value());
    (void)take_frames(server);

    server.begin_drain();
    RUVIA_CHECK(server.draining());
    RUVIA_CHECK(!server.drain_final());
    const auto initial = take_frames(server);
    RUVIA_CHECK(goaway_last_stream_ids(initial) == std::vector<std::uint32_t>{0x7fffffffU});
    std::optional<std::string> ping_payload;
    for (const auto& frame : initial) {
        if (frame.is(ruvia::http2_frame_type::ping)) {
            RUVIA_CHECK_EQ(frame.header_.flags_ & 0x1, 0);
            ping_payload = frame.payload_;
        }
    }
    RUVIA_CHECK(ping_payload.has_value());
    if (!ping_payload) {
        return;
    }

    std::pmr::string in_flight(&resource);
    append_frame(in_flight, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 3, get_block(&resource));
    RUVIA_CHECK(server.feed(in_flight) == ruvia::http2_feed_result::accepted);
    auto second = take_request_head(server);
    RUVIA_CHECK(second && second->stream_id() == 3);
    RUVIA_CHECK(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 3));

    std::pmr::string ack(&resource);
    append_frame(ack, ruvia::http2_frame_type::ping, 0x1, 0, *ping_payload);
    RUVIA_CHECK(server.feed(ack) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(server.drain_final());
    RUVIA_CHECK(goaway_last_stream_ids(take_frames(server)) == std::vector<std::uint32_t>{3});

    std::pmr::string refused(&resource);
    append_frame(refused, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 5, get_block(&resource));
    RUVIA_CHECK(server.feed(refused) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.next_event().has_value());
    const auto refusal = take_frames(server);
    RUVIA_CHECK(refusal.size() == 1 && refusal.front().is(ruvia::http2_frame_type::rst_stream) &&
                refusal.front().header_.stream_id_ == 5 && read_u32(refusal.front().payload_) == 7U);
    RUVIA_CHECK(!server.connection_error().has_value());
}

// A client that withholds the PING ACK cannot keep the drain open indefinitely.
RUVIA_TEST(http2_server_graceful_drain_bounds_streams_opened_before_ping_ack) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = started_server(&resource);
    const auto opening = opening_wire(&resource);
    RUVIA_CHECK(server.feed(opening) == ruvia::http2_feed_result::accepted);
    (void)take_frames(server);
    server.begin_drain();
    (void)take_frames(server);
    const auto block = get_block(&resource);
    std::uint32_t stream_id = 1;
    // The drain started at stream 0: 128 further peer streams (1..255) are admitted.
    for (; stream_id <= 255; stream_id += 2) {
        std::pmr::string request(&resource);
        append_frame(request, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, stream_id, block);
        RUVIA_CHECK(server.feed(request) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        RUVIA_CHECK(head.has_value());
        if (!head) {
            return;
        }
        ruvia::http_response response(&resource);
        const auto submitted = server.submit_response_head(stream_id, response,
            ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
        RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
        (void)server.release(std::move(*head));
        const auto frames = take_frames(server);
        RUVIA_CHECK(goaway_last_stream_ids(frames).empty());
        RUVIA_CHECK(!has_frame(frames, ruvia::http2_frame_type::rst_stream, stream_id));
    }
    RUVIA_CHECK(!server.drain_final());
    std::pmr::string beyond(&resource);
    append_frame(beyond, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, stream_id, block);
    RUVIA_CHECK(server.feed(beyond) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.next_event().has_value());
    RUVIA_CHECK(server.drain_final());
    const auto frames = take_frames(server);
    RUVIA_CHECK(goaway_last_stream_ids(frames) == std::vector<std::uint32_t>{255});
    RUVIA_CHECK(has_frame(frames, ruvia::http2_frame_type::rst_stream, stream_id));
}

// A client drains in one step: its boundary covers server-initiated streams only.
RUVIA_TEST(http2_client_graceful_drain_sends_final_goaway_at_once) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    (void)ruvia::testing::drain_output(client);
    client.begin_drain();
    RUVIA_CHECK(client.drain_final());
    const auto frames = take_frames(client);
    RUVIA_CHECK(goaway_last_stream_ids(frames) == std::vector<std::uint32_t>{0});
    for (const auto& frame : frames) {
        RUVIA_CHECK(!frame.is(ruvia::http2_frame_type::ping));
    }
}

// RFC 8441 section 3: "Receipt of this parameter by a server does not have any
// impact." The value range and 1->0 ban remain sender rules enforced by clients.
RUVIA_TEST(http2_server_ignores_enable_connect_protocol_from_client) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = started_server(&resource);
    auto wire = opening_wire(&resource);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x01", 6));
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x00", 6));
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x02", 6));
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block(&resource));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    auto head = server.next_event();
    RUVIA_CHECK(head && head->request_head() != nullptr);
    RUVIA_CHECK(goaway_last_stream_ids(take_frames(server)).empty());

    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    (void)ruvia::testing::drain_output(client);
    std::pmr::string from_server(&resource);
    append_frame(from_server, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x02", 6));
    RUVIA_CHECK(client.feed(from_server) == ruvia::http2_feed_result::protocol_failure);
    RUVIA_CHECK(client.connection_error() == ruvia::http2_error_code::protocol_error);
}

// RFC 9113 section 5.5: unknown frame types are discarded, including on a
// connected CONNECT stream; known non-stream-management frames still reset it.
RUVIA_TEST(http2_connected_tunnel_discards_unknown_extension_frames) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = started_server(&resource);
    auto wire = opening_wire(&resource);
    append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block(&resource, {}));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head_event = server.next_event();
    RUVIA_CHECK(head_event && head_event->request_head() != nullptr);
    if (!head_event || head_event->request_head() == nullptr) {
        return;
    }
    auto head = std::move(*head_event->request_head());
    ruvia::http_response accepted(&resource);
    accepted.status(ruvia::http_status::ok);
    RUVIA_CHECK(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
    (void)take_frames(server);

    std::pmr::string extension(&resource);
    append_frame(extension, std::uint8_t{0x21}, 0, 1, "opaque");
    append_frame(extension, ruvia::http2_frame_type::data, 0, 1, "after");
    RUVIA_CHECK(server.feed(extension) == ruvia::http2_feed_result::accepted);
    auto data = server.next_event();
    RUVIA_CHECK(data && data->tunnel_data() != nullptr && data->tunnel_data()->bytes() == "after");
    RUVIA_CHECK(!server.next_event().has_value());
    RUVIA_CHECK(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    RUVIA_CHECK(!server.stream_aborted(1));

    std::pmr::string headers(&resource);
    append_frame(headers, ruvia::http2_frame_type::headers, end_headers_flag, 1, get_block(&resource));
    RUVIA_CHECK(server.feed(headers) == ruvia::http2_feed_result::accepted);
    while (server.next_event()) {
    }
    RUVIA_CHECK(has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    (void)server.release(std::move(head));
}

// RFC 9110 section 8.6 makes Content-Length on CONNECT only a sender SHOULD NOT
// and RFC 9113 section 8.5 does not list it as malformed: a declared zero is
// accepted (as in HTTP/1), while a non-zero length claims unframed content.
RUVIA_TEST(http2_connect_accepts_zero_content_length_and_resets_nonzero) {
    for (const std::string_view protocol : {std::string_view{}, std::string_view{"connect-udp"}}) {
        for (const std::string_view length : {std::string_view{"0"}, std::string_view{"3"}}) {
            std::pmr::unsynchronized_pool_resource resource;
            auto server = started_server(&resource);
            auto wire = opening_wire(&resource);
            auto block = connect_block(&resource, protocol);
            ruvia::hpack_encoder::encode_header(block, "content-length", length);
            append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, block);
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            const auto head = take_request_head(server);
            const auto frames = take_frames(server);
            const bool accepted = length == "0";
            RUVIA_CHECK_EQ(head.has_value(), accepted);
            RUVIA_CHECK_EQ(has_frame(frames, ruvia::http2_frame_type::rst_stream, 1), !accepted);
            RUVIA_CHECK(!server.connection_error().has_value());
        }
    }
}
