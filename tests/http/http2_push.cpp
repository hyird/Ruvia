#include <gtest/gtest.h>

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

#include "content_decoding_fixture.h"
#include "http2_connection.h"
#include "http_client_protocol.h"
#include "http_header.h"
#include "http_response.h"
#include "http_response_server.h"
namespace {

void submit_buffered_response(ruvia::http2_connection& server, std::uint32_t stream_id, const ruvia::http_response& response) {
    const auto head = server.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
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
                const auto parent = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
                EXPECT_TRUE(std::holds_alternative<std::uint32_t>(parent));
                relay.transfer(client, server);
                auto request = server.next_event();
                auto end = server.next_event();
                const std::array fields{ruvia::http_header_view{"x-owned", value}};
                const auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/owned", .headers_ = fields});
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
                EXPECT_TRUE(request.headers_.size() == 1 && request.headers_[0].name() == "x-owned" && request.headers_[0].value() == value);
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
        const auto parent = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "a"});
        const auto other = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "a"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(parent) && std::holds_alternative<std::uint32_t>(other));
        if (!std::holds_alternative<std::uint32_t>(parent) || !std::holds_alternative<std::uint32_t>(other)) {
            continue;
        }
        const auto parent_id = std::get<std::uint32_t>(parent);
        const auto other_id = std::get<std::uint32_t>(other);
        (void)ruvia::test::drain_output(client);
        const auto make_frame = [&](ruvia::http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload) {
            std::string frame(ruvia::http2_frame_header_bytes, '\0');
            EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(frame.data(), frame.size()), static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
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
        EXPECT_TRUE(reset && reset->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream) && reset->stream_id_ == parent_id);
        if (reset_wire.size() == 2 * (ruvia::http2_frame_header_bytes + 4)) {
            const auto cancel = std::string_view(reset_wire).substr(ruvia::http2_frame_header_bytes + 4);
            const auto promised = ruvia::parse_http2_frame_header(std::span<const char>(cancel.data(), cancel.size()));
            EXPECT_TRUE(promised && promised->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream) && promised->stream_id_ == 2u);
            EXPECT_EQ(static_cast<unsigned char>(cancel.back()), static_cast<unsigned char>(ruvia::http2_error_code::cancel));
        }
        EXPECT_TRUE(!client.wants_write());
        const auto promise_end = make_frame(ruvia::http2_frame_type::continuation, 4, parent_id, std::string_view(block).substr(split));
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
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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

    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/"});
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(frame.data(), ruvia::http2_frame_header_bytes), payload_len, ruvia::http2_frame_type::push_promise, 0x4 /* END_HEADERS */, 1 /* associated_stream_id */));

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
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(second_blocked) && std::get<ruvia::http2_response_head_submit_error>(second_blocked) == ruvia::http2_response_head_submit_error::peer_stream_limit_reached);
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
        const auto committed = server.submit_streaming_response_head(id, ruvia::http_response{}, with_trailers ? ruvia::http_response_trailer_intent::present : ruvia::http_response_trailer_intent::none);
        EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
        const auto second_blocked = server.submit_streaming_response_head(std::get<0>(second).stream_id(), ruvia::http_response{});
        EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(second_blocked) && std::get<ruvia::http2_response_head_submit_error>(second_blocked) == ruvia::http2_response_head_submit_error::peer_stream_limit_reached);
        const std::array fields_value{ruvia::http_header_view{"x-final", "done"}};
        const auto trailers = ruvia::validate_http_response_trailers(with_trailers ? std::span<const ruvia::http_header_view>(fields_value) : std::span<const ruvia::http_header_view>{});
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
            const auto committed = server.submit_streaming_response_head(std::get<0>(first).stream_id(), ruvia::http_response{});
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
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_head) && std::get<ruvia::http2_response_head_submit_error>(closed_head) == ruvia::http2_response_head_submit_error::closed);
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(partial_headers).first(9), static_cast<std::uint32_t>(req_block.size()), ruvia::http2_frame_type::headers, 0x1 /* END_STREAM only */, 1));
    partial_headers.append(req_block);
    EXPECT_TRUE(server.feed(partial_headers) == ruvia::http2_feed_result::accepted);
    const auto push_incomplete = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/a"});
    EXPECT_TRUE(push_incomplete.index() == 1 && std::get<1>(push_incomplete) == ruvia::http2_push_submit_error::invalid_state);
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
    const auto connect_id = connect_client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect_id));
    relay.transfer(connect_client, connect_server);
    auto connect_req = connect_server.next_event();
    EXPECT_TRUE(connect_req && connect_req->request_head());
    const auto push_on_connect = connect_server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/a"});
    EXPECT_TRUE(push_on_connect.index() == 1 && std::get<1>(push_on_connect) == ruvia::http2_push_submit_error::invalid_state);
    if (connect_req && connect_req->request_head()) {
        (void)connect_server.release(std::move(*connect_req->request_head()));
    }

    std::string push_block;
    ruvia::hpack_encoder::encode_header(push_block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(push_block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(push_block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(push_block, ":path", "/pushed");
    std::string bad_promise(ruvia::http2_frame_header_bytes + 4, '\0');
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(bad_promise).first(9), static_cast<std::uint32_t>(4 + push_block.size()), ruvia::http2_frame_type::push_promise, 0x4, 1));
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
    const auto parent_id = drain_client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/"});
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(drain_promise).first(9), static_cast<std::uint32_t>(4 + dyn_push_block.size()), ruvia::http2_frame_type::push_promise, 0x4, 1));
    drain_promise[12] = 2;
    drain_promise.append(dyn_push_block);
    EXPECT_TRUE(drain_client.feed(drain_promise) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!drain_client.next_event());
    EXPECT_TRUE(ruvia::test::drain_output(drain_client).empty());

    // Response on stream 1 referencing dynamic table index 62 (0xbe) decodes "x-dyn: kept".
    std::string resp_block("\x88\xbe", 2);
    std::string resp_frame(ruvia::http2_frame_header_bytes, '\0');
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(resp_frame).first(9), static_cast<std::uint32_t>(resp_block.size()), ruvia::http2_frame_type::headers, 0x5, 1));
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
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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
    const auto first = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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
        const auto sub = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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

    const auto parent = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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
        auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/asset"});
        EXPECT_TRUE(pushed.index() == 0);
        if (pushed.index() != 0) {
            return;
        }
        held_heads.push_back(std::move(std::get<0>(pushed)));
        (void)ruvia::test::drain_output(server);
    }

    const std::array<char, 17> goaway{0, 0, 8, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(server.feed(std::string_view(goaway.data(), goaway.size())) == ruvia::http2_feed_result::accepted);
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

    const auto connect_id = client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
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
    EXPECT_TRUE(server.submit_connect_response_head(1, response) == ruvia::http2_submit_status::accepted);
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(promise).first(9), static_cast<std::uint32_t>(4 + push_block.size()), ruvia::http2_frame_type::push_promise, 0x4, 1));
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
        EXPECT_TRUE(associated && associated->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
        EXPECT_EQ(associated->stream_id_, 1u);
        EXPECT_EQ(static_cast<unsigned char>(output[12]), static_cast<unsigned char>(ruvia::http2_error_code::protocol_error));
        const auto promised = ruvia::parse_http2_frame_header(std::span(output).subspan(ruvia::http2_frame_header_bytes + 4).first(9));
        EXPECT_TRUE(promised && promised->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
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

    const auto parent = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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
    EXPECT_TRUE(server.feed(std::string_view(goaway.data(), goaway.size())) == ruvia::http2_feed_result::accepted);
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

    const auto sub1 = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(wire.data(), wire.size()), static_cast<std::uint32_t>(payload.size()), ruvia::http2_frame_type::push_promise, 0x4, parent_id));
        wire.append(payload);
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        while (client.next_event()) {
        }
    }

    const auto sub2 = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(sub2));
}

