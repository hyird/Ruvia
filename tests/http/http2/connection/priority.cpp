#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

#include "test_harness.h"
namespace {
void transfer(ruvia::http2_connection& from, ruvia::http2_connection& to) {
    const auto bytes_value = from.pending_output();
    const auto status = to.feed(bytes_value);
    if (status != ruvia::http2_feed_result::accepted && status != ruvia::http2_feed_result::need_input) {
        throw std::runtime_error("priority transfer failed");
    }
    (void)from.consume_output(bytes_value.size());
}
}  // namespace
RUVIA_TEST(http2_priority_update_client_submission_and_server_event) {
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    RUVIA_CHECK(client.submit_priority_update(1, {.urgency_ = 0, .incremental_ = true}) == ruvia::http2_submit_status::accepted);
    transfer(client, server);
    const auto event = server.next_event();
    RUVIA_CHECK(event && event->priority_update());
    RUVIA_CHECK_EQ(event->priority_update()->element_id_, 1u);
    RUVIA_CHECK(event->priority_update()->fields_.urgency_ == 0);
    RUVIA_CHECK(server.submit_priority_update(1, {}) == ruvia::http2_submit_status::invalid_state);
    RUVIA_CHECK(client.submit_priority_update(2, {}) == ruvia::http2_submit_status::invalid_state);
}
RUVIA_TEST(http2_priority_update_idle_stream_budget_and_wrong_peer_role) {
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    for (std::uint32_t i = 0; i < 128; ++i) {
        RUVIA_CHECK(client.submit_priority_update(i * 2 + 1, {.urgency_ = 3}) == ruvia::http2_submit_status::accepted);
        transfer(client, server);
        RUVIA_CHECK(server.next_event()->priority_update());
    }
    RUVIA_CHECK(client.submit_priority_update(257, {}) == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(server.feed(client.pending_output()) == ruvia::http2_feed_result::protocol_failure);
    RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::protocol_error);
}

RUVIA_TEST(http2_priority_update_ignores_malformed_values_without_consuming_idle_budget) {
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    for (std::uint32_t index = 0; index != 128; ++index) {
        RUVIA_CHECK(client.submit_priority_update(index * 2 + 1, {.urgency_ = 3}) == ruvia::http2_submit_status::accepted);
        transfer(client, server);
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
    transfer(client, server);
    const auto update = server.next_event();
    RUVIA_CHECK(update && update->priority_update() && update->priority_update()->fields_.urgency_ == 0);

    RUVIA_CHECK(client.submit_priority_update(257, {}) == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(server.feed(client.pending_output()) == ruvia::http2_feed_result::protocol_failure);
    RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::protocol_error);
}

RUVIA_TEST(http2_priority_update_rejects_invalid_ids_even_with_malformed_values) {
    for (const char id : {char{0}, char{2}}) {
        auto client = ruvia::http2_connection::client();
        auto server = ruvia::http2_connection::server();
        transfer(client, server);
        transfer(server, client);
        transfer(client, server);
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
    auto client = ruvia::http2_connection::client();
    auto server = ruvia::http2_connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);

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
