#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_response_stream.h"

#include "failing_memory_resource.h"
#include "output_drain.h"

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
    ruvia::testing::output_relay relay;
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
    ruvia::http_response response(&resource);
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
    auto credit = data->message_body_chunk()->take_credit();
    EXPECT_TRUE(client.acknowledge(std::move(credit)) == ruvia::http2_received_data_acknowledge_status::acknowledged);
    auto end = client.next_event();
    EXPECT_TRUE(end && end->message_end());
    EXPECT_TRUE(promise->push_promise()->request_.headers_[0].value() == value);
    EXPECT_TRUE(!client.connection_error());
}

TEST(Ruvia, http2_push_metadata_outlives_input_and_connection_for_complete_and_fragmented_blocks) {
    ruvia::testing::output_relay relay;
    for (const std::size_t value_bytes : {std::size_t{1000}, std::size_t{20'000}}) {
        failing_memory_resource resource;
        {
            std::optional<ruvia::http2_event> promise;
            const std::string value(value_bytes, 'q');
            {
                auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
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
                std::string wire = ruvia::testing::drain_output(server);
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
        EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    }
}

TEST(Ruvia, http2_parent_reset_cancels_fragmented_push_and_keeps_hpack_state) {
    ruvia::testing::output_relay relay;
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
        (void)ruvia::testing::drain_output(client);
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
        const auto reset_wire = ruvia::testing::drain_output(client);
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
    ruvia::testing::output_relay relay;
    for (const bool enabled : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = enabled});
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
    ruvia::testing::output_relay relay;
    std::pmr::monotonic_buffer_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    relay.transfer(client, server);

    // Prepare HPACK block with unsafe method (POST)
    std::pmr::string block(&resource);
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
    const auto pending = ruvia::testing::drain_output(client);
    EXPECT_TRUE(pending.size() >= ruvia::http2_frame_header_bytes + 4);
    const auto parsed_value = ruvia::parse_http2_frame_header(std::span(pending.data(), pending.size()));
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_EQ(parsed_value->type_, static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
    EXPECT_EQ(parsed_value->stream_id_, 2u);
    EXPECT_EQ(parsed_value->length_, 4u);
}

TEST(Ruvia, http2_push_reservation_does_not_consume_peer_concurrency_until_response_headers) {
    ruvia::testing::output_relay relay;
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
        (void)ruvia::testing::drain_output(server);
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
    ruvia::testing::output_relay relay;
    failing_memory_resource resource;
    {
        auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
        ruvia::http_response response(&resource);
        response.body("parent");
        submit_buffered_response(server, 1, response);
        relay.transfer(server, client);
        auto parent_head = client.next_event();
        auto body = client.next_event();
        EXPECT_TRUE(parent_head && parent_head->response_head() && parent_head->response_head()->stream_id() == 1);
        EXPECT_TRUE(body && body->message_body_chunk() && body->message_body_chunk()->bytes() == "parent");
        if (body && body->message_body_chunk()) {
            EXPECT_TRUE(client.acknowledge(body->message_body_chunk()->take_credit()) == ruvia::http2_received_data_acknowledge_status::acknowledged);
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
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, http2_push_finish_releases_peer_concurrency_while_its_request_lease_is_retained) {
    ruvia::testing::output_relay relay;
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
    ruvia::testing::output_relay relay;
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
        (void)ruvia::testing::drain_output(server);
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
            (void)ruvia::testing::drain_output(server);
        }
        const auto refused = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/two"});
        EXPECT_TRUE(refused.index() == 1 && std::get<1>(refused) == ruvia::http2_push_submit_error::stream_limit);
        EXPECT_TRUE(ruvia::testing::drain_output(server).empty());
        EXPECT_TRUE(!server.connection_error().has_value());
        if (opened) {
            (void)server.release(std::move(*opened));
        }
    }
}

TEST(Ruvia, http2_server_goaway_releases_unprocessed_local_push_streams) {
    ruvia::testing::output_relay relay;
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

namespace {
struct push_resource final : std::pmr::memory_resource {
    std::size_t reject_at_least_{0};
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_at_least_ != 0 && bytes_value >= reject_at_least_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace
TEST(Ruvia, http2_push_repeated_response_and_cancellation_preserve_promises) {
    ruvia::testing::output_relay relay;
    push_resource resource;
    {
        auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
                    ruvia::http_response response(&resource);
                    response.body("asset");
                    submit_buffered_response(server, std::get<0>(push).stream_id(), response);
                    relay.transfer(server, client);
                    auto head = client.next_event();
                    auto data = client.next_event();
                    EXPECT_TRUE(data && data->message_body_chunk());
                    if (data && data->message_body_chunk()) {
                        auto credit = data->message_body_chunk()->take_credit();
                        EXPECT_TRUE(client.acknowledge(std::move(credit)) == ruvia::http2_received_data_acknowledge_status::acknowledged);
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
        resource.reject_at_least_ = request_path.size();
        EXPECT_TRUE(!server.wants_write());
        bool threw = false;
        try {
            (void)server.submit_push_request(1, {.authority_ = "example.test", .path_ = request_path});
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
        EXPECT_TRUE(!server.wants_write());
        EXPECT_EQ(retained->push_promise()->request_.path_, "/retained");

        resource.reject_at_least_ = 0;
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
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    // 1. Server rejects submit_push_request while parent HEADERS is awaiting CONTINUATION.
    std::pmr::string req_block(&resource);
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
    auto connect_client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto connect_server = ruvia::http2_connection::server({.resource_ = &resource});
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

    std::pmr::string push_block(&resource);
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
    auto drain_client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto drain_server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(drain_client, drain_server);
    relay.transfer(drain_server, drain_client);
    relay.transfer(drain_client, drain_server);
    const auto parent_id = drain_client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(parent_id));
    relay.transfer(drain_client, drain_server);
    drain_client.begin_drain();
    (void)ruvia::testing::drain_output(drain_client);

    std::pmr::string dyn_push_block(&resource);
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
    EXPECT_TRUE(ruvia::testing::drain_output(drain_client).empty());

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
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
    ruvia::http_response response(&resource);
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
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
    (void)ruvia::testing::drain_output(server);

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
    EXPECT_TRUE(ruvia::testing::drain_output(server).empty());
    (void)server.release(std::move(std::get<0>(pushed)));
    for (auto& head : held_heads) {
        (void)server.release(std::move(head));
    }
}

TEST(Ruvia, http2_goaway_closes_more_reserved_pushes_than_the_concurrency_limit) {
    // Reserved push promises do not occupy a concurrency slot, so a GOAWAY
    // boundary can cover more live streams than SETTINGS_MAX_CONCURRENT_STREAMS.
    // Closing them must stay inside the stream table's capacity.
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
        (void)ruvia::testing::drain_output(server);
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
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
    ruvia::http_response response(&resource);
    response.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(1, response) ==
                ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    while (client.next_event()) {
    }
    EXPECT_TRUE(!client.connection_error());

    std::pmr::string push_block(&resource);
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
    const auto output = ruvia::testing::drain_output(client);
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
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
        (void)ruvia::testing::drain_output(server);
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
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto sub1 = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(sub1));
    const auto parent_id = std::get<std::uint32_t>(sub1);
    (void)ruvia::testing::drain_output(client);

    std::pmr::string pblock(&resource);
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
    ruvia::testing::output_relay relay;
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
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
    std::pmr::string scheme("https", &resource);
    std::string authority("example.test");
    std::pmr::string path("/owned-asset", &resource);
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