TEST(Ruvia, http2_push_request_view_accepts_named_owners_and_copies_before_owner_mutation) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"})));
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(partial_frame).first(9), static_cast<std::uint32_t>(partial_block.size()), ruvia::http2_frame_type::headers, 0x1, 1));
    partial_frame.append(partial_block);
    EXPECT_TRUE(server.feed(partial_frame) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") == ruvia::http2_submit_status::invalid_state);
    std::array<char, 9> cont{};
    EXPECT_TRUE(ruvia::encode_http2_frame_header(cont, 0, ruvia::http2_frame_type::continuation, 0x4, 1));
    EXPECT_TRUE(server.feed(std::string_view(cont.data(), cont.size())) == ruvia::http2_feed_result::accepted);
    auto req1 = server.next_event();
    (void)server.next_event();
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") == ruvia::http2_submit_status::accepted);

    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/pushed"});
    EXPECT_TRUE(pushed.index() == 0);
    // Pushed stream in reserved (local) state cannot send ALTSVC before response headers open it.
    // An interim response is a response HEADERS frame too (RFC 9113 §8.4.2): the
    // stream is half-closed (remote) afterwards and the server may advertise on it.
    std::uint32_t pushed_id = 0;
    if (pushed.index() == 0) {
        pushed_id = std::get<0>(pushed).stream_id();
        EXPECT_TRUE(server.submit_alternative_service_advertisement(pushed_id, "", "h3=\":443\"") == ruvia::http2_submit_status::invalid_state);
        EXPECT_TRUE(server.submit_interim_response_head(pushed_id, ruvia::http_interim_response_head(ruvia::http_status::early_hints)) == ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.submit_alternative_service_advertisement(pushed_id, "", "h3=\":443\"") == ruvia::http2_submit_status::accepted);
        (void)server.release(std::move(std::get<0>(pushed)));
    }

    ruvia::http_response resp;
    resp.status(ruvia::http_status::no_content);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(server.submit_response_head(1, resp, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, resp))));
    // Stream 1 is now closed (retained only by req1 lease); ALTSVC submission must be rejected.
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") == ruvia::http2_submit_status::invalid_state);
    if (req1 && req1->request_head()) {
        (void)server.release(std::move(*req1->request_head()));
    }
    relay.transfer(server, client);
    bool pushed_altsvc_seen = false;
    while (auto event = client.next_event()) {
        if (const auto* advertisement = event->alternative_service_advertisement(); advertisement != nullptr && advertisement->stream_id_ == pushed_id) {
            pushed_altsvc_seen = advertisement->field_value_ == "h3=\":443\"";
        }
    }
    EXPECT_TRUE(pushed_altsvc_seen);
    EXPECT_TRUE(!client.connection_error());

    // 2. Open CONNECT tunnel ignores ALTSVC on stream (RFC 7838 section 4 / RFC 9113
    // section 5.5 discard) and rejects server submission.
    (void)client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    const auto connect_submitted = client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect_submitted));
    const auto connect_id = std::get<std::uint32_t>(connect_submitted);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> req3_lease;
    std::optional<ruvia::http2_request_head_event> connect_lease;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            if (rh->stream_id() == connect_id) {
                EXPECT_TRUE(server.submit_alternative_service_advertisement(connect_id, "", "h3=\":443\"") == ruvia::http2_submit_status::invalid_state);
                ruvia::http_response ok_resp;
                ok_resp.status(ruvia::http_status::ok);
                EXPECT_TRUE(server.submit_connect_response_head(connect_id, ok_resp) == ruvia::http2_submit_status::accepted);
                EXPECT_TRUE(server.submit_alternative_service_advertisement(connect_id, "", "h3=\":443\"") == ruvia::http2_submit_status::invalid_state);
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(altsvc_on_tunnel).first(9), 8, ruvia::http2_frame_type::alternative_service, 0, connect_id));
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
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(altsvc_on_reserved).first(9), 8, ruvia::http2_frame_type::alternative_service, 0, pushed2_id));
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
    auto client = ruvia::http2_connection::client({.enable_push_ = true, .receive_origin_advertisements_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto connect_submitted = client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
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
    EXPECT_TRUE(server.submit_connect_response_head(connect_id, ok_resp) == ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    while (client.next_event()) {
    }

    const auto altsvc_tunnel = std::get<0>(ruvia::encode_http2_alternative_service_frame(connect_id, "", "h3=\":443\"", 16384));
    const auto altsvc_idle = std::get<0>(ruvia::encode_http2_alternative_service_frame(99, "", "h3=\":443\"", 16384));
    EXPECT_TRUE(client.feed(std::string_view(altsvc_tunnel.data(), altsvc_tunnel.size())) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.next_event().has_value());
    EXPECT_TRUE(client.feed(std::string_view(altsvc_idle.data(), altsvc_idle.size())) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.next_event().has_value());
    EXPECT_TRUE(!client.connection_error().has_value());

    const auto get_submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(frame).first(ruvia::http2_frame_header_bytes), 6, ruvia::http2_frame_type::priority_update, 0, 0));
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(frame.data(), frame.size()), 0 /* length */, ruvia::http2_frame_type::priority_update, 0 /* flags */, 0 /* stream_id */));

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

    EXPECT_TRUE(client.submit_priority_update(1, {.urgency_ = 1, .incremental_ = true}) == ruvia::http2_submit_status::accepted);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/prio"});
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
        (void)server.submit_response_head(1, resp, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, resp));
        (void)server.release(std::move(*req_head));
    }
}

