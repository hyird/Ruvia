#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

#include "output_drain.h"
TEST(Ruvia, http2_priority_update_client_submission_and_server_event) {
    ruvia::testing::output_relay relay;
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
    ruvia::testing::output_relay relay;
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
    ruvia::testing::output_relay relay;
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
    ruvia::testing::output_relay relay;
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
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
        ruvia::http_response resp(&resource);
        resp.status(ruvia::http_status::no_content);
        (void)server.submit_response_head(
            1, resp, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, resp));
        (void)server.release(std::move(*req_head));
    }
}
