#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"

#include "test_harness.h"

namespace {

class accounting_allocation_resource final : public std::pmr::memory_resource {
public:
    explicit accounting_allocation_resource(
        std::size_t fail_at = (std::numeric_limits<std::size_t>::max)()) noexcept
        : fail_at_(fail_at) {}

    [[nodiscard]] std::size_t failure_points() const noexcept {
        return failure_points_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        // Tiny STL debug proxies may allocate in noexcept constructors.
        if (bytes_value >= 32 && failure_points_++ == fail_at_) {
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
    std::size_t fail_at_;
    std::size_t failure_points_{};
};

class toggle_allocation_resource final : public std::pmr::memory_resource {
public:
    void reject(bool value = true) noexcept {
        reject_ = value;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    bool reject_{false};
};

void append_frame(std::pmr::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload_value) {
    std::array<char, ruvia::http2_frame_header_bytes> header_value{};
    if (!ruvia::encode_http2_frame_header(
            header_value, static_cast<std::uint32_t>(payload_value.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header_value.data(), header_value.size());
    wire.append(payload_value);
}

void append_peer_settings(std::pmr::string& wire) {
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
}

void append_response(std::pmr::string& wire, std::pmr::memory_resource* resource,
    std::uint32_t stream_id, std::string_view status, std::string_view body = {},
    std::string_view trailer_name = {}, std::string_view trailer_value = {}) {
    std::pmr::string block(resource);
    ruvia::hpack_encoder::encode_header(block, ":status", status);
    const auto head_flags = static_cast<std::uint8_t>(0x4 | (body.empty() && trailer_name.empty() ? 0x1 : 0));
    append_frame(wire, ruvia::http2_frame_type::headers, head_flags, stream_id, block);
    if (!body.empty()) {
        append_frame(wire, ruvia::http2_frame_type::data,
            static_cast<std::uint8_t>(trailer_name.empty() ? 0x1 : 0), stream_id, body);
    }
    if (!trailer_name.empty()) {
        block.clear();
        ruvia::hpack_encoder::encode_header(block, trailer_name, trailer_value);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, stream_id, block);
    }
}

std::pmr::string client_response_wire(std::pmr::memory_resource* resource, std::string_view body,
    bool include_header = false, bool end_stream = true) {
    std::pmr::string block(resource);
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
    if (include_header) {
        ruvia::hpack_encoder::encode_header(block, "x-test", "value");
    }

    std::pmr::string wire(resource);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    append_frame(wire, ruvia::http2_frame_type::data, end_stream ? 0x1 : 0, 1, body);
    return wire;
}

std::pmr::string client_response_with_trailers_wire(std::pmr::memory_resource* resource) {
    std::pmr::string wire(resource);
    append_peer_settings(wire);
    std::pmr::string block(resource);
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
    ruvia::hpack_encoder::encode_header(block, "x-test", "owned");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    block.clear();
    ruvia::hpack_encoder::encode_header(block, "x-trace", "done");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    return wire;
}

std::pmr::string server_request_wire(std::pmr::memory_resource* resource, std::string_view body,
    std::string_view method = "POST", std::string_view expectation = {},
    bool end_stream = true) {
    std::pmr::string block(resource);
    ruvia::hpack_encoder::encode_header(block, ":method", method);
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", body.empty() ? "0" : "1");
    if (!expectation.empty()) {
        ruvia::hpack_encoder::encode_header(block, "expect", expectation);
    }

    std::pmr::string wire(ruvia::http2_client_preface, resource);
    append_peer_settings(wire);
    const auto head_flags = static_cast<std::uint8_t>(0x4 | (end_stream && body.empty() ? 0x1 : 0));
    append_frame(wire, ruvia::http2_frame_type::headers, head_flags, 1, block);
    if (!body.empty()) {
        append_frame(wire, ruvia::http2_frame_type::data,
            end_stream ? 0x1 : 0, 1, body);
    }
    return wire;
}

std::pmr::string server_request_with_trailers_wire(std::pmr::memory_resource* resource) {
    std::pmr::string block(resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", "1");

    std::pmr::string wire(ruvia::http2_client_preface, resource);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    append_frame(wire, ruvia::http2_frame_type::data, 0, 1, "x");
    block.clear();
    ruvia::hpack_encoder::encode_header(block, "x-request-trace", "done");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    return wire;
}

ruvia::http2_connection prepared_client(std::pmr::memory_resource* resource) {
    auto client = ruvia::http2_connection::client({.resource_ = resource});
    (void)client.consume_output(client.pending_output().size());
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
    if (submitted.submitted() == nullptr) {
        throw std::logic_error("test request was not submitted");
    }
    (void)client.consume_output(client.pending_output().size());
    return client;
}

ruvia::http2_connection prepared_client_method(std::pmr::memory_resource* resource,
    std::string_view method) {
    auto client = ruvia::http2_connection::client({.resource_ = resource});
    (void)client.consume_output(client.pending_output().size());
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = method, .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
    if (submitted.submitted() == nullptr) {
        throw std::logic_error("test request was not submitted");
    }
    (void)client.consume_output(client.pending_output().size());
    return client;
}

}  // namespace

RUVIA_TEST(http2_public_request_submission_exposes_one_exclusive_success_or_failure_contract) {
    auto client = ruvia::http2_connection::client();
    const auto invalid = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "", .authority_ = "example.test"});
    RUVIA_CHECK(invalid.submitted() == nullptr);
    RUVIA_CHECK(invalid.failure() != nullptr);
    if (const auto* failure = invalid.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::http2_request_head_submit_error::invalid_message);
    }
    const auto extended = client.submit_request_head(ruvia::http2_extended_connect_request_head_view{
        .protocol_ = "websocket", .authority_ = "example.test"});
    RUVIA_CHECK(extended.submitted() == nullptr);
    RUVIA_CHECK(extended.failure() != nullptr);
    if (const auto* failure = extended.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::http2_request_head_submit_error::peer_capability_unavailable);
    }
    const auto regular = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .authority_ = "example.test"});
    RUVIA_CHECK(regular.failure() == nullptr);
    RUVIA_CHECK(regular.submitted() != nullptr);
    if (const auto* submitted = regular.submitted()) {
        RUVIA_CHECK_EQ(submitted->stream_id(), std::uint32_t{1});
    }
    const auto connect = client.submit_request_head(ruvia::http2_connect_request_head_view{
        .authority_ = "example.test:443"});
    RUVIA_CHECK(connect.failure() == nullptr);
    RUVIA_CHECK(connect.submitted() != nullptr);
    if (const auto* submitted = connect.submitted()) {
        RUVIA_CHECK_EQ(submitted->stream_id(), std::uint32_t{3});
    }
    auto server = ruvia::http2_connection::server();
    const auto wrong_role = server.submit_request_head(ruvia::http2_regular_request_head_view{
        .authority_ = "example.test"});
    RUVIA_CHECK(wrong_role.submitted() == nullptr);
    RUVIA_CHECK(wrong_role.failure() != nullptr);
    if (const auto* failure = wrong_role.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::http2_request_head_submit_error::invalid_state);
    }
}