namespace {

using ruvia::http2_cleartext_preface_probe;
using ruvia::http2_client_preface;
using ruvia::probe_http2_cleartext_preface;

}  // namespace

TEST(Ruvia, http2_cleartext_preface_accepts_only_prior_knowledge) {
    EXPECT_TRUE(probe_http2_cleartext_preface("GET / HTTP/1.1\r\n") == http2_cleartext_preface_probe::http1);
    EXPECT_TRUE(probe_http2_cleartext_preface("") == http2_cleartext_preface_probe::http1);
    EXPECT_TRUE(probe_http2_cleartext_preface("PRI * HT") == http2_cleartext_preface_probe::need_more_preface);
    EXPECT_TRUE(probe_http2_cleartext_preface(http2_client_preface) == http2_cleartext_preface_probe::complete_preface);
    EXPECT_TRUE(probe_http2_cleartext_preface("PRI /not-http2\r\n") == http2_cleartext_preface_probe::drop_connection);
}

TEST(Ruvia, frame_public_codec_preserves_wire_bytes_and_buffer_boundaries) {
    std::array<char, 11> storage{};
    storage.fill('#');
    auto output = std::span(storage).subspan(1, ruvia::http2_frame_header_bytes);
    EXPECT_TRUE(ruvia::encode_http2_frame_header(output, 0xabcdef, static_cast<ruvia::http2_frame_type>(0xfe), 0x81, 0x7f123456));
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
        const auto result_value = ruvia::decode_http_origin_advertisement(std::span(std::get<0>(frame)).subspan(ruvia::http2_frame_header_bytes));
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
        const auto decoded = ruvia::decode_http2_alternative_service(stream_id, std::span(std::get<0>(result_value)).subspan(ruvia::http2_frame_header_bytes));
        ASSERT_EQ(decoded.index(), 0u);
        EXPECT_EQ(std::get<0>(decoded).origin_, origin);
        EXPECT_EQ(std::get<0>(decoded).field_value_, value);

        const auto limited = ruvia::encode_http2_alternative_service_frame(stream_id, origin, value, size - 1);
        ASSERT_NE(limited.index(), 0u);
        EXPECT_EQ(std::get<1>(limited), ruvia::http_connection_advertisement_error::limit);
    }
    const auto http2_limited = ruvia::encode_http2_origin_frame(origins, static_cast<std::uint32_t>(payload_bytes - 1));
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
    auto prepared = writer.prepare(ruvia::http_origin_view::https({.host_ = "example.test"}), {.method_ = "GET", .target_ = "/ws"}, request_buffer);
    ruvia::http1_client_response_parser parser(std::move(std::get<ruvia::prepared_http1_client_request>(prepared)).take_exchange_state());
    auto parsed = parser.parse("HTTP/1.1 200 OK\r\n" + std::string(fields) + "\r\n");
    if (parsed.parsed() == nullptr) {
        throw std::runtime_error("expected parsed response");
    }
    return std::move(*parsed.parsed()).take_head();
}

