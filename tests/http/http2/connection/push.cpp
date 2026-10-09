#include <array>
#include <cstring>
#include <memory_resource>
#include <string>
#include <variant>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

#include "http2/http2_frame_codec.h"
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
    std::size_t live_bytes_{0};
    std::size_t live_allocations_{0};
    std::size_t reject_at_least_{0};
    std::size_t rejected_allocations_{0};
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_at_least_ != 0 && bytes_value >= reject_at_least_) {
            ++rejected_allocations_;
            throw std::bad_alloc();
        }
        auto* pointer = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        live_bytes_ += bytes_value;
        ++live_allocations_;
        return pointer;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        if (live_allocations_ == 0 || live_bytes_ < bytes_value) {
            std::terminate();
        }
        live_bytes_ -= bytes_value;
        --live_allocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace
RUVIA_TEST(http2_push_repeated_response_and_cancellation_release_storage_and_preserve_promises) {
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
        std::size_t baseline_bytes = 0;
        std::size_t baseline_allocations = 0;
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
            // Drain consumed event storage before comparing allocations.
            RUVIA_CHECK(!client.next_event());
            RUVIA_CHECK(!server.next_event());
            if (i == 3) {
                // Retained connection/container storage (including implementation-specific
                // sentinels) is the stable baseline; per-push storage must return to it.
                baseline_bytes = resource.live_bytes_;
                baseline_allocations = resource.live_allocations_;
            }
            if (i > 3) {
                RUVIA_CHECK_EQ(resource.live_bytes_, baseline_bytes);
                RUVIA_CHECK_EQ(resource.live_allocations_, baseline_allocations);
            }
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
        RUVIA_CHECK_EQ(resource.rejected_allocations_, 1u);
        RUVIA_CHECK_EQ(std::string(server.pending_output()), pending_output);
        RUVIA_CHECK_EQ(resource.live_bytes_, baseline_bytes);
        RUVIA_CHECK_EQ(resource.live_allocations_, baseline_allocations);
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
    RUVIA_CHECK_EQ(resource.live_bytes_, 0u);
    RUVIA_CHECK_EQ(resource.live_allocations_, 0u);
}