RUVIA_TEST(http2_public_construction_propagates_allocation_failure) {
    for (const auto role : {ruvia::http2_role::client, ruvia::http2_role::server}) {
        accounting_allocation_resource baseline;
        {
            auto connection = role == ruvia::http2_role::client
                                  ? ruvia::http2_connection::client({.resource_ = &baseline})
                                  : ruvia::http2_connection::server({.resource_ = &baseline});
            RUVIA_CHECK(connection.wants_write());
        }
        for (std::size_t fail_at = 0; fail_at < baseline.failure_points(); ++fail_at) {
            accounting_allocation_resource resource(fail_at);
            bool threw = false;
            try {
                auto connection = role == ruvia::http2_role::client
                                      ? ruvia::http2_connection::client({.resource_ = &resource})
                                      : ruvia::http2_connection::server({.resource_ = &resource});
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            RUVIA_CHECK(threw);
        }
    }
}

RUVIA_TEST(http2_public_escaped_events_remain_usable_after_move_assignment) {
    std::pmr::unsynchronized_pool_resource original;
    std::pmr::unsynchronized_pool_resource replacement;
    auto wire = server_request_wire(std::pmr::new_delete_resource(), "x");
    std::optional<ruvia::http2_event> request;
    std::optional<ruvia::http2_received_data_credit> credit;
    {
        auto connection = ruvia::http2_connection::server({.resource_ = &original});
        RUVIA_CHECK(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        request.emplace(std::move(*connection.next_event()));
        auto body = connection.next_event();
        RUVIA_CHECK(body && body->message_body_chunk() != nullptr);
        credit.emplace(body->message_body_chunk()->take_credit());
        connection = ruvia::http2_connection::server({.resource_ = &replacement});
        RUVIA_CHECK(request->request_head()->request().method() == "POST");
    }
    request.reset();
    credit.reset();
}

RUVIA_TEST(http2_public_client_terminal_event_preserves_unacknowledged_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client(&resource);
    const auto wire = client_response_wire(&resource, "x");
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);

    const auto head = client.next_event();
    auto chunk = client.next_event();
    auto* body = chunk ? chunk->message_body_chunk() : nullptr;
    RUVIA_CHECK(head && head->response_head() != nullptr);
    RUVIA_CHECK(body != nullptr && body->bytes() == "x");
    auto credit = body->take_credit();

    const auto end = client.next_event();
    RUVIA_CHECK(end && end->message_end() != nullptr);
    RUVIA_CHECK(client.acknowledge(std::move(credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
    RUVIA_CHECK(client.acknowledge(std::move(credit)) ==
                ruvia::http2_received_data_acknowledge_status::invalid_credit);
}

RUVIA_TEST(http2_public_dropped_data_credit_returns_debt_and_releases_closed_stream) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client(&resource);
    const auto wire = client_response_wire(&resource, "x");
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);

    (void)client.next_event();
    {
        auto chunk = client.next_event();
        auto* body = chunk ? chunk->message_body_chunk() : nullptr;
        RUVIA_CHECK(body != nullptr);
        auto credit = body->take_credit();
        const auto end = client.next_event();
        RUVIA_CHECK(end && end->message_end() != nullptr);
        RUVIA_CHECK(credit.valid());
    }

    RUVIA_CHECK(
        client.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::closed);
}

RUVIA_TEST(http2_public_client_reset_preserves_outstanding_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client(&resource);
    const auto wire = client_response_wire(&resource, "x", false, false);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);

    (void)client.next_event();
    auto chunk = client.next_event();
    auto* body = chunk ? chunk->message_body_chunk() : nullptr;
    RUVIA_CHECK(body != nullptr);
    auto credit = body->take_credit();

    RUVIA_CHECK(client.submit_reset(1, ruvia::http2_error_code::cancel) ==
                ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(client.acknowledge(std::move(credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
}

RUVIA_TEST(http2_public_peer_reset_preserves_outstanding_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client(&resource);
    auto wire = client_response_wire(&resource, "x", false, false);
    constexpr std::array<char, 4> cancel_payload{0, 0, 0, 8};
    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1,
        std::string_view(cancel_payload.data(), cancel_payload.size()));
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);

    (void)client.next_event();
    auto chunk = client.next_event();
    auto* body = chunk ? chunk->message_body_chunk() : nullptr;
    RUVIA_CHECK(body != nullptr);
    auto credit = body->take_credit();
    const auto closed = client.next_event();
    RUVIA_CHECK(closed && closed->stream_closed() != nullptr);

    RUVIA_CHECK(client.acknowledge(std::move(credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
}

RUVIA_TEST(http2_public_server_request_view_hides_stream_storage) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    RUVIA_CHECK(!server.header_block_in_progress());
    (void)server.consume_output(server.pending_output().size());
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    const auto request_view = server.server_request_view(1);
    RUVIA_CHECK(request_view.has_value());
    RUVIA_CHECK(request_view->method_ == "POST");
    RUVIA_CHECK(request_view->path_ == "/upload");
    RUVIA_CHECK(!server.server_request_view(3).has_value());
    const auto window = server.send_window_state(1);
    RUVIA_CHECK(window.has_value());
    RUVIA_CHECK(window->available_ == 65535);
    RUVIA_CHECK(!window->queued_data_);
    RUVIA_CHECK(!server.stream_aborted(1));
    RUVIA_CHECK(server.stream_aborted(3));

    auto request = ruvia::make_http2_server_request(server, 1, &resource, {});
    RUVIA_CHECK((request.index() == 0));
    RUVIA_CHECK(std::get<0>(request).method() == "POST");
    const auto handshake = ruvia::validate_http2_websocket_handshake(server, 1, std::get<0>(request));
    RUVIA_CHECK(handshake.accepted() == nullptr);
}

RUVIA_TEST(http2_public_server_release_preserves_outstanding_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    const auto wire = server_request_wire(&resource, "x");
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    auto chunk = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    auto* body = chunk ? chunk->message_body_chunk() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(body != nullptr && body->bytes() == "x");
    RUVIA_CHECK(end && end->message_end() != nullptr);
    auto credit = body->take_credit();

    ruvia::http_response response({.resource_ = &resource});
    RUVIA_CHECK(server.submit_buffered_response(1, response) == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
    RUVIA_CHECK(server.acknowledge(std::move(credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
}

RUVIA_TEST(http2_public_server_submits_streaming_response_head_and_data) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(end && end->message_end() != nullptr);
    (void)server.consume_output(server.pending_output().size());

    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    RUVIA_CHECK(server.submit_streaming_response_head(1, std::move(response)) ==
                ruvia::http2_submit_status::accepted);

    const auto head_output = server.pending_output();
    const auto head = ruvia::parse_http2_frame_header(
        std::span<const char>(head_output.data(), head_output.size()));
    RUVIA_CHECK(head.has_value());
    RUVIA_CHECK(head && head->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    RUVIA_CHECK(head && head->stream_id_ == 1);
    RUVIA_CHECK(head && (head->flags_ & 0x1U) == 0);
    (void)server.consume_output(head_output.size());

    RUVIA_CHECK(server.submit_data(1, "event: update\n\ndata: ok\n\n",
                    ruvia::http2_end_stream::end_stream) ==
                ruvia::http2_data_submit_status::accepted);
    const auto data_output = server.pending_output();
    const auto data = ruvia::parse_http2_frame_header(
        std::span<const char>(data_output.data(), data_output.size()));
    RUVIA_CHECK(data.has_value());
    RUVIA_CHECK(data && data->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
    RUVIA_CHECK(data && (data->flags_ & 0x1U) != 0);

    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_server_streaming_response_commit_plan_and_finish_are_public) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(end && end->message_end() != nullptr);
    (void)server.consume_output(server.pending_output().size());

    ruvia::http_response response({.resource_ = &resource});
    const auto committed = server.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::present);
    RUVIA_CHECK(committed.failure() == nullptr);
    RUVIA_CHECK(committed.submitted() != nullptr);
    if (const auto* plan = committed.submitted()) {
        RUVIA_CHECK(plan->framing() == ruvia::http_response_stream_framing::http2_frames);
        RUVIA_CHECK(plan->head_disposition() ==
                    ruvia::http_response_stream_head_disposition::body_open);
        RUVIA_CHECK(plan->trailer_framing() ==
                    ruvia::http_response_stream_trailer_framing::http2_trailing_headers);
    }
    ruvia::http_response duplicate({.resource_ = &resource});
    const auto rejected = server.submit_streaming_response_head(1, std::move(duplicate),
        ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(rejected.failure() != nullptr);
    RUVIA_CHECK(rejected.failure() && rejected.failure()->error() ==
                                          ruvia::http2_response_head_submit_error::invalid_state);
    (void)server.consume_output(server.pending_output().size());

    const std::array<ruvia::http_header_view, 1> fields_value{{{"x-final", "done"}}};
    const auto trailers = ruvia::validate_http_response_trailers(fields_value);
    RUVIA_CHECK(server.submit_data(1, "body", ruvia::http2_end_stream::keep_open) ==
                ruvia::http2_data_submit_status::accepted);
    RUVIA_CHECK(server.finish_response(1, trailers) ==
                ruvia::http2_finish_response_status::accepted);
    const auto trailer_frame = ruvia::parse_http2_frame_header(
        std::span<const char>(server.pending_output().data(), server.pending_output().size()));
    RUVIA_CHECK(trailer_frame && trailer_frame->type_ ==
                                     static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
    (void)server.consume_output(ruvia::http2_frame_header_bytes + trailer_frame->length_);
    const auto terminal = ruvia::parse_http2_frame_header(
        std::span<const char>(server.pending_output().data(), server.pending_output().size()));
    RUVIA_CHECK(terminal && terminal->type_ ==
                                static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    RUVIA_CHECK(terminal && (terminal->flags_ & 0x1U) != 0);
    RUVIA_CHECK(server.finish_response(1, trailers) ==
                ruvia::http2_finish_response_status::invalid_state);
    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_streaming_head_response_ends_at_headers) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    const auto wire = server_request_wire(&resource, {}, "HEAD");
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(end && end->message_end() != nullptr);
    (void)server.consume_output(server.pending_output().size());

    ruvia::http_response response({.resource_ = &resource});
    const auto committed = server.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(committed.submitted() != nullptr);
    RUVIA_CHECK(committed.submitted() && committed.submitted()->head_disposition() ==
                                             ruvia::http_response_stream_head_disposition::message_ended);
    const auto output = server.pending_output();
    const auto frame = ruvia::parse_http2_frame_header(
        std::span<const char>(output.data(), output.size()));
    RUVIA_CHECK(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    RUVIA_CHECK(frame && (frame->flags_ & 0x1U) != 0);
    RUVIA_CHECK(server.submit_data(1, "", ruvia::http2_end_stream::end_stream) ==
                ruvia::http2_data_submit_status::invalid_state);
    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_server_submits_buffered_response_head_and_returns_write_plan) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(end && end->message_end() != nullptr);
    (void)server.consume_output(server.pending_output().size());

    ruvia::http_response response({.resource_ = &resource});
    response.body("payload");
    const auto wrong_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::head, response);
    const auto mismatch = server.submit_response_head(1, response, wrong_plan);
    RUVIA_CHECK(mismatch.failure() != nullptr);
    RUVIA_CHECK(mismatch.failure() && mismatch.failure()->error() ==
                                          ruvia::http2_response_head_submit_error::response_plan_mismatch);
    const auto write_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::post, response);
    const auto submitted = server.submit_response_head(1, response, write_plan);
    RUVIA_CHECK(submitted.failure() == nullptr);
    RUVIA_CHECK(submitted.submitted() != nullptr);
    if (const auto* plan = submitted.submitted()) {
        RUVIA_CHECK(plan->response_status() == ruvia::http_status::ok);
        RUVIA_CHECK(plan->send_body());
        RUVIA_CHECK(plan->content_length() == 7);
    }
    const auto head_output = server.pending_output();
    const auto head = ruvia::parse_http2_frame_header(
        std::span<const char>(head_output.data(), head_output.size()));
    RUVIA_CHECK(head && head->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    RUVIA_CHECK(head && (head->flags_ & 0x1U) == 0);
    (void)server.consume_output(head_output.size());
    RUVIA_CHECK(server.submit_data(1, "payload", ruvia::http2_end_stream::end_stream) ==
                ruvia::http2_data_submit_status::accepted);

    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_response_head_submit_exposes_shared_failure_values) {
    std::pmr::monotonic_buffer_resource resource;

    auto closed = ruvia::http2_connection::server({.resource_ = &resource});
    ruvia::http_response closed_response({.resource_ = &resource});
    const auto closed_buffered = closed.submit_response_head(1, closed_response,
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, closed_response));
    RUVIA_CHECK(closed_buffered.failure() != nullptr);
    RUVIA_CHECK(closed_buffered.failure() && closed_buffered.failure()->error() ==
                                                 ruvia::http2_response_head_submit_error::closed);
    const auto closed_streaming = closed.submit_streaming_response_head(1,
        ruvia::http_response({.resource_ = &resource}), ruvia::http_response_stream_kind::generic,
        ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(closed_streaming.failure() != nullptr);
    RUVIA_CHECK(closed_streaming.failure() && closed_streaming.failure()->error() ==
                                                  ruvia::http2_response_head_submit_error::closed);

    auto client = prepared_client(&resource);
    ruvia::http_response client_response({.resource_ = &resource});
    const auto invalid_state = client.submit_streaming_response_head(1, std::move(client_response),
        ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(invalid_state.failure() != nullptr);
    RUVIA_CHECK(invalid_state.failure() && invalid_state.failure()->error() ==
                                               ruvia::http2_response_head_submit_error::invalid_state);

    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(end && end->message_end() != nullptr);
    (void)server.consume_output(server.pending_output().size());

    ruvia::http_response invalid_response({.resource_ = &resource});
    invalid_response.header("Content-Length", "invalid");
    const auto invalid_message = server.submit_streaming_response_head(1,
        std::move(invalid_response), ruvia::http_response_stream_kind::generic,
        ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(invalid_message.failure() != nullptr);
    RUVIA_CHECK(invalid_message.failure() && invalid_message.failure()->error() ==
                                                 ruvia::http2_response_head_submit_error::invalid_message);
    RUVIA_CHECK(server.pending_output().empty());
    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_dropped_request_preserves_outstanding_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    const auto wire = server_request_wire(&resource, "x");
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    auto chunk = server.next_event();
    auto* body = chunk ? chunk->message_body_chunk() : nullptr;
    RUVIA_CHECK(request && request->request_head() != nullptr);
    RUVIA_CHECK(body != nullptr);
    auto credit = body->take_credit();

    request.reset();
    RUVIA_CHECK(server.acknowledge(std::move(credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
}

RUVIA_TEST(http2_public_dropped_request_event_abandons_its_stream) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    (void)server.consume_output(server.pending_output().size());

    {
        const auto request = server.next_event();
        RUVIA_CHECK(request && request->request_head() != nullptr);
    }

    const auto output = server.pending_output();
    const auto frame =
        ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
    RUVIA_CHECK(frame.has_value());
    RUVIA_CHECK(
        frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
}

RUVIA_TEST(http2_public_dropped_request_retries_failed_abandonment) {
    toggle_allocation_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    std::pmr::string initial_output(&resource);
    server.take_output(initial_output);
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head() != nullptr);
    resource.reject();
    request.reset();

    resource.reject(false);
    const auto output = server.pending_output();
    constexpr auto settings_ack_bytes = ruvia::http2_frame_header_bytes;
    RUVIA_CHECK(output.size() == settings_ack_bytes + ruvia::http2_frame_header_bytes + 4);
    const auto reset = ruvia::parse_http2_frame_header(std::span<const char>(
        output.data() + settings_ack_bytes, output.size() - settings_ack_bytes));
    RUVIA_CHECK(
        reset && reset->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
    RUVIA_CHECK(reset && reset->stream_id_ == 1);
}

RUVIA_TEST(http2_public_request_endpoint_survives_connection_destruction_without_aba) {
    std::pmr::monotonic_buffer_resource resource;
    auto escaped = [&]() {
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        (void)server.consume_output(server.pending_output().size());
        const auto wire = server_request_wire(&resource, {});
        (void)server.feed(wire);
        return server.next_event();
    }();
    RUVIA_CHECK(escaped && escaped->request_head() != nullptr);

    auto other = ruvia::http2_connection::server({.resource_ = &resource});
    RUVIA_CHECK(other.release(std::move(*escaped->request_head())) ==
                ruvia::http2_server_request_release_status::invalid_lease);
    escaped.reset();
}

RUVIA_TEST(http2_public_data_credit_endpoint_survives_connection_destruction_without_aba) {
    std::pmr::monotonic_buffer_resource resource;
    auto escaped_credit = [&]() {
        auto client = prepared_client(&resource);
        const auto wire = client_response_wire(&resource, "x");
        (void)client.feed(wire);
        (void)client.next_event();
        auto chunk = client.next_event();
        return chunk->message_body_chunk()->take_credit();
    }();
    RUVIA_CHECK(escaped_credit.valid());

    auto other = prepared_client(&resource);
    RUVIA_CHECK(other.acknowledge(std::move(escaped_credit)) ==
                ruvia::http2_received_data_acknowledge_status::invalid_credit);
    RUVIA_CHECK(escaped_credit.valid());
}

RUVIA_TEST(http2_public_request_views_survive_connection_move_assignment) {
    auto* resource = std::pmr::new_delete_resource();
    // Keep caller-owned input alive so this test isolates the connection's decoded storage.
    const auto wire = server_request_wire(resource, "x");
    std::optional<ruvia::http2_event> escaped_request;
    std::optional<ruvia::http2_event> escaped_chunk;

    auto server = ruvia::http2_connection::server({.resource_ = resource});
    (void)server.consume_output(server.pending_output().size());
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head() != nullptr);
    if (request) {
        escaped_request.emplace(std::move(*request));
    }
    auto chunk = server.next_event();
    RUVIA_CHECK(chunk && chunk->message_body_chunk() != nullptr);
    if (chunk) {
        escaped_chunk.emplace(std::move(*chunk));
    }

    // Move assignment destroys the old implementation while the public events still hold leases.
    auto replacement = ruvia::http2_connection::server({.resource_ = resource});
    server = std::move(replacement);

    const auto* request_head = escaped_request ? escaped_request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    if (request_head != nullptr) {
        const auto& materialized = request_head->request();
        RUVIA_CHECK(materialized.method() == "POST");
        RUVIA_CHECK(materialized.target() == "/upload");
        RUVIA_CHECK(materialized.authority() == "example.test");
        const auto content_length = materialized.header("content-length");
        RUVIA_CHECK(content_length && *content_length == "1");
    }

    const auto* body = escaped_chunk ? escaped_chunk->message_body_chunk() : nullptr;
    RUVIA_CHECK(body != nullptr);
    if (body != nullptr) {
        RUVIA_CHECK(body->bytes() == "x");
    }
}

#if !defined(_MSC_VER)
RUVIA_TEST(http2_public_response_materialization_failure_keeps_event_retryable) {
    toggle_allocation_resource resource;
    auto client = prepared_client(&resource);
    const auto wire = client_response_wire(&resource, {}, true);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);

    resource.reject();
    bool threw = false;
    try {
        (void)client.next_event();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);

    resource.reject(false);
    const auto retried = client.next_event();
    RUVIA_CHECK(retried && retried->response_head() != nullptr);
    RUVIA_CHECK(retried && retried->response_head()->head().headers().size() == 1);
}

RUVIA_TEST(http2_public_request_materialization_failure_keeps_event_retryable) {
    toggle_allocation_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    resource.reject();
    bool threw = false;
    try {
        (void)server.next_event();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);

    resource.reject(false);
    const auto retried = server.next_event();
    RUVIA_CHECK(retried && retried->request_head() != nullptr);
    RUVIA_CHECK(retried && retried->request_head()->request().method() == "POST");
}
#endif

RUVIA_TEST(http2_public_stream_receive_status_is_a_read_only_snapshot) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client(&resource);
    RUVIA_CHECK(client.stream_receive_status(1) == ruvia::http2_stream_receive_status::open);
    RUVIA_CHECK(client.stream_receive_status(3) == ruvia::http2_stream_receive_status::closed);

    auto wire = client_response_wire(&resource, {}, false, false);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(client.stream_receive_status(1) == ruvia::http2_stream_receive_status::open);
    while (client.next_event()) {
    }

    std::pmr::string end_frame(&resource);
    append_frame(end_frame, ruvia::http2_frame_type::data, 0x1, 1, {});
    RUVIA_CHECK(client.feed(end_frame) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(client.stream_receive_status(1) == ruvia::http2_stream_receive_status::ended);
}

RUVIA_TEST(http2_public_received_peer_settings_reports_handshake_readiness) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    RUVIA_CHECK(!client.received_peer_settings());

    std::pmr::string wire(&resource);
    append_peer_settings(wire);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(client.received_peer_settings());
}

RUVIA_TEST(http2_public_response_head_and_trailers_are_owned_by_events) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client(&resource);
    auto wire = client_response_with_trailers_wire(&resource);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);

    auto head_event = client.next_event();
    RUVIA_CHECK(head_event && head_event->response_head() != nullptr);
    auto owned_head = std::move(*head_event->response_head()).take_head();
    RUVIA_CHECK(owned_head.status() == ruvia::http_status::ok);
    RUVIA_CHECK(owned_head.headers().size() == 1);
    RUVIA_CHECK(owned_head.headers().front().name() == "x-test");

    auto end_event = client.next_event();
    RUVIA_CHECK(end_event && end_event->message_end() != nullptr);
    RUVIA_CHECK(end_event->message_end()->trailers().size() == 1);
    auto owned_trailers_value = std::move(*end_event->message_end()).take_trailers();
    RUVIA_CHECK(owned_trailers_value.size() == 1);
    RUVIA_CHECK(owned_trailers_value.front().name() == "x-trace");
    RUVIA_CHECK(owned_trailers_value.front().value() == "done");

    auto replacement = ruvia::http2_connection::client({.resource_ = &resource});
    client = std::move(replacement);
    RUVIA_CHECK(owned_head.headers().front().value() == "owned");
    RUVIA_CHECK(owned_trailers_value.front().value() == "done");
}

RUVIA_TEST(http2_public_message_end_reports_metadata_only_and_empty_trailers) {
    for (const auto method : {std::string_view("HEAD"), std::string_view("GET")}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = prepared_client_method(&resource, method);
        std::pmr::string wire(&resource);
        append_peer_settings(wire);
        append_response(wire, &resource, 1, method == "HEAD" ? "200" : "304");
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = client.next_event();
        RUVIA_CHECK(head && head->response_head() != nullptr);
        auto end = client.next_event();
        RUVIA_CHECK(end && end->message_end() != nullptr);
        RUVIA_CHECK(end->message_end()->trailers().empty());
        RUVIA_CHECK(end->message_end()->content_semantics() ==
                    ruvia::http2_message_content_semantics::metadata_only);
    }

    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client_method(&resource, "GET");
    std::pmr::string wire(&resource);
    append_peer_settings(wire);
    append_response(wire, &resource, 1, "200", "body");
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().has_value());
    RUVIA_CHECK(client.next_event().has_value());
    const auto end = client.next_event();
    RUVIA_CHECK(end && end->message_end() != nullptr);
    RUVIA_CHECK(end->message_end()->content_semantics() ==
                ruvia::http2_message_content_semantics::content);
}

RUVIA_TEST(http2_public_trailer_decode_failure_is_retryable_and_event_retains_fields) {
    toggle_allocation_resource resource;
    auto client = prepared_client(&resource);
    std::pmr::string wire(&resource);
    append_peer_settings(wire);
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().has_value());
    block.clear();
    const std::string trailer_value(256, 't');
    ruvia::hpack_encoder::encode_header(block, "x-trace", trailer_value);
    std::pmr::string trailers(&resource);
    append_frame(trailers, ruvia::http2_frame_type::headers, 0x5, 1, block);
    resource.reject();
    bool threw = false;
    try {
        (void)client.feed(trailers);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    resource.reject(false);
    RUVIA_CHECK(client.feed(trailers) == ruvia::http2_feed_result::accepted);
    auto retried = client.next_event();
    RUVIA_CHECK(retried && retried->message_end() != nullptr);
    RUVIA_CHECK(retried->message_end()->trailers().size() == 1);
    const auto received_value = retried->message_end()->trailers();
    RUVIA_CHECK(received_value.front().value() == trailer_value);
}

RUVIA_TEST(http2_public_data_credit_merge_is_linear) {
    toggle_allocation_resource resource;
    auto client = prepared_client(&resource);
    auto wire = client_response_wire(&resource, "x", false, false);
    append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "x");
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    std::pmr::string drained(&resource);
    client.take_output(drained);
    RUVIA_CHECK(client.next_event().has_value());
    auto first = client.next_event();
    RUVIA_CHECK(first && first->message_body_chunk() != nullptr);
    auto first_credit = first->message_body_chunk()->take_credit();
    constexpr std::size_t frame_count = 2;
    for (std::size_t index = 1; index < frame_count; ++index) {
        auto chunk = client.next_event();
        RUVIA_CHECK(chunk && chunk->message_body_chunk() != nullptr);
        auto next_credit = chunk->message_body_chunk()->take_credit();
        RUVIA_CHECK(first_credit.merge(std::move(next_credit)) ==
                    ruvia::http2_received_data_credit_merge_status::merged);
        RUVIA_CHECK(first_credit.valid());
        RUVIA_CHECK(!next_credit.valid());
        RUVIA_CHECK(client.pending_output().empty());
    }
    auto end = client.next_event();
    RUVIA_CHECK(end && end->message_end() != nullptr);
    RUVIA_CHECK(client.pending_output().empty());
    RUVIA_CHECK(client.acknowledge(std::move(first_credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
    RUVIA_CHECK(client.submit_reset(1, ruvia::http2_error_code::cancel) ==
                ruvia::http2_submit_status::closed);
}

RUVIA_TEST(http2_public_data_credit_merge_rejects_different_connection_unchanged) {
    std::pmr::monotonic_buffer_resource resource;
    auto first_client = prepared_client(&resource);
    std::pmr::string first_wire(&resource);
    append_peer_settings(first_wire);
    append_response(first_wire, &resource, 1, "200", "a", {}, {});
    RUVIA_CHECK(first_client.feed(first_wire) == ruvia::http2_feed_result::accepted);
    (void)first_client.next_event();
    auto first_event = first_client.next_event();
    auto first_credit = first_event->message_body_chunk()->take_credit();

    auto second_client = prepared_client(&resource);
    std::pmr::string second_wire(&resource);
    append_peer_settings(second_wire);
    append_response(second_wire, &resource, 1, "200", "b", {}, {});
    RUVIA_CHECK(second_client.feed(second_wire) == ruvia::http2_feed_result::accepted);
    (void)second_client.next_event();
    auto second_event = second_client.next_event();
    auto second_credit = second_event->message_body_chunk()->take_credit();

    RUVIA_CHECK(first_credit.merge(std::move(second_credit)) ==
                ruvia::http2_received_data_credit_merge_status::different_stream);
    RUVIA_CHECK(first_credit.valid());
    RUVIA_CHECK(second_credit.valid());
    RUVIA_CHECK(first_client.acknowledge(std::move(first_credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
    RUVIA_CHECK(second_client.acknowledge(std::move(second_credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
}

RUVIA_TEST(http2_public_data_credit_merge_rejects_different_stream_unchanged) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client(&resource);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/two"});
    RUVIA_CHECK(submitted.submitted() != nullptr);

    std::pmr::string wire(&resource);
    append_peer_settings(wire);
    append_response(wire, &resource, 1, "200", "a");
    append_response(wire, &resource, 3, "200", "b");
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    (void)client.next_event();
    auto first_event = client.next_event();
    RUVIA_CHECK(first_event && first_event->message_body_chunk() != nullptr);
    auto first_credit = first_event->message_body_chunk()->take_credit();
    (void)client.next_event();
    (void)client.next_event();
    auto second_event = client.next_event();
    RUVIA_CHECK(second_event && second_event->message_body_chunk() != nullptr);
    auto second_credit = second_event->message_body_chunk()->take_credit();

    RUVIA_CHECK(first_credit.merge(std::move(second_credit)) ==
                ruvia::http2_received_data_credit_merge_status::different_stream);
    RUVIA_CHECK(first_credit.valid());
    RUVIA_CHECK(second_credit.valid());
    RUVIA_CHECK(client.acknowledge(std::move(first_credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
    RUVIA_CHECK(client.acknowledge(std::move(second_credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
}

RUVIA_TEST(http2_public_server_message_end_owns_request_trailers) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    auto wire = server_request_with_trailers_wire(&resource);
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head() != nullptr);
    RUVIA_CHECK(!request->request_head()->request().header("x-request-trace"));
    const auto chunk = server.next_event();
    RUVIA_CHECK(chunk && chunk->message_body_chunk() != nullptr);
    auto end = server.next_event();
    RUVIA_CHECK(end && end->message_end() != nullptr);
    RUVIA_CHECK(end->message_end()->content_semantics() ==
                ruvia::http2_message_content_semantics::content);
    RUVIA_CHECK(end->message_end()->trailers().size() == 1);
    auto trailers = std::move(*end->message_end()).take_trailers();
    RUVIA_CHECK(trailers.size() == 1);
    if (!trailers.empty()) {
        RUVIA_CHECK(trailers.front().name() == "x-request-trace");
        RUVIA_CHECK(trailers.front().value() == "done");
    }
    RUVIA_CHECK(server.release(std::move(*request->request_head())) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_request_headers_remain_valid_while_trailers_arrive) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    const std::string initial_value(700, 'a');
    const std::string trailer_value(3000, 'b');
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");
    ruvia::hpack_encoder::encode_header(block, "x-initial", initial_value);
    std::pmr::string wire(ruvia::http2_client_preface, &resource);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head() != nullptr);
    const auto borrowed = request->request_head()->request().header("x-initial");
    RUVIA_CHECK(borrowed && *borrowed == initial_value);

    block.clear();
    ruvia::hpack_encoder::encode_header(block, "x-trailer", trailer_value);
    std::pmr::string trailers(&resource);
    append_frame(trailers, ruvia::http2_frame_type::headers, 0x5, 1, block);
    RUVIA_CHECK(server.feed(trailers) == ruvia::http2_feed_result::accepted);
    auto end = server.next_event();
    RUVIA_CHECK(end && end->message_end() != nullptr);
    RUVIA_CHECK(borrowed && *borrowed == initial_value);
    RUVIA_CHECK(!request->request_head()->request().header("x-trailer"));
    const auto fields_value = end->message_end()->trailers();
    RUVIA_CHECK(fields_value.size() == 1);
    if (!fields_value.empty()) {
        RUVIA_CHECK(fields_value.front().value() == trailer_value);
    }
    RUVIA_CHECK(server.release(std::move(*request->request_head())) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_server_release_before_message_end_keeps_terminal_event) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head() != nullptr);
    ruvia::http_response response({.resource_ = &resource});
    RUVIA_CHECK(server.submit_buffered_response(1, response) ==
                ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(server.release(std::move(*request->request_head())) ==
                ruvia::http2_server_request_release_status::released);

    auto end = server.next_event();
    RUVIA_CHECK(end && end->message_end() != nullptr);
    RUVIA_CHECK(end->message_end()->stream_id() == 1);
    RUVIA_CHECK(end->message_end()->trailers().empty());
}

RUVIA_TEST(http2_public_server_request_snapshot_exposes_http_decisions) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", "1");
    ruvia::hpack_encoder::encode_header(block, "expect", "100-continue");
    std::pmr::string wire(ruvia::http2_client_preface, &resource);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);

    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto event = server.next_event();
    auto* request = event ? event->request_head() : nullptr;
    RUVIA_CHECK(request != nullptr);
    if (request == nullptr) {
        return;
    }
    RUVIA_CHECK(request->snapshot().content_ == ruvia::http_request_content_indication::will_follow);
    RUVIA_CHECK(request->snapshot().body_open_);
    RUVIA_CHECK(!request->snapshot().connect_pending_);
    const auto plan = request->expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
    RUVIA_CHECK(plan.send_continue() != nullptr);
    RUVIA_CHECK(plan.rejection() == nullptr);
}

RUVIA_TEST(http2_public_dropped_request_before_message_end_keeps_terminal_event) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)server.consume_output(server.pending_output().size());
    auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head() != nullptr);
    request.reset();

    bool saw_end = false;
    while (const auto event = server.next_event()) {
        if (event->message_end() != nullptr) {
            saw_end = true;
            RUVIA_CHECK(event->message_end()->stream_id() == 1);
            RUVIA_CHECK(event->message_end()->trailers().empty());
        }
    }
    RUVIA_CHECK(saw_end);
}

RUVIA_TEST(http2_public_client_reset_before_terminal_events_drain_keeps_events_readable) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/", .content_ = ruvia::http2_request_content::streaming()});
    RUVIA_CHECK(submitted.submitted() != nullptr);
    auto wire = client_response_wire(&resource, {}, false, true);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    const auto reset = client.submit_reset(1, ruvia::http2_error_code::cancel);
    RUVIA_CHECK(reset == ruvia::http2_submit_status::accepted);

    bool saw_head = false;
    bool saw_end = false;
    while (const auto event = client.next_event()) {
        if (event->response_head() != nullptr) {
            saw_head = true;
            RUVIA_CHECK(event->response_head()->head().status() == ruvia::http_status::ok);
        }
        if (event->message_end() != nullptr) {
            saw_end = true;
            RUVIA_CHECK(event->message_end()->trailers().empty());
        }
    }
    RUVIA_CHECK(saw_head);
    RUVIA_CHECK(saw_end);
}

RUVIA_TEST(http2_public_streaming_known_length_keeps_upload_open_for_trailers) {
    for (const std::uint64_t length : {0U, 3U}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = ruvia::http2_connection::client({.resource_ = &resource});
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        auto exchange_value = [&](auto& from, auto& to) {
            const auto wire = from.pending_output();
            RUVIA_CHECK(to.feed(wire) == ruvia::http2_feed_result::accepted);
            RUVIA_CHECK(from.consume_output(wire.size()) == ruvia::http2_output_consume_status::drained);
        };
        exchange_value(client, server);
        exchange_value(server, client);
        const auto head = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http2_request_content::streaming(length)});
        RUVIA_CHECK(head.submitted() != nullptr);
        exchange_value(client, server);
        bool saw_end = false;
        std::optional<ruvia::http2_request_head_event> lease;
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                RUVIA_CHECK_EQ(request->request().header("content-length").value_or(""), length == 0 ? "0" : "3");
                lease.emplace(std::move(*request));
            }
            saw_end |= event->message_end() != nullptr;
        }
        RUVIA_CHECK(!saw_end);
        if (length != 0) {
            RUVIA_CHECK(client.submit_data(1, "abc", ruvia::http2_end_stream::keep_open) == ruvia::http2_data_submit_status::accepted);
        }
        const std::array<ruvia::http_header_view, 1> trailers{{{"x-end", "retained"}}};
        RUVIA_CHECK(client.finish_request(1, trailers) == ruvia::http2_finish_request_status::accepted);
        exchange_value(client, server);
        while (auto event = server.next_event()) {
            if (const auto* end = event->message_end()) {
                saw_end = true;
                RUVIA_CHECK_EQ(end->trailers().size(), std::size_t{1});
                if (!end->trailers().empty()) {
                    RUVIA_CHECK_EQ(end->trailers()[0].value(), "retained");
                }
            }
        }
        RUVIA_CHECK(saw_end);
    }
}

RUVIA_TEST(http2_public_push_request_lease_preserves_fields_and_releases_repeated_streams) {
    std::pmr::unsynchronized_pool_resource resource;
    {
        auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        auto exchange_value = [&](auto& from, auto& to) {
            const auto wire = from.pending_output();
            RUVIA_CHECK(to.feed(wire) == ruvia::http2_feed_result::accepted);
            (void)from.consume_output(wire.size());
        };
        exchange_value(client, server);
        exchange_value(server, client);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
        RUVIA_CHECK(submitted.submitted() != nullptr);
        exchange_value(client, server);
        std::optional<ruvia::http2_request_head_event> parent;
        while (auto event = server.next_event()) {
            if (auto* head = event->request_head()) {
                parent.emplace(std::move(*head));
            }
        }
        RUVIA_CHECK(parent.has_value());
        const std::array<ruvia::http_header_view, 3> headers{{{"host", "EXAMPLE.test:443"}, {"cookie", "a=1"}, {"x-push", "request"}}};
        for (unsigned repeat = 0; repeat != 100; ++repeat) {
            auto pushed = server.submit_push_request(submitted.submitted()->stream_id(),
                {.authority_ = "example.test", .path_ = "/asset?version=1", .headers_ = headers});
            RUVIA_CHECK((pushed.index() == 0));
            if ((pushed.index() != 0)) {
                break;
            }
            const auto& request = std::get<0>(pushed).request();
            RUVIA_CHECK(request.scheme() == "https" && request.authority() == "example.test");
            RUVIA_CHECK(request.path() == "/asset" && request.query_string() == "version=1");
            RUVIA_CHECK(request.header("cookie") == "a=1");
            RUVIA_CHECK(request.header("host") == "EXAMPLE.test:443");
            RUVIA_CHECK(request.header("x-push") == "request");
            ruvia::http_response response;
            RUVIA_CHECK(server.submit_buffered_response(std::get<0>(pushed).stream_id(), response) == ruvia::http2_submit_status::accepted);
            exchange_value(server, client);
            unsigned promised = 0;
            unsigned ended = 0;
            while (auto event = client.next_event()) {
                if (const auto* promise = event->push_promise()) {
                    ++promised;
                    RUVIA_CHECK(promise->request_.path_ == "/asset?version=1");
                }
                if (event->message_end()) {
                    ++ended;
                }
            }
            RUVIA_CHECK(promised == 1 && ended == 1);
            RUVIA_CHECK(server.release(std::move(std::get<0>(pushed))) == ruvia::http2_server_request_release_status::released);
            exchange_value(client, server);
            while (server.next_event()) {
            }
        }
        RUVIA_CHECK(server.release(std::move(*parent)) == ruvia::http2_server_request_release_status::released);
    }
}
