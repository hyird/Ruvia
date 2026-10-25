#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

#include "output_drain.h"
#include "test_harness.h"
RUVIA_TEST(http2_priority_update_client_submission_and_server_event) {
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    RUVIA_CHECK(client.submit_priority_update(1, {.urgency_ = 0, .incremental_ = true}) == ruvia::http2_submit_status::accepted);
    relay.transfer(client, server);
    const auto event = server.next_event();
    RUVIA_CHECK(event && event->priority_update());
    RUVIA_CHECK_EQ(event->priority_update()->element_id_, 1u);
    RUVIA_CHECK(event->priority_update()->fields_.urgency_ == 0);
    RUVIA_CHECK(server.submit_priority_update(1, {}) == ruvia::http2_submit_status::invalid_state);
    RUVIA_CHECK(client.submit_priority_update(2, {}) == ruvia::http2_submit_status::invalid_state);
}
RUVIA_TEST(http2_priority_update_idle_stream_budget_and_wrong_peer_role) {
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    for (std::uint32_t i = 0; i < 128; ++i) {
        RUVIA_CHECK(client.submit_priority_update(i * 2 + 1, {.urgency_ = 3}) == ruvia::http2_submit_status::accepted);
        relay.transfer(client, server);
        RUVIA_CHECK(server.next_event()->priority_update());
    }
    RUVIA_CHECK(client.submit_priority_update(257, {}) == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(relay.feed(client, server) == ruvia::http2_feed_result::protocol_failure);
    RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::protocol_error);
}

RUVIA_TEST(http2_priority_update_ignores_malformed_values_without_consuming_idle_budget) {
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    for (std::uint32_t index = 0; index != 128; ++index) {
        RUVIA_CHECK(client.submit_priority_update(index * 2 + 1, {.urgency_ = 3}) == ruvia::http2_submit_status::accepted);
        relay.transfer(client, server);
        RUVIA_CHECK(server.next_event()->priority_update());
    }

    std::array<char, ruvia::http2_frame_header_bytes + 6> frame{};
    RUVIA_CHECK(ruvia::encode_http2_frame_header(std::span(frame).first(ruvia::http2_frame_header_bytes),
        6, ruvia::http2_frame_type::priority_update, 0, 0));
    // Stream 257 followed by a malformed Structured Fields dictionary.
    frame[ruvia::http2_frame_header_bytes + 2] = 1;
    frame[ruvia::http2_frame_header_bytes + 3] = 1;
    frame[ruvia::http2_frame_header_bytes + 4] = 'u';
    frame[ruvia::http2_frame_header_bytes + 5] = '=';
    const auto status = server.feed(std::string_view(frame.data(), frame.size()));
    RUVIA_CHECK(status == ruvia::http2_feed_result::accepted || status == ruvia::http2_feed_result::need_input);
    RUVIA_CHECK(!server.connection_error().has_value());
    RUVIA_CHECK(!server.next_event());
    if (server.connection_error().has_value()) {
        return;
    }

    RUVIA_CHECK(client.submit_priority_update(1, {.urgency_ = 0}) == ruvia::http2_submit_status::accepted);
    relay.transfer(client, server);
    const auto update = server.next_event();
    RUVIA_CHECK(update && update->priority_update() && update->priority_update()->fields_.urgency_ == 0);

    RUVIA_CHECK(client.submit_priority_update(257, {}) == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(relay.feed(client, server) == ruvia::http2_feed_result::protocol_failure);
    RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::protocol_error);
}

RUVIA_TEST(http2_priority_update_rejects_invalid_ids_even_with_malformed_values) {
    ruvia::testing::output_relay relay;
    for (const char id : {char{0}, char{2}}) {
        auto client = ruvia::http2_connection::client();
        auto server = ruvia::http2_connection::server();
        relay.transfer(client, server);
        relay.transfer(server, client);
        relay.transfer(client, server);
        std::array<char, ruvia::http2_frame_header_bytes + 6> frame{};
        RUVIA_CHECK(ruvia::encode_http2_frame_header(std::span(frame).first(ruvia::http2_frame_header_bytes),
            6, ruvia::http2_frame_type::priority_update, 0, 0));
        frame[ruvia::http2_frame_header_bytes + 3] = id;
        frame[ruvia::http2_frame_header_bytes + 4] = 'u';
        frame[ruvia::http2_frame_header_bytes + 5] = '=';
        RUVIA_CHECK(server.feed(std::string_view(frame.data(), frame.size())) == ruvia::http2_feed_result::protocol_failure);
        RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::protocol_error);
    }
}