// The server's selection on an open Extended CONNECT stream.
std::variant<ruvia::websocket_client_negotiation_result_view, ruvia::websocket_client_handshake_error> open_stream_selection(const ruvia::websocket_client_negotiation& negotiation, const ruvia::http_client_response_head& head) {
    return ruvia::validate_extended_connect_websocket_client_response(negotiation, head, true);
}

template <typename head_type>
concept ws_client_negotiation_accepts_response_head = requires(const ruvia::websocket_client_negotiation& negotiation, head_type&& head) { ruvia::validate_extended_connect_websocket_client_response(negotiation, std::forward<head_type>(head), true); };

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
        const auto append_frame = [&](std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload) {
            std::array<char, ruvia::http2_frame_header_bytes> header{};
            EXPECT_TRUE(ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
            wire.append(header.data(), header.size());
            wire.append(payload);
        };
        std::string settings_wire;
        constexpr std::array<char, 6> enable_connect_protocol{0x00, 0x08, 0x00, 0x00, 0x00, 0x01};
        append_frame(settings_wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view(enable_connect_protocol.data(), enable_connect_protocol.size()));
        EXPECT_TRUE(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }

        ruvia::websocket_client_negotiation negotiation({});
        const auto submitted = ruvia::submit_http2_websocket_client_request(connection, negotiation, "https", "example.test", "/ws");
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
        EXPECT_TRUE(ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(enable_connect_protocol.size()), ruvia::http2_frame_type::settings, 0, 0));
        settings_wire.append(header.data(), header.size());
        settings_wire.append(enable_connect_protocol.data(), enable_connect_protocol.size());
        EXPECT_TRUE(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }
        ruvia::websocket_client_negotiation negotiation({});
        const std::array<ruvia::http_header_view, 1> borrowed_headers{{{"x-trace", "borrowed"}}};
        const auto submitted = ruvia::submit_http2_websocket_client_request(connection, negotiation, "https", "example.test", "/ws", borrowed_headers);
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
        EXPECT_TRUE(std::holds_alternative<ruvia::websocket_client_handshake_error>(res) && std::get<ruvia::websocket_client_handshake_error>(res) == ruvia::websocket_client_handshake_error::forbidden_field);
    }
}

