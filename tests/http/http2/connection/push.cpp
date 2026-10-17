#include <array>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <string>
#include <variant>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_response_stream.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {
void transfer(ruvia::http2_connection& from, ruvia::http2_connection& to) {
    const auto bytes_value = from.pending_output();
    const auto status = to.feed(bytes_value);
    if (status != ruvia::http2_feed_result::accepted && status != ruvia::http2_feed_result::need_input) {
        throw std::runtime_error("HTTP/2 push transfer failed, status " + std::to_string(static_cast<int>(status)) +
                                 ", error " + std::to_string(to.connection_error() ? static_cast<int>(*to.connection_error()) : -1));
    }
    (void)from.consume_output(bytes_value.size());
}
}  // namespace

RUVIA_TEST(http2_push_promise_fragmentation_response_and_detached_metadata) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    RUVIA_CHECK(submitted.submitted() != nullptr);
    transfer(client, server);
    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head());
    auto request_end = server.next_event();
    RUVIA_CHECK(request_end && request_end->message_end());
    const std::string value(20'000, 'x');
    const std::array fields_value{ruvia::http_header_view{"x-long", value}};
    const auto push = server.submit_push_promise(1, {.authority_ = "example.test", .path_ = "/asset", .headers_ = fields_value});
    RUVIA_CHECK((push.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(push), 2u);
    transfer(server, client);
    auto promise = client.next_event();
    RUVIA_CHECK(promise && promise->push_promise());
    RUVIA_CHECK(promise->push_promise()->request_.path_ == "/asset");
    RUVIA_CHECK(promise->push_promise()->request_.headers_[0].value() == value);
    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.body("asset");
    RUVIA_CHECK(server.submit_buffered_response(2, response) == ruvia::http2_submit_status::accepted);
    transfer(server, client);
    auto head = client.next_event();
    RUVIA_CHECK(head && head->response_head());
    RUVIA_CHECK_EQ(head->response_head()->stream_id(), 2u);
    auto data = client.next_event();
    RUVIA_CHECK(data && data->message_body_chunk());
    RUVIA_CHECK(data->message_body_chunk()->bytes() == "asset");
    auto credit = data->message_body_chunk()->take_credit();
    RUVIA_CHECK(client.acknowledge(std::move(credit)) == ruvia::http2_received_data_acknowledge_status::acknowledged);
    auto end = client.next_event();
    RUVIA_CHECK(end && end->message_end());
    RUVIA_CHECK(promise->push_promise()->request_.headers_[0].value() == value);
    RUVIA_CHECK(!client.connection_error());
}

RUVIA_TEST(http2_push_metadata_outlives_input_and_connection_for_complete_and_fragmented_blocks) {
    for (const std::size_t value_bytes : {std::size_t{1000}, std::size_t{20'000}}) {
        failing_memory_resource resource;
        {
            std::optional<ruvia::http2_event> promise;
            const std::string value(value_bytes, 'q');
            {
                auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
                auto server = ruvia::http2_connection::server();
                transfer(client, server);
                transfer(server, client);
                transfer(client, server);
                const auto parent = client.submit_request_head(
                    ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
                RUVIA_CHECK(parent.submitted());
                transfer(client, server);
                auto request = server.next_event();
                auto end = server.next_event();
                const std::array fields{ruvia::http_header_view{"x-owned", value}};
                const auto pushed = server.submit_push_promise(1,
                    {.authority_ = "example.test", .path_ = "/owned", .headers_ = fields});
                RUVIA_CHECK(pushed.index() == 0);
                std::string wire(server.pending_output());
                const auto first = ruvia::parse_http2_frame_header(std::span(wire.data(), wire.size()));
                RUVIA_CHECK(first && ((first->flags_ & 4U) != 0) == (value_bytes == 1000));
                RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
                auto received = client.next_event();
                RUVIA_CHECK(received && received->push_promise());
                if (received) {
                    promise.emplace(std::move(*received));
                }
                wire.assign(wire.size(), '\0');
            }
            RUVIA_CHECK(promise && promise->push_promise());
            if (promise && promise->push_promise()) {
                const auto& request = promise->push_promise()->request_;
                RUVIA_CHECK_EQ(request.method_, "GET");
                RUVIA_CHECK_EQ(request.scheme_, "https");
                RUVIA_CHECK_EQ(request.authority_, "example.test");
                RUVIA_CHECK_EQ(request.path_, "/owned");
                RUVIA_CHECK(request.headers_.size() == 1 &&
                            request.headers_[0].name() == "x-owned" && request.headers_[0].value() == value);
            }
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(http2_parent_reset_preserves_fragmented_push_and_hpack_state) {
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
        transfer(client, server);
        transfer(server, client);
        transfer(client, server);
        const auto parent = client.submit_request_head(
            ruvia::http2_regular_request_head_view{.authority_ = "a"});
        const auto other = client.submit_request_head(
            ruvia::http2_regular_request_head_view{.authority_ = "a"});
        RUVIA_CHECK(parent.submitted() && other.submitted());
        if (!parent.submitted() || !other.submitted()) {
            continue;
        }
        const auto parent_id = parent.submitted()->stream_id();
        const auto other_id = other.submitted()->stream_id();
        RUVIA_CHECK(client.consume_output(client.pending_output().size()) == ruvia::http2_output_consume_status::drained);
        const auto make_frame = [&](ruvia::http2_frame_type type, std::uint8_t flags,
                                    std::uint32_t stream_id, std::string_view payload) {
            std::string frame(ruvia::http2_frame_header_bytes, '\0');
            RUVIA_CHECK(ruvia::encode_http2_frame_header(std::span(frame.data(), frame.size()),
                static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
            frame.append(payload);
            return frame;
        };
        std::string payload("\0\0\0\2", 4);
        payload.append(block.data(), split);
        const auto promise_start = make_frame(ruvia::http2_frame_type::push_promise, 0, parent_id, payload);
        RUVIA_CHECK(client.feed(promise_start) == ruvia::http2_feed_result::accepted);
        RUVIA_CHECK(client.header_block_in_progress());
        RUVIA_CHECK(client.submit_reset(parent_id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        const auto reset_wire = client.pending_output();
        const auto reset = ruvia::parse_http2_frame_header(std::span(reset_wire.data(), reset_wire.size()));
        RUVIA_CHECK(reset && reset->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream) &&
                    reset->stream_id_ == parent_id);
        RUVIA_CHECK(client.consume_output(reset_wire.size()) == ruvia::http2_output_consume_status::drained);
        const auto promise_end = make_frame(ruvia::http2_frame_type::continuation, 4, parent_id,
            std::string_view(block).substr(split));
        RUVIA_CHECK(client.feed(promise_end) == ruvia::http2_feed_result::accepted);
        RUVIA_CHECK(!client.header_block_in_progress());
        auto promise = client.next_event();
        RUVIA_CHECK(promise && promise->push_promise());
        if (!promise || !promise->push_promise()) {
            continue;
        }
        const auto& pushed = *promise->push_promise();
        RUVIA_CHECK_EQ(pushed.associated_stream_id_, parent_id);
        RUVIA_CHECK_EQ(pushed.promised_stream_id_, 2u);
        RUVIA_CHECK_EQ(pushed.request_.path_, "/");
        RUVIA_CHECK(pushed.request_.headers_.size() == 1 &&
                    pushed.request_.headers_[0].name() == "x" && pushed.request_.headers_[0].value() == "y");
        promise.reset();
        RUVIA_CHECK(!client.next_event());
        const std::string pushed_block(1, static_cast<char>(0x88));
        const auto pushed_response = make_frame(ruvia::http2_frame_type::headers, 5, 2, pushed_block);
        RUVIA_CHECK(client.feed(pushed_response) == ruvia::http2_feed_result::accepted);
        auto pushed_head = client.next_event();
        RUVIA_CHECK(pushed_head && pushed_head->response_head() && pushed_head->response_head()->stream_id() == 2);
        auto pushed_end = client.next_event();
        RUVIA_CHECK(pushed_end && pushed_end->message_end());
        pushed_head.reset();
        pushed_end.reset();
        // Index 62 was introduced by the push, despite cancellation of its parent.
        const std::string indexed_block("\x88\xbe", 2);
        const auto other_response = make_frame(ruvia::http2_frame_type::headers, 5, other_id, indexed_block);
        RUVIA_CHECK(client.feed(other_response) == ruvia::http2_feed_result::accepted);
        auto other_head = client.next_event();
        RUVIA_CHECK(other_head && other_head->response_head() && other_head->response_head()->stream_id() == other_id);
        if (other_head && other_head->response_head()) {
            const auto fields = other_head->response_head()->head().headers();
            RUVIA_CHECK(fields.size() == 1 && fields[0].name() == "x" && fields[0].value() == "y");
        }
        auto other_end = client.next_event();
        RUVIA_CHECK(other_end && other_end->message_end());
        RUVIA_CHECK(!client.connection_error());
        RUVIA_CHECK(!client.wants_write());
    }
}

RUVIA_TEST(http2_push_disabled_peer_and_client_cancellation) {
    for (const bool enabled : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = enabled});
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        transfer(client, server);
        transfer(server, client);
        const auto submitted = client.submit_request_head(
            ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
        RUVIA_CHECK(submitted.submitted() != nullptr);
        transfer(client, server);
        auto request = server.next_event();
        auto end = server.next_event();
        const auto push = server.submit_push_promise(1, {.authority_ = "example.test"});
        if (!enabled) {
            RUVIA_CHECK((push.index() != 0) && std::get<1>(push) == ruvia::http2_push_submit_error::push_disabled);
            continue;
        }
        RUVIA_CHECK((push.index() == 0));
        transfer(server, client);
        RUVIA_CHECK(client.next_event()->push_promise());
        RUVIA_CHECK(client.submit_reset(std::get<0>(push), ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        transfer(client, server);
        auto closed = server.next_event();
        RUVIA_CHECK(closed && closed->stream_closed());
        RUVIA_CHECK_EQ(closed->stream_closed()->stream_id(), std::get<0>(push));
    }
}

RUVIA_TEST(http2_push_promise_unsafe_method_resets_promised_stream_without_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);

    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/"});
    RUVIA_CHECK(submitted.submitted() != nullptr);
    transfer(client, server);

    // Prepare HPACK block with unsafe method (POST)
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/upload");

    std::string frame;
    frame.resize(ruvia::http2_frame_header_bytes + 4 + block.size());
    const auto payload_len = static_cast<std::uint32_t>(4 + block.size());
    RUVIA_CHECK(ruvia::encode_http2_frame_header(
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
    RUVIA_CHECK(feed_status == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!client.connection_error());

    // Client should have emitted RST_STREAM on promised stream 2
    const auto pending = client.pending_output();
    RUVIA_CHECK(pending.size() >= ruvia::http2_frame_header_bytes + 4);
    const auto parsed_value = ruvia::parse_http2_frame_header(std::span(pending.data(), pending.size()));
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK_EQ(parsed_value->type_, static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
    RUVIA_CHECK_EQ(parsed_value->stream_id_, 2u);
    RUVIA_CHECK_EQ(parsed_value->length_, 4u);
}

RUVIA_TEST(http2_push_reservation_does_not_consume_peer_concurrency_until_response_headers) {
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    RUVIA_CHECK(submitted.submitted());
    transfer(client, server);
    auto head = server.next_event();
    auto end = server.next_event();
    const auto set_limit = [&](std::uint32_t limit) {
        std::array<char, 15> frame{};
        RUVIA_CHECK(ruvia::encode_http2_frame_header(std::span(frame).first(9), 6, ruvia::http2_frame_type::settings, 0, 0));
        frame[10] = 3;
        for (std::size_t i = 0; i < 4; ++i) {
            frame[11 + i] = static_cast<char>(limit >> (24 - i * 8));
        }
        RUVIA_CHECK(server.feed(std::string_view(frame.data(), frame.size())) == ruvia::http2_feed_result::accepted);
        (void)server.consume_output(server.pending_output().size());
    };
    set_limit(0);
    const auto first = server.submit_push_promise(1, {.authority_ = "example.test", .path_ = "/one"});
    const auto second = server.submit_push_promise(1, {.authority_ = "example.test", .path_ = "/two"});
    RUVIA_CHECK((first.index() == 0) && (second.index() == 0));
    if ((first.index() != 0) || (second.index() != 0)) {
        return;
    }
    RUVIA_CHECK(server.submit_streaming_response_head(std::get<0>(first), ruvia::http_response{}) == ruvia::http2_submit_status::peer_capability_unavailable);
    set_limit(1);
    RUVIA_CHECK(server.submit_streaming_response_head(std::get<0>(first), ruvia::http_response{}) == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(server.submit_streaming_response_head(std::get<0>(second), ruvia::http_response{}) == ruvia::http2_submit_status::peer_capability_unavailable);
    RUVIA_CHECK(server.submit_data(std::get<0>(first), {}, ruvia::http2_end_stream::end_stream) == ruvia::http2_data_submit_status::accepted);
    RUVIA_CHECK(server.submit_streaming_response_head(std::get<0>(second), ruvia::http_response{}) == ruvia::http2_submit_status::accepted);
}

RUVIA_TEST(http2_push_response_events_survive_a_reset_in_the_same_input_batch) {
    failing_memory_resource resource;
    {
        auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        transfer(client, server);
        transfer(server, client);
        transfer(client, server);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
        RUVIA_CHECK(submitted.submitted() != nullptr);
        transfer(client, server);
        auto request = server.next_event();
        auto request_end = server.next_event();
        const auto push = server.submit_push_promise(1, {.authority_ = "example.test", .path_ = "/reset"});
        RUVIA_CHECK(push.index() == 0);
        if (push.index() != 0) {
            return;
        }
        const auto id = std::get<0>(push);
        RUVIA_CHECK(server.submit_streaming_response_head(id, ruvia::http_response{}) == ruvia::http2_submit_status::accepted);
        RUVIA_CHECK(server.submit_reset(id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        transfer(server, client);
        auto promise = client.next_event();
        auto head = client.next_event();
        auto closed = client.next_event();
        RUVIA_CHECK(promise && promise->push_promise());
        RUVIA_CHECK(head && head->response_head() && head->response_head()->stream_id() == id);
        RUVIA_CHECK(closed && closed->stream_closed() && closed->stream_closed()->stream_id() == id);
        RUVIA_CHECK(!client.next_event());
        if (promise && promise->push_promise()) {
            RUVIA_CHECK_EQ(promise->push_promise()->request_.path_, "/reset");
        }
        ruvia::http_response response({.resource_ = &resource});
        response.body("parent");
        RUVIA_CHECK(server.submit_buffered_response(1, response) == ruvia::http2_submit_status::accepted);
        transfer(server, client);
        auto parent_head = client.next_event();
        auto body = client.next_event();
        RUVIA_CHECK(parent_head && parent_head->response_head() && parent_head->response_head()->stream_id() == 1);
        RUVIA_CHECK(body && body->message_body_chunk() && body->message_body_chunk()->bytes() == "parent");
        if (body && body->message_body_chunk()) {
            RUVIA_CHECK(client.acknowledge(body->message_body_chunk()->take_credit()) == ruvia::http2_received_data_acknowledge_status::acknowledged);
        }
        auto end = client.next_event();
        RUVIA_CHECK(end && end->message_end());
        RUVIA_CHECK(!client.connection_error());
        RUVIA_CHECK(!server.connection_error());
        RUVIA_CHECK(request && request->request_head());
        if (request && request->request_head()) {
            RUVIA_CHECK(server.release(std::move(*request->request_head())) == ruvia::http2_server_request_release_status::released);
        }
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http2_push_allocation_failure_preserves_the_parent_and_is_retryable) {
    bool completed = false;
    for (std::size_t failure_index = 0; failure_index < 32 && !completed; ++failure_index) {
        failing_memory_resource resource;
        {
            auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
            auto server = ruvia::http2_connection::server();
            transfer(client, server);
            transfer(server, client);
            transfer(client, server);
            const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
            RUVIA_CHECK(submitted.submitted() != nullptr);
            transfer(client, server);
            auto request = server.next_event();
            auto request_end = server.next_event();
            const std::string path = "/" + std::string(128, 'p');
            const auto push = server.submit_push_promise(1, {.authority_ = "example.test", .path_ = path});
            RUVIA_CHECK(push.index() == 0);
            if (push.index() != 0) {
                return;
            }
            const auto id = std::get<0>(push);
            const std::string wire(server.pending_output());
            (void)server.consume_output(wire.size());
            resource.fail_after(failure_index);
            bool failed = false;
            try {
                RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            resource.allow_allocations();
            if (failed) {
                RUVIA_CHECK(!client.next_event());
                RUVIA_CHECK(!client.connection_error());
                RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
            } else {
                completed = true;
            }
            auto promise = client.next_event();
            RUVIA_CHECK(promise && promise->push_promise());
            if (promise && promise->push_promise()) {
                RUVIA_CHECK_EQ(std::string_view(promise->push_promise()->request_.path_), std::string_view(path));
            }
            RUVIA_CHECK(server.submit_streaming_response_head(id, ruvia::http_response{}) == ruvia::http2_submit_status::accepted);
            RUVIA_CHECK(server.submit_reset(id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
            transfer(server, client);
            auto head = client.next_event();
            auto closed = client.next_event();
            RUVIA_CHECK(head && head->response_head() && head->response_head()->stream_id() == id);
            RUVIA_CHECK(closed && closed->stream_closed() && closed->stream_closed()->stream_id() == id);
            RUVIA_CHECK(server.submit_buffered_response(1, ruvia::http_response{}) == ruvia::http2_submit_status::accepted);
            transfer(server, client);
            auto parent_head = client.next_event();
            auto parent_end = client.next_event();
            RUVIA_CHECK(parent_head && parent_head->response_head() && parent_head->response_head()->stream_id() == 1);
            RUVIA_CHECK(parent_end && parent_end->message_end() && parent_end->message_end()->stream_id() == 1);
            RUVIA_CHECK(!client.connection_error());
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK(completed);
}

RUVIA_TEST(http2_push_finish_releases_peer_concurrency_while_its_request_lease_is_retained) {
    for (const bool with_trailers : {false, true}) {
        auto client = ruvia::http2_connection::client({.enable_push_ = true});
        auto server = ruvia::http2_connection::server();
        transfer(client, server);
        transfer(server, client);
        transfer(client, server);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
        RUVIA_CHECK(submitted.submitted() != nullptr);
        transfer(client, server);
        auto request = server.next_event();
        auto request_end = server.next_event();
        std::array<char, 15> settings{};
        RUVIA_CHECK(ruvia::encode_http2_frame_header(std::span(settings).first(9), 6, ruvia::http2_frame_type::settings, 0, 0));
        settings[10] = 3;
        settings[14] = 1;
        RUVIA_CHECK(server.feed(std::string_view(settings.data(), settings.size())) == ruvia::http2_feed_result::accepted);
        auto first = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/one"});
        const auto second = server.submit_push_promise(1, {.authority_ = "example.test", .path_ = "/two"});
        RUVIA_CHECK(first.index() == 0 && second.index() == 0);
        if (first.index() != 0 || second.index() != 0) {
            continue;
        }
        const auto id = std::get<0>(first).stream_id();
        const auto committed = server.submit_streaming_response_head(id, ruvia::http_response{},
            ruvia::http_response_stream_kind::generic,
            with_trailers ? ruvia::http_response_trailer_intent::present : ruvia::http_response_trailer_intent::none);
        RUVIA_CHECK(committed.submitted() != nullptr);
        RUVIA_CHECK(server.submit_streaming_response_head(std::get<0>(second), ruvia::http_response{}) == ruvia::http2_submit_status::peer_capability_unavailable);
        const std::array fields_value{ruvia::http_header_view{"x-final", "done"}};
        const auto trailers = ruvia::validate_http_response_trailers(
            with_trailers ? std::span<const ruvia::http_header_view>(fields_value) : std::span<const ruvia::http_header_view>{});
        RUVIA_CHECK(server.finish_response(id, trailers) == ruvia::http2_finish_response_status::accepted);
        RUVIA_CHECK(server.submit_streaming_response_head(std::get<0>(second), ruvia::http_response{}) == ruvia::http2_submit_status::accepted);
        RUVIA_CHECK_EQ(std::get<0>(first).request().path(), "/one");
        RUVIA_CHECK(server.release(std::move(std::get<0>(first))) == ruvia::http2_server_request_release_status::released);
    }
}

RUVIA_TEST(http2_server_goaway_releases_unprocessed_local_push_streams) {
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server();
    transfer(client, server);
    transfer(server, client);
    transfer(client, server);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    RUVIA_CHECK(submitted.submitted());
    transfer(client, server);
    auto head = server.next_event();
    auto end = server.next_event();
    const auto push = server.submit_push_promise(1, {.authority_ = "example.test"});
    RUVIA_CHECK((push.index() == 0));
    const std::array<char, 17> goaway{0, 0, 8, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    RUVIA_CHECK(server.feed(std::string_view(goaway.data(), goaway.size())) == ruvia::http2_feed_result::accepted);
    auto draining = server.next_event();
    auto closed = server.next_event();
    RUVIA_CHECK(draining && draining->goaway());
    RUVIA_CHECK(closed && closed->stream_closed() && closed->stream_closed()->stream_id() == std::get<0>(push));
    RUVIA_CHECK(server.submit_streaming_response_head(std::get<0>(push), ruvia::http_response{}) == ruvia::http2_submit_status::closed);
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
RUVIA_TEST(http2_push_repeated_response_and_cancellation_preserve_promises) {
    push_resource resource;
    {
        auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        transfer(client, server);
        transfer(server, client);
        transfer(client, server);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
        RUVIA_CHECK(submitted.submitted());
        transfer(client, server);
        auto request = server.next_event();
        auto request_end = server.next_event();
        const auto first = server.submit_push_promise(1, {.authority_ = "example.test", .path_ = "/retained"});
        RUVIA_CHECK(first.index() == 0);
        transfer(server, client);
        auto retained = client.next_event();
        RUVIA_CHECK(retained && retained->push_promise());
        RUVIA_CHECK(client.submit_reset(std::get<0>(first), ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        transfer(client, server);
        auto closed = server.next_event();
        closed.reset();
        for (std::size_t i = 0; i < 32; ++i) {
            {
                const auto push = server.submit_push_promise(1, {.authority_ = "example.test", .path_ = "/asset"});
                RUVIA_CHECK(push.index() == 0);
                transfer(server, client);
                auto promise = client.next_event();
                RUVIA_CHECK(promise && promise->push_promise());
                if (i % 2 == 0) {
                    RUVIA_CHECK(client.submit_reset(std::get<0>(push), ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
                    transfer(client, server);
                    auto canceled = server.next_event();
                    RUVIA_CHECK(canceled && canceled->stream_closed());
                } else {
                    ruvia::http_response response({.resource_ = &resource});
                    response.body("asset");
                    RUVIA_CHECK(server.submit_buffered_response(std::get<0>(push), response) == ruvia::http2_submit_status::accepted);
                    transfer(server, client);
                    auto head = client.next_event();
                    auto data = client.next_event();
                    RUVIA_CHECK(data && data->message_body_chunk());
                    if (data && data->message_body_chunk()) {
                        auto credit = data->message_body_chunk()->take_credit();
                        RUVIA_CHECK(client.acknowledge(std::move(credit)) == ruvia::http2_received_data_acknowledge_status::acknowledged);
                    }
                    auto end = client.next_event();
                    RUVIA_CHECK(head && head->response_head());
                    RUVIA_CHECK(end && end->message_end());
                }
            }
            RUVIA_CHECK(!client.next_event());
            RUVIA_CHECK(!server.next_event());

            RUVIA_CHECK(retained->push_promise()->request_.path_ == "/retained");
        }
        std::string request_path(16 * 1024, 'p');
        request_path.front() = '/';
        resource.reject_at_least_ = request_path.size();
        const auto pending_output = std::string(server.pending_output());
        bool threw = false;
        try {
            (void)server.submit_push_promise(1, {.authority_ = "example.test", .path_ = request_path});
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(std::string(server.pending_output()), pending_output);
        RUVIA_CHECK_EQ(retained->push_promise()->request_.path_, "/retained");

        resource.reject_at_least_ = 0;
        const auto retry = server.submit_push_promise(1, {.authority_ = "example.test", .path_ = request_path});
        RUVIA_CHECK(retry.index() == 0);
        transfer(server, client);
        auto retried_promise = client.next_event();
        RUVIA_CHECK(retried_promise && retried_promise->push_promise());
        if (retried_promise && retried_promise->push_promise()) {
            const auto& retry_path = retried_promise->push_promise()->request_.path_;
            RUVIA_CHECK(std::string_view(retry_path.data(), retry_path.size()) == request_path);
        }
    }
}