RUVIA_TEST(http2_priority_update_zero_length_on_client_yields_protocol_error) {
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    // Empty PRIORITY_UPDATE frame on stream 0 sent to client
    std::array<char, ruvia::http2_frame_header_bytes> frame{};
    RUVIA_CHECK(ruvia::encode_http2_frame_header(
        std::span(frame.data(), frame.size()),
        0 /* length */,
        ruvia::http2_frame_type::priority_update,
        0 /* flags */,
        0 /* stream_id */));

    const auto feed_status = client.feed(std::string_view(frame.data(), frame.size()));
    RUVIA_CHECK(feed_status == ruvia::http2_feed_result::protocol_failure);
    // RFC 9218 §7.1: client receiving PRIORITY_UPDATE MUST treat as PROTOCOL_ERROR,
    // not FRAME_SIZE_ERROR even though length is 0 (< 4).
    RUVIA_CHECK(client.connection_error() == ruvia::http2_error_code::protocol_error);
}

// RFC 9218 section 7.1 makes over-committed idle priorities a connection error
// only for the PRIORITY_UPDATE receiver. A HEADERS that would exceed the limit
// together with retained idle priorities is a refused stream (RFC 9113 5.1.2).
RUVIA_TEST(http2_headers_crowded_by_idle_priorities_is_refused_not_connection_error) {
    auto server = ruvia::http2_connection::server();
    (void)ruvia::testing::drain_output(server);
    std::string wire(ruvia::http2_client_preface);
    const auto append = [&](ruvia::http2_frame_type type, std::uint8_t flags, std::uint32_t stream,
                            std::string_view payload) {
        std::array<char, ruvia::http2_frame_header_bytes> header{};
        RUVIA_CHECK(ruvia::encode_http2_frame_header(
            header, static_cast<std::uint32_t>(payload.size()), type, flags, stream));
        wire.append(header.data(), header.size());
        wire.append(payload);
    };
    append(ruvia::http2_frame_type::settings, 0, 0, {});
    for (std::uint32_t index = 0; index != 128; ++index) {
        const std::uint32_t id = index * 2 + 3;
        const std::array<char, 7> payload{static_cast<char>(id >> 24), static_cast<char>(id >> 16),
            static_cast<char>(id >> 8), static_cast<char>(id), 'u', '=', '3'};
        append(ruvia::http2_frame_type::priority_update, 0, 0, std::string_view(payload.data(), payload.size()));
    }
    std::pmr::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");
    append(ruvia::http2_frame_type::headers, 0x5, 1, block);
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    bool saw_request = false;
    while (auto event = server.next_event()) {
        saw_request = saw_request || event->request_head() != nullptr;
    }
    RUVIA_CHECK(!saw_request);
    std::string refused;
    {
        std::array<char, ruvia::http2_frame_header_bytes> header{};
        RUVIA_CHECK(ruvia::encode_http2_frame_header(header, 4, ruvia::http2_frame_type::rst_stream, 0, 1));
        refused.append(header.data(), header.size());
        refused.append("\0\0\0\x07", 4);
    }
    RUVIA_CHECK(ruvia::testing::drain_output(server).find(refused) != std::string::npos);

    // The prioritized stream itself still fits: 127 idle priorities remain above it.
    wire.clear();
    append(ruvia::http2_frame_type::headers, 0x5, 3, block);
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    saw_request = false;
    while (auto event = server.next_event()) {
        saw_request = saw_request || event->request_head() != nullptr;
    }
    RUVIA_CHECK(saw_request);
}

RUVIA_TEST(http2_priority_update_on_idle_stream_is_attached_to_snapshot_and_cleaned_up_on_release) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    RUVIA_CHECK(client.submit_priority_update(1, {.urgency_ = 1, .incremental_ = true}) ==
                ruvia::http2_submit_status::accepted);
    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/prio"});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
    relay.transfer(client, server);

    std::optional<ruvia::http2_request_head_event> req_head;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            req_head.emplace(std::move(*rh));
        }
    }
    RUVIA_CHECK(req_head.has_value());
    if (req_head) {
        RUVIA_CHECK(req_head->snapshot().priority_update_.has_value());
        if (req_head->snapshot().priority_update_) {
            RUVIA_CHECK_EQ(req_head->snapshot().priority_update_->request_priority().urgency_, std::uint8_t{1});
            RUVIA_CHECK(req_head->snapshot().priority_update_->request_priority().incremental_);
        }
        ruvia::http_response resp(&resource);
        resp.status(ruvia::http_status::no_content);
        (void)server.submit_response_head(
            1, resp, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, resp));
        (void)server.release(std::move(*req_head));
    }
}