// RFC 8441 section 5: END_STREAM after the 2xx is the server's orderly tunnel
// close; only END_STREAM on the response HEADERS leaves no WebSocket stream.
// The fact is fixed by the HEADERS frame even when the DATA that ends the
// stream arrives in the same feed().
TEST(Ruvia, websocket_h2_client_response_head_reports_end_stream_only_from_headers) {
    for (const bool end_on_headers : {false, true}) {
        auto connection = ruvia::http2_connection::client({});
        const auto append_frame = [&](std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload) {
            std::array<char, ruvia::http2_frame_header_bytes> header{};
            EXPECT_TRUE(ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
            wire.append(header.data(), header.size());
            wire.append(payload);
        };
        std::string settings_wire;
        constexpr std::array<char, 6> enable_connect_protocol{0x00, 0x08, 0x00, 0x00, 0x00, 0x01};
        append_frame(settings_wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view(enable_connect_protocol.data(), enable_connect_protocol.size()));
        EXPECT_TRUE(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }
        ruvia::websocket_client_negotiation negotiation({});
        const auto submitted = ruvia::submit_http2_websocket_client_request(connection, negotiation, "https", "example.test", "/ws");
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
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, stream_id,
                std::string_view("\x88\x05\x03\xe8"
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
        const auto negotiated = ruvia::validate_extended_connect_websocket_client_response(negotiation, head->head(), !head->stream_ended());
        if (end_on_headers) {
            EXPECT_TRUE(std::holds_alternative<ruvia::websocket_client_handshake_error>(negotiated) && std::get<ruvia::websocket_client_handshake_error>(negotiated) == ruvia::websocket_client_handshake_error::stream_closed);
        } else {
            EXPECT_TRUE(negotiated.index() == 0);
            auto data = connection.next_event();
            EXPECT_TRUE(data && data->tunnel_data() != nullptr);
            auto end = connection.next_event();
            EXPECT_TRUE(end && end->tunnel_end() != nullptr);
        }
    }
}
