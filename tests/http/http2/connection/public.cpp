#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"

#include "output_drain.h"
#include "stl_allocation_fixture.h"
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
    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_allocations_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (failure_points_++ == fail_at_) {
            throw std::bad_alloc();
        }
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++live_allocations_;
        return allocation;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
        --live_allocations_;
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t fail_at_;
    std::size_t failure_points_{};
    std::size_t live_allocations_{};
};

class toggle_allocation_resource final : public std::pmr::memory_resource {
public:
    void reject(bool value = true) noexcept {
        reject_ = value;
    }
    void reject_after_output(const ruvia::http2_connection* connection) noexcept {
        reject_after_output_ = connection;
    }
    [[nodiscard]] std::size_t allocation_attempts() const noexcept {
        return allocation_attempts_;
    }
    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_allocations_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocation_attempts_;
        if (reject_ || (reject_after_output_ != nullptr && reject_after_output_->wants_write())) {
            throw std::bad_alloc();
        }
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++live_allocations_;
        return allocation;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
        --live_allocations_;
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    bool reject_{false};
    const ruvia::http2_connection* reject_after_output_{};
    std::size_t allocation_attempts_{};
    std::size_t live_allocations_{};
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

void append_header_section(std::pmr::string& wire, std::uint32_t stream_id,
    std::string_view block, bool end_stream, bool fragmented) {
    const auto flags = static_cast<std::uint8_t>(end_stream ? 0x1 : 0);
    if (!fragmented) {
        append_frame(wire, ruvia::http2_frame_type::headers,
            static_cast<std::uint8_t>(flags | 0x4), stream_id, block);
        return;
    }
    const auto split = block.size() / 2;
    append_frame(wire, ruvia::http2_frame_type::headers, flags, stream_id, block.substr(0, split));
    append_frame(wire, ruvia::http2_frame_type::continuation, 0x4, stream_id, block.substr(split));
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
    (void)ruvia::testing::drain_output(client);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
    if (!std::holds_alternative<std::uint32_t>(submitted)) {
        throw std::logic_error("test request was not submitted");
    }
    (void)ruvia::testing::drain_output(client);
    return client;
}

ruvia::http2_connection prepared_client_method(std::pmr::memory_resource* resource,
    std::string_view method) {
    auto client = ruvia::http2_connection::client({.resource_ = resource});
    (void)ruvia::testing::drain_output(client);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = method, .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
    if (!std::holds_alternative<std::uint32_t>(submitted)) {
        throw std::logic_error("test request was not submitted");
    }
    (void)ruvia::testing::drain_output(client);
    return client;
}

struct data_output_observations final {
    struct entry_type final {
        std::uint32_t stream_id_{};
        std::size_t payload_bytes_{};
    };
    std::array<entry_type, 4> entries_{};
    std::size_t count_{};
    bool overflow_{};

    static void observe(void* context, std::uint32_t stream_id, std::size_t payload_bytes) noexcept {
        auto& self = *static_cast<data_output_observations*>(context);
        if (self.count_ == self.entries_.size()) {
            self.overflow_ = true;
            return;
        }
        self.entries_[self.count_++] = {stream_id, payload_bytes};
    }
};

ruvia::http2_submit_status submit_buffered_response(
    ruvia::http2_connection& server, std::uint32_t stream_id, const ruvia::http_response& response) {
    std::variant<ruvia::http_buffered_response_write_plan, ruvia::http2_response_head_submit_error> head =
        ruvia::http2_response_head_submit_error::response_plan_mismatch;
    for (const auto method : {ruvia::http_known_method::get, ruvia::http_known_method::post,
             ruvia::http_known_method::options, ruvia::http_known_method::head,
             ruvia::http_known_method::put, ruvia::http_known_method::delete_value,
             ruvia::http_known_method::patch, ruvia::http_known_method::connect,
             ruvia::http_known_method::unknown}) {
        head = server.submit_response_head(
            stream_id, response, ruvia::plan_buffered_http_response_write(method, response));
        const auto* error = std::get_if<ruvia::http2_response_head_submit_error>(&head);
        if (error == nullptr || *error != ruvia::http2_response_head_submit_error::response_plan_mismatch) {
            break;
        }
    }
    const auto* plan = std::get_if<ruvia::http_buffered_response_write_plan>(&head);
    if (plan == nullptr) {
        const auto error = std::get<ruvia::http2_response_head_submit_error>(head);
        if (error == ruvia::http2_response_head_submit_error::closed) {
            return ruvia::http2_submit_status::closed;
        }
        if (error == ruvia::http2_response_head_submit_error::peer_stream_limit_reached) {
            return ruvia::http2_submit_status::peer_capability_unavailable;
        }
        return ruvia::http2_submit_status::invalid_state;
    }
    if (plan->send_body()) {
        const auto data = server.submit_data(
            stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, response.body_bytes().size());
        if (data.status_ == ruvia::http2_data_submit_status::closed) {
            return ruvia::http2_submit_status::closed;
        }
        if (data.status_ != ruvia::http2_data_submit_status::complete ||
            data.consumed_bytes_ != response.body_bytes().size()) {
            return ruvia::http2_submit_status::invalid_state;
        }
    }
    return ruvia::http2_submit_status::accepted;
}
}  // namespace

RUVIA_TEST(http2_public_request_submission_exposes_one_exclusive_success_or_failure_contract) {
    auto client = ruvia::http2_connection::client();
    const auto invalid = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "", .authority_ = "example.test"});
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_request_head_submit_error>(invalid));
    if (const auto* failure = std::get_if<ruvia::http2_request_head_submit_error>(&invalid)) {
        RUVIA_CHECK(*failure == ruvia::http2_request_head_submit_error::invalid_message);
    }
    const auto extended = client.submit_request_head(ruvia::http2_extended_connect_request_head_view{
        .protocol_ = "websocket", .authority_ = "example.test"});
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_request_head_submit_error>(extended));
    if (const auto* failure = std::get_if<ruvia::http2_request_head_submit_error>(&extended)) {
        RUVIA_CHECK(*failure == ruvia::http2_request_head_submit_error::peer_capability_unavailable);
    }
    const auto regular = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .authority_ = "example.test"});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(regular));
    if (const auto* submitted = std::get_if<std::uint32_t>(&regular)) {
        RUVIA_CHECK_EQ(*submitted, std::uint32_t{1});
    }
    const auto connect = client.submit_request_head(ruvia::http2_connect_request_head_view{
        .authority_ = "example.test:443"});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(connect));
    if (const auto* submitted = std::get_if<std::uint32_t>(&connect)) {
        RUVIA_CHECK_EQ(*submitted, std::uint32_t{3});
    }
    auto server = ruvia::http2_connection::server();
    const auto wrong_role = server.submit_request_head(ruvia::http2_regular_request_head_view{
        .authority_ = "example.test"});
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_request_head_submit_error>(wrong_role));
    if (const auto* failure = std::get_if<ruvia::http2_request_head_submit_error>(&wrong_role)) {
        RUVIA_CHECK(*failure == ruvia::http2_request_head_submit_error::invalid_state);
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
        RUVIA_CHECK_EQ(baseline.live_allocations(), std::size_t{0});
        if (!ruvia::test::stl_allocation_failure_injection_enabled(ruvia_ctx)) {
            continue;
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
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
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
        RUVIA_CHECK(request->request_head()->request_view().method_ == "POST");
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
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)client.acknowledge(std::move(credit)); }));
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

// RFC 9113 section 8.7: REFUSED_STREAM means the server did not process the
// request, so it is published as unprocessed (retryable), not as a reset stream,
// and the connection stays usable. After a final response head it stays a reset.
RUVIA_TEST(http2_public_client_refused_stream_reports_request_unprocessed) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client(&resource);
    std::pmr::string wire(&resource);
    append_peer_settings(wire);
    constexpr std::array<char, 4> refused_payload{0, 0, 0, 7};
    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1,
        std::string_view(refused_payload.data(), refused_payload.size()));
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    bool unprocessed = false;
    while (auto event = client.next_event()) {
        RUVIA_CHECK(event->stream_closed() == nullptr);
        if (const auto* value = event->request_unprocessed()) {
            unprocessed = value->stream_id() == 1;
        }
    }
    RUVIA_CHECK(unprocessed);
    RUVIA_CHECK(!client.connection_error().has_value());
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"})));

    auto started = prepared_client(&resource);
    auto started_wire = client_response_wire(&resource, "", false, false);
    append_frame(started_wire, ruvia::http2_frame_type::rst_stream, 0, 1,
        std::string_view(refused_payload.data(), refused_payload.size()));
    RUVIA_CHECK(started.feed(started_wire) == ruvia::http2_feed_result::accepted);
    bool reset = false;
    while (auto event = started.next_event()) {
        RUVIA_CHECK(event->request_unprocessed() == nullptr);
        if (const auto* closed = event->stream_closed()) {
            reset = closed->error() == ruvia::http2_error_code::refused_stream;
        }
    }
    RUVIA_CHECK(reset);
}

RUVIA_TEST(http2_public_server_request_view_hides_stream_storage) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    RUVIA_CHECK(!server.header_block_in_progress());
    (void)ruvia::testing::drain_output(server);
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto head_event = server.next_event();
    RUVIA_CHECK(head_event && head_event->request_head() != nullptr);
    const auto request_view = head_event->request_head()->request_view();
    RUVIA_CHECK(request_view.method_ == "POST");
    RUVIA_CHECK(request_view.path_ == "/upload");
    const auto window = server.send_window_state(1);
    RUVIA_CHECK(window.has_value());
    RUVIA_CHECK(window->available_ == 65535);
    RUVIA_CHECK(!server.stream_aborted(1));
    RUVIA_CHECK(server.stream_aborted(3));

    auto request = head_event->request_head()->build_request(&resource, {});
    RUVIA_CHECK(request.method() == "POST");
    const auto handshake = ruvia::validate_http2_websocket_handshake(server, 1, request);
    RUVIA_CHECK(handshake.accepted() == nullptr);
}

// RFC 9113 section 8.2.3: cookie crumbs are joined with "; ". A crumb with an
// empty value is not a cookie-pair and must not leave a leading or trailing
// separator in the joined field value.
RUVIA_TEST(http2_public_server_request_joins_cookie_crumbs_without_empty_members) {
    std::pmr::monotonic_buffer_resource resource;
    for (const auto crumbs : {std::array<std::string_view, 3>{"", "a=b", "c=d"}, std::array<std::string_view, 3>{"a=b", "", "c=d"},
             std::array<std::string_view, 3>{"a=b", "c=d", ""}}) {
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        (void)ruvia::testing::drain_output(server);
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/");
        for (const auto crumb : crumbs) {
            ruvia::hpack_encoder::encode_header(block, "cookie", crumb);
        }
        std::pmr::string wire(ruvia::http2_client_preface, &resource);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head_event = server.next_event();
        RUVIA_CHECK(head_event && head_event->request_head() != nullptr);
        auto request = head_event->request_head()->build_request(&resource, {});
        RUVIA_CHECK(request.header("cookie") == std::string_view("a=b; c=d"));
    }
}

RUVIA_TEST(http2_public_server_release_preserves_outstanding_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
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

    ruvia::http_response response(&resource);
    RUVIA_CHECK(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
    RUVIA_CHECK(server.acknowledge(std::move(credit)) ==
                ruvia::http2_received_data_acknowledge_status::acknowledged);
}

RUVIA_TEST(http2_public_send_window_observation_ends_with_protocol_closure) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    const auto wire = server_request_wire(&resource, {}, "GET");
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    auto* head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(head != nullptr);
    if (head == nullptr) {
        return;
    }
    const auto end = server.next_event();
    RUVIA_CHECK(end && end->message_end());
    RUVIA_CHECK(!server.next_event());
    RUVIA_CHECK(server.send_window_state(1).has_value());

    ruvia::http_response response(&resource);
    RUVIA_CHECK(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(head->request_view().method_ == "GET");
    RUVIA_CHECK(!server.send_window_state(1));
    RUVIA_CHECK_EQ(server.pending_data_output_bytes(1), std::size_t{0});
    RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    RUVIA_CHECK(!server.send_window_state(1));
}

// RFC 9113 section 5.1 (closed): WINDOW_UPDATE frames received on a closed
// stream are ignored, including a zero increment, both while the stream is
// still retained by its owner and after it has been released.
RUVIA_TEST(http2_public_window_update_on_closed_stream_is_ignored) {
    for (const bool released : {false, true}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        (void)ruvia::testing::drain_output(server);
        const auto request_wire = server_request_wire(&resource, {}, "GET");
        RUVIA_CHECK(server.feed(request_wire) == ruvia::http2_feed_result::accepted);
        auto request = server.next_event();
        auto* head = request ? request->request_head() : nullptr;
        RUVIA_CHECK(head != nullptr);
        if (head == nullptr) {
            return;
        }
        (void)server.next_event();
        ruvia::http_response response(&resource);
        RUVIA_CHECK(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
        if (released) {
            RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
        }
        (void)ruvia::testing::drain_output(server);

        std::pmr::string wire(&resource);
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, std::string_view("\x00\x00\x00\x00", 4));
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, std::string_view("\x00\x00\x01\x00", 4));
        // A later request proves the connection stayed usable.
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/next");
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 3, block);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        RUVIA_CHECK(!server.connection_error().has_value());
        RUVIA_CHECK(!server.wants_write());
        auto next = server.next_event();
        RUVIA_CHECK(next && next->request_head() && next->request_head()->stream_id() == 3);
        if (!released) {
            RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
        }
        if (next && next->request_head()) {
            (void)server.next_event();
            RUVIA_CHECK(server.release(std::move(*next->request_head())) == ruvia::http2_server_request_release_status::released);
        }
    }
}

RUVIA_TEST(http2_public_send_window_observation_excludes_reset_streams_with_credit) {
    for (const bool peer_reset : {false, true}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        auto wire = server_request_wire(&resource, "x", "POST", {}, false);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto request = server.next_event();
        auto* head = request ? request->request_head() : nullptr;
        auto chunk = server.next_event();
        auto* body = chunk ? chunk->message_body_chunk() : nullptr;
        RUVIA_CHECK(head != nullptr && body != nullptr);
        if (head == nullptr || body == nullptr) {
            continue;
        }
        auto credit = body->take_credit();
        chunk.reset();
        RUVIA_CHECK(!server.next_event());

        constexpr std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
        wire.clear();
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
            std::string_view(zero_window.data(), zero_window.size()));
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        ruvia::http_response response(&resource);
        RUVIA_CHECK(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
            server.submit_streaming_response_head(1, std::move(response))));
        const auto blocked = server.submit_data(1, "blocked", ruvia::http2_end_stream::keep_open, 7);
        RUVIA_CHECK(blocked.status_ == ruvia::http2_data_submit_status::backpressured);
        RUVIA_CHECK_EQ(blocked.consumed_bytes_, std::size_t{0});
        RUVIA_CHECK(server.send_window_state(1).has_value());
        RUVIA_CHECK_EQ(server.pending_data_output_bytes(1), std::size_t{0});

        if (peer_reset) {
            constexpr std::array<char, 4> cancel{0, 0, 0, 8};
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1,
                std::string_view(cancel.data(), cancel.size()));
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            const auto closed = server.next_event();
            RUVIA_CHECK(closed && closed->stream_closed());
            RUVIA_CHECK(!server.next_event());
        } else {
            RUVIA_CHECK(server.submit_reset(1, ruvia::http2_error_code::cancel) ==
                        ruvia::http2_submit_status::accepted);
        }

        RUVIA_CHECK(credit.valid());
        RUVIA_CHECK(head->request_view().method_ == "POST");
        RUVIA_CHECK(!server.send_window_state(1));
        RUVIA_CHECK(server.stream_aborted(1));
        const auto retry = server.submit_data(1, "blocked", ruvia::http2_end_stream::keep_open, 7);
        RUVIA_CHECK(retry.status_ == ruvia::http2_data_submit_status::closed);
        RUVIA_CHECK_EQ(retry.consumed_bytes_, std::size_t{0});
        RUVIA_CHECK_EQ(server.pending_data_output_bytes(1), std::size_t{0});
        RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
        RUVIA_CHECK(!server.send_window_state(1));
        RUVIA_CHECK(server.acknowledge(std::move(credit)) ==
                    ruvia::http2_received_data_acknowledge_status::acknowledged);
        RUVIA_CHECK(!server.send_window_state(1));
    }
}

RUVIA_TEST(http2_public_send_window_observation_keeps_unconsumed_half_closed_streams) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    auto wire = server_request_wire(&resource, {}, "GET");
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    auto* head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(head != nullptr);
    if (head == nullptr) {
        return;
    }
    const auto end = server.next_event();
    RUVIA_CHECK(end && end->message_end());
    RUVIA_CHECK(!server.next_event());
    RUVIA_CHECK(server.stream_receive_status(1) == ruvia::http2_stream_receive_status::ended);

    constexpr std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
        std::string_view(zero_window.data(), zero_window.size()));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    ruvia::http_response response(&resource);
    RUVIA_CHECK(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
        server.submit_streaming_response_head(1, std::move(response))));
    (void)ruvia::testing::drain_output(server);
    const auto blocked = server.submit_data(1, "body", ruvia::http2_end_stream::end_stream, 4);
    RUVIA_CHECK(blocked.status_ == ruvia::http2_data_submit_status::backpressured);
    RUVIA_CHECK_EQ(blocked.consumed_bytes_, std::size_t{0});
    const auto window = server.send_window_state(1);
    RUVIA_CHECK(window.has_value());
    if (window) {
        RUVIA_CHECK_EQ(window->stream_window_, 0);
        RUVIA_CHECK_EQ(window->available_, std::uint32_t{0});
    }
    RUVIA_CHECK_EQ(server.pending_data_output_bytes(1), std::size_t{0});

    constexpr std::array<char, 4> increment{0, 0, 0, 4};
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1,
        std::string_view(increment.data(), increment.size()));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.wants_write());
    RUVIA_CHECK(server.send_window_state(1)->available_ == 4);
    const auto retried = server.submit_data(1, "body", ruvia::http2_end_stream::end_stream, 4);
    RUVIA_CHECK(retried.status_ == ruvia::http2_data_submit_status::complete);
    RUVIA_CHECK_EQ(retried.consumed_bytes_, std::size_t{4});
    RUVIA_CHECK(!server.send_window_state(1));
    RUVIA_CHECK_EQ(server.pending_data_output_bytes(1), std::size_t{4});
    const auto output = ruvia::testing::drain_output(server);
    const auto frame = ruvia::parse_http2_frame_header(output);
    RUVIA_CHECK(frame && frame->length_ == 4 && (frame->flags_ & 1U) != 0);
    RUVIA_CHECK_EQ(output.size(), ruvia::http2_frame_header_bytes + 4);
    RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    RUVIA_CHECK(!server.send_window_state(1));
}

RUVIA_TEST(http2_public_server_submits_streaming_response_head_and_data) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(end && end->message_end() != nullptr);
    (void)ruvia::testing::drain_output(server);

    ruvia::http_response response(&resource);
    response.status(ruvia::http_status::ok);
    RUVIA_CHECK(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
        server.submit_streaming_response_head(1, std::move(response))));

    const auto head_output = ruvia::testing::drain_output(server);
    const auto head = ruvia::parse_http2_frame_header(
        std::span<const char>(head_output.data(), head_output.size()));
    RUVIA_CHECK(head.has_value());
    RUVIA_CHECK(head && head->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    RUVIA_CHECK(head && head->stream_id_ == 1);
    RUVIA_CHECK(head && (head->flags_ & 0x1U) == 0);

    RUVIA_CHECK(server.submit_data(1, "event: update\n\ndata: ok\n\n",
                          ruvia::http2_end_stream::end_stream, 65535)
                    .status_ ==
                ruvia::http2_data_submit_status::complete);
    const auto data_output = ruvia::testing::drain_output(server);
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
    (void)ruvia::testing::drain_output(server);
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(end && end->message_end() != nullptr);
    (void)ruvia::testing::drain_output(server);

    ruvia::http_response response(&resource);
    const auto committed = server.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_trailer_intent::present);
    RUVIA_CHECK(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
    if (const auto* plan = std::get_if<ruvia::http_response_stream_commit_plan>(&committed)) {
        RUVIA_CHECK(plan->framing() == ruvia::http_response_stream_framing::http2_frames);
        RUVIA_CHECK(plan->head_disposition() ==
                    ruvia::http_response_stream_head_disposition::body_open);
        RUVIA_CHECK(plan->trailers_available());
    }
    ruvia::http_response duplicate(&resource);
    const auto rejected = server.submit_streaming_response_head(1, std::move(duplicate),
        ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(rejected));
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(rejected) &&
                std::get<ruvia::http2_response_head_submit_error>(rejected) ==
                    ruvia::http2_response_head_submit_error::invalid_state);
    (void)ruvia::testing::drain_output(server);

    const std::array<ruvia::http_header_view, 1> fields_value{{{"x-final", "done"}}};
    const auto trailers = ruvia::validate_http_response_trailers(fields_value);
    RUVIA_CHECK(server.submit_data(1, "body", ruvia::http2_end_stream::keep_open, 65535).status_ ==
                ruvia::http2_data_submit_status::complete);
    RUVIA_CHECK(server.finish_response(1, trailers) ==
                ruvia::http2_finish_status::accepted);
    std::pmr::string frame_bytes(&resource);
    RUVIA_CHECK(server.take_output_batch(1, frame_bytes).status_ == ruvia::http2_output_batch_status::taken);
    const auto trailer_frame = ruvia::parse_http2_frame_header(
        std::span<const char>(frame_bytes.data(), frame_bytes.size()));
    RUVIA_CHECK(trailer_frame && trailer_frame->type_ ==
                                     static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
    frame_bytes.clear();
    RUVIA_CHECK(server.take_output_batch(1, frame_bytes).status_ == ruvia::http2_output_batch_status::taken);
    const auto terminal = ruvia::parse_http2_frame_header(
        std::span<const char>(frame_bytes.data(), frame_bytes.size()));
    RUVIA_CHECK(terminal && terminal->type_ ==
                                static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    RUVIA_CHECK(terminal && (terminal->flags_ & 0x1U) != 0);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)server.finish_response(1, trailers); }));
    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_retried_data_and_trailers_keep_hpack_state_across_window_reopen) {
    for (const auto value_size : {ruvia::http2_default_max_frame_size - 12,
             ruvia::http2_default_max_frame_size - 11}) {
        toggle_allocation_resource resource;
        {
            auto server = ruvia::http2_connection::server({.resource_ = &resource});
            (void)ruvia::testing::drain_output(server);
            const auto wire = server_request_wire(&resource, {}, "GET");
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            const auto end = server.next_event();
            auto* request_head = request ? request->request_head() : nullptr;
            RUVIA_CHECK(request_head != nullptr);
            RUVIA_CHECK(end && end->message_end() != nullptr);
            (void)ruvia::testing::drain_output(server);

            constexpr std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
            std::pmr::string window_settings(&resource);
            append_frame(window_settings, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view(zero_window.data(), zero_window.size()));
            RUVIA_CHECK(server.feed(window_settings) == ruvia::http2_feed_result::accepted);
            RUVIA_CHECK(!server.next_event());
            (void)ruvia::testing::drain_output(server);
            ruvia::http_response response(&resource);
            response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
            const auto committed = server.submit_streaming_response_head(1, std::move(response),
                ruvia::http_response_trailer_intent::present);
            RUVIA_CHECK(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
            (void)ruvia::testing::drain_output(server);
            const auto blocked = server.submit_data(1, "z", ruvia::http2_end_stream::keep_open, 1);
            RUVIA_CHECK(blocked.status_ == ruvia::http2_data_submit_status::backpressured);
            RUVIA_CHECK_EQ(blocked.consumed_bytes_, std::size_t{0});
            std::string value(value_size, 't');
            const auto expected = value;
            const std::array fields{ruvia::http_header_view{"x-tail", value}};
            const auto trailers = ruvia::validate_http_response_trailers(fields);

            constexpr std::array<char, 6> zero_table{0, 1, 0, 0, 0, 0};
            std::pmr::string table_settings(&resource);
            append_frame(table_settings, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view(zero_table.data(), zero_table.size()));
            RUVIA_CHECK(server.feed(table_settings) == ruvia::http2_feed_result::accepted);
            RUVIA_CHECK(!server.next_event());
            (void)ruvia::testing::drain_output(server);
            constexpr std::array<char, 4> increment{0, 0, 0, 1};
            std::pmr::string window_update(&resource);
            append_frame(window_update, ruvia::http2_frame_type::window_update, 0, 1,
                std::string_view(increment.data(), increment.size()));
            RUVIA_CHECK(server.feed(window_update) == ruvia::http2_feed_result::accepted);
            RUVIA_CHECK(!server.next_event());
            RUVIA_CHECK(!server.wants_write());
            if (ruvia::test::stl_allocation_failure_injection_enabled(ruvia_ctx)) {
                resource.reject_after_output(&server);
            }
            const auto submitted = server.submit_data(1, "z", ruvia::http2_end_stream::keep_open, 1);
            resource.reject_after_output(nullptr);
            RUVIA_CHECK(submitted.status_ == ruvia::http2_data_submit_status::complete);
            RUVIA_CHECK_EQ(submitted.consumed_bytes_, std::size_t{1});
            RUVIA_CHECK(server.finish_response(1, trailers) == ruvia::http2_finish_status::accepted);
            value.assign(value_size, 'x');
            const auto output = ruvia::testing::drain_output(server);
            std::string body;
            std::pmr::string encoded(&resource);
            unsigned header_runs = 0;
            bool headers_complete = false;
            for (std::size_t offset = 0; offset < output.size();) {
                const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(output).subspan(offset));
                RUVIA_CHECK(frame.has_value());
                if (!frame) {
                    break;
                }
                const auto payload_offset = offset + ruvia::http2_frame_header_bytes;
                RUVIA_CHECK(frame->length_ <= output.size() - payload_offset);
                if (frame->length_ > output.size() - payload_offset) {
                    break;
                }
                const auto payload = output.substr(payload_offset, frame->length_);
                RUVIA_CHECK_EQ(frame->stream_id_, std::uint32_t{1});
                if (frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    RUVIA_CHECK_EQ(header_runs, 0u);
                    RUVIA_CHECK((frame->flags_ & 1) == 0);
                    body.append(payload);
                } else {
                    RUVIA_CHECK(!headers_complete);
                    if (frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                        ++header_runs;
                        RUVIA_CHECK((frame->flags_ & 1) != 0);
                    } else {
                        RUVIA_CHECK(frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::continuation));
                        RUVIA_CHECK_EQ(header_runs, 1u);
                    }
                    encoded.append(payload);
                    headers_complete = (frame->flags_ & 4) != 0;
                }
                offset = payload_offset + frame->length_;
            }
            RUVIA_CHECK_EQ(body, "z");
            RUVIA_CHECK_EQ(header_runs, 1u);
            RUVIA_CHECK(headers_complete);
            RUVIA_CHECK(encoded.starts_with('\x20'));
            ruvia::hpack_decoder decoder({.resource_ = &resource});
            decoder.set_max_dynamic_table_size(0);
            unsigned field_count = 0;
            const auto decoded = decoder.decode(encoded, [&](std::string_view name, std::string_view field_value) {
                ++field_count;
                RUVIA_CHECK_EQ(name, "x-tail");
                RUVIA_CHECK_EQ(field_value, expected);
                return true;
            });
            RUVIA_CHECK(!decoded.has_value());
            RUVIA_CHECK_EQ(field_count, 1u);
            RUVIA_CHECK(!server.send_window_state(1));
            if (request_head != nullptr) {
                RUVIA_CHECK(server.release(std::move(*request_head)) ==
                            ruvia::http2_server_request_release_status::released);
            }
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(http2_public_streaming_head_response_ends_at_headers) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
    const auto wire = server_request_wire(&resource, {}, "HEAD");
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(end && end->message_end() != nullptr);
    (void)ruvia::testing::drain_output(server);

    ruvia::http_response response(&resource);
    const auto committed = server.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
    RUVIA_CHECK(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed) &&
                std::get<ruvia::http_response_stream_commit_plan>(committed).head_disposition() ==
                    ruvia::http_response_stream_head_disposition::message_ended);
    const auto output = ruvia::testing::drain_output(server);
    const auto frame = ruvia::parse_http2_frame_header(
        std::span<const char>(output.data(), output.size()));
    RUVIA_CHECK(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    RUVIA_CHECK(frame && (frame->flags_ & 0x1U) != 0);
    RUVIA_CHECK(ruvia::testing::throws_on(
        [&] { (void)server.submit_data(1, "", ruvia::http2_end_stream::end_stream, 65535); }));
    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_transformed_coding_ows_preserves_response_framing) {
    // A complete gzip representation of an empty payload; the coded body still
    // has twenty bytes that must be framed independently of its decoded size.
    constexpr std::string_view encoded("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00", 20);
    for (const bool replace_body : {false, true}) {
        for (const std::string_view coding : {" gzip", "gzip ", "\tgzip\t", " \tgzip\t "}) {
            std::pmr::unsynchronized_pool_resource resource;
            auto client = prepared_client(&resource);
            auto server = ruvia::http2_connection::server({.resource_ = &resource});
            const auto wire = server_request_wire(&resource, {}, "GET");
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            RUVIA_CHECK(request && request->request_head());
            const auto request_end = server.next_event();
            RUVIA_CHECK(request_end && request_end->message_end());
            ruvia::http_response response(&resource);
            response.body(replace_body ? "old representation" : encoded);
            response.header("ETag", "\"v1\"");
            response.header("Content-Length", "999");
            if (replace_body) {
                std::pmr::string replacement(encoded, &resource);
                response.replace_body_with_content_encoding(std::move(replacement), coding);
            } else {
                response.apply_content_encoding(coding);
            }
            RUVIA_CHECK(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
            const auto response_wire = ruvia::testing::drain_output(server);
            RUVIA_CHECK(client.feed(response_wire) == ruvia::http2_feed_result::accepted);
            const auto head = client.next_event();
            RUVIA_CHECK(head && head->response_head());
            if (head && head->response_head()) {
                const auto fields = head->response_head()->head().headers();
                const auto find_value = [&](std::string_view name) {
                    const auto found = std::find_if(fields.begin(), fields.end(),
                        [name](const auto& field) { return field.name() == name; });
                    return found == fields.end() ? std::string_view{} : found->value();
                };
                RUVIA_CHECK_EQ(find_value("content-encoding"), "gzip");
                RUVIA_CHECK_EQ(find_value("content-length"), "20");
                RUVIA_CHECK_EQ(find_value("etag"), "W/\"v1\"");
            }
            auto body = client.next_event();
            auto* chunk = body ? body->message_body_chunk() : nullptr;
            RUVIA_CHECK(chunk != nullptr);
            if (chunk != nullptr) {
                RUVIA_CHECK_EQ(chunk->bytes(), encoded);
                auto credit = chunk->take_credit();
                (void)client.acknowledge(std::move(credit));
            }
            const auto end = client.next_event();
            RUVIA_CHECK(end && end->message_end() && end->message_end()->stream_id() == 1);
            RUVIA_CHECK(!client.next_event());
            RUVIA_CHECK(!client.connection_error());
        }
    }
}

RUVIA_TEST(http2_public_empty_buffered_head_matches_get_content_length) {
    for (const std::string_view method : {std::string_view("GET"), std::string_view("HEAD")}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto client = prepared_client(&resource);
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        const auto wire = server_request_wire(&resource, {}, method);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto request = server.next_event();
        RUVIA_CHECK(request && request->request_head());
        const auto request_end = server.next_event();
        RUVIA_CHECK(request_end && request_end->message_end());
        ruvia::http_response response(&resource);
        response.body("");
        const auto plan = ruvia::plan_buffered_http_response_write(
            request->request_head()->build_request().known_method(), response);
        const auto submitted = server.submit_response_head(1, response, plan);
        RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
        (void)server.release(std::move(*request->request_head()));
        const auto response_wire = ruvia::testing::drain_output(server);
        RUVIA_CHECK(client.feed(response_wire) == ruvia::http2_feed_result::accepted);
        const auto head = client.next_event();
        RUVIA_CHECK(head && head->response_head());
        if (head && head->response_head()) {
            const auto fields = head->response_head()->head().headers();
            const auto found = std::find_if(fields.begin(), fields.end(),
                [](const auto& field) { return field.name() == "content-length"; });
            RUVIA_CHECK(found != fields.end());
            if (found != fields.end()) {
                RUVIA_CHECK_EQ(found->value(), "0");
            }
        }
        const auto end = client.next_event();
        RUVIA_CHECK(end && end->message_end() && end->message_end()->stream_id() == 1);
        RUVIA_CHECK(!client.next_event());
        RUVIA_CHECK(!client.connection_error());
    }
}

RUVIA_TEST(http2_public_server_submits_buffered_response_head_and_returns_write_plan) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(end && end->message_end() != nullptr);
    (void)ruvia::testing::drain_output(server);

    ruvia::http_response response(&resource);
    response.body("payload");
    const auto wrong_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::head, response);
    const auto mismatch = server.submit_response_head(1, response, wrong_plan);
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(mismatch));
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(mismatch) &&
                std::get<ruvia::http2_response_head_submit_error>(mismatch) ==
                    ruvia::http2_response_head_submit_error::response_plan_mismatch);
    const auto write_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::post, response);
    const auto submitted = server.submit_response_head(1, response, write_plan);
    RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
    if (const auto* plan = std::get_if<ruvia::http_buffered_response_write_plan>(&submitted)) {
        RUVIA_CHECK(plan->body_plan().response_status() == ruvia::http_status::ok);
        RUVIA_CHECK(plan->send_body());
        RUVIA_CHECK(plan->content_length() == 7);
    }
    const auto head_output = ruvia::testing::drain_output(server);
    const auto head = ruvia::parse_http2_frame_header(
        std::span<const char>(head_output.data(), head_output.size()));
    RUVIA_CHECK(head && head->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    RUVIA_CHECK(head && (head->flags_ & 0x1U) == 0);
    RUVIA_CHECK(server.submit_data(1, "payload", ruvia::http2_end_stream::end_stream, 65535).status_ ==
                ruvia::http2_data_submit_status::complete);

    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_response_head_submit_exposes_shared_failure_values) {
    std::pmr::monotonic_buffer_resource resource;

    auto closed = ruvia::http2_connection::server({.resource_ = &resource});
    ruvia::http_response closed_response(&resource);
    const auto closed_buffered = closed.submit_response_head(1, closed_response,
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, closed_response));
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_buffered));
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_buffered) &&
                std::get<ruvia::http2_response_head_submit_error>(closed_buffered) ==
                    ruvia::http2_response_head_submit_error::closed);
    const auto closed_streaming = closed.submit_streaming_response_head(1,
        ruvia::http_response(&resource),
        ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_streaming));
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_streaming) &&
                std::get<ruvia::http2_response_head_submit_error>(closed_streaming) ==
                    ruvia::http2_response_head_submit_error::closed);

    auto client = prepared_client(&resource);
    ruvia::http_response client_response(&resource);
    const auto invalid_state = client.submit_streaming_response_head(1, std::move(client_response),
        ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_state));
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_state) &&
                std::get<ruvia::http2_response_head_submit_error>(invalid_state) ==
                    ruvia::http2_response_head_submit_error::invalid_state);

    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    RUVIA_CHECK(end && end->message_end() != nullptr);
    (void)ruvia::testing::drain_output(server);

    ruvia::http_response invalid_response(&resource);
    invalid_response.header("Content-Length", "invalid");
    const auto invalid_message = server.submit_streaming_response_head(1,
        std::move(invalid_response),
        ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_message));
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_message) &&
                std::get<ruvia::http2_response_head_submit_error>(invalid_message) ==
                    ruvia::http2_response_head_submit_error::invalid_message);
    RUVIA_CHECK(!server.wants_write());
    RUVIA_CHECK(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_response_head_validates_stable_representation_metadata) {
    for (const auto invalid : {ruvia::http_header_view{"Content-Type", "text plain"},
             ruvia::http_header_view{"Content-Encoding", "gzip;q=1"},
             ruvia::http_header_view{"Trailer", "Content-Length"}}) {
        for (const bool streaming : {false, true}) {
            std::pmr::monotonic_buffer_resource resource;
            auto server = ruvia::http2_connection::server({.resource_ = &resource});
            (void)ruvia::testing::drain_output(server);
            const auto wire = server_request_wire(&resource, {});
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            const auto end = server.next_event();
            auto* request_head = request ? request->request_head() : nullptr;
            RUVIA_CHECK(request_head != nullptr);
            RUVIA_CHECK(end && end->message_end() != nullptr);
            (void)ruvia::testing::drain_output(server);

            ruvia::http_response response(&resource);
            try {
                response.header_stable_view(invalid.name(), invalid.value());
            } catch (const std::invalid_argument&) {
                continue;
            }
            if (streaming) {
                const auto rejected = server.submit_streaming_response_head(1, std::move(response),
                    ruvia::http_response_trailer_intent::none);
                RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(rejected));
                if (const auto* err = std::get_if<ruvia::http2_response_head_submit_error>(&rejected)) {
                    RUVIA_CHECK(*err == ruvia::http2_response_head_submit_error::invalid_message);
                }
            } else {
                const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::post, response);
                const auto rejected = server.submit_response_head(1, response, plan);
                RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(rejected));
                if (const auto* err = std::get_if<ruvia::http2_response_head_submit_error>(&rejected)) {
                    RUVIA_CHECK(*err == ruvia::http2_response_head_submit_error::invalid_message);
                }
            }
            RUVIA_CHECK(!server.wants_write());
            if (request_head != nullptr) {
                RUVIA_CHECK(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
            }
        }
    }
}

// The handler's `chunked` Transfer-Encoding is the framework-owned framing hint
// on HTTP/2 as on HTTP/1: the writer omits it, so the peer receives a valid
// head instead of a connection-specific field (RFC 9113 section 8.2.2).
RUVIA_TEST(http2_public_response_head_omits_chunked_transfer_encoding_hint) {
    for (const auto status : {ruvia::http_status::ok, ruvia::http_status::no_content, ruvia::http_status::not_modified}) {
        for (const bool streaming : {false, true}) {
            std::pmr::unsynchronized_pool_resource resource;
            auto client = prepared_client(&resource);
            auto server = ruvia::http2_connection::server({.resource_ = &resource});
            const auto wire = server_request_wire(&resource, {}, "GET");
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            const auto request_end = server.next_event();
            auto* request_head = request ? request->request_head() : nullptr;
            RUVIA_CHECK(request_head != nullptr);
            RUVIA_CHECK(request_end && request_end->message_end() != nullptr);

            ruvia::http_response response(&resource);
            response.status(status);
            response.header("Transfer-Encoding", "ChUnKeD");
            if (streaming) {
                const auto committed = server.submit_streaming_response_head(1, std::move(response),
                    ruvia::http_response_trailer_intent::none);
                const auto* plan = std::get_if<ruvia::http_response_stream_commit_plan>(&committed);
                RUVIA_CHECK(plan != nullptr);
                if (plan != nullptr &&
                    plan->head_disposition() == ruvia::http_response_stream_head_disposition::body_open) {
                    RUVIA_CHECK(server.submit_data(1, {}, ruvia::http2_end_stream::end_stream, 65535).status_ ==
                                ruvia::http2_data_submit_status::complete);
                }
            } else {
                const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
                RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
                    server.submit_response_head(1, response, plan)));
            }
            if (request_head != nullptr) {
                (void)server.release(std::move(*request_head));
            }

            const auto response_wire = ruvia::testing::drain_output(server);
            RUVIA_CHECK(client.feed(response_wire) == ruvia::http2_feed_result::accepted);
            const auto head = client.next_event();
            RUVIA_CHECK(head && head->response_head());
            if (head && head->response_head()) {
                const auto fields = head->response_head()->head().headers();
                RUVIA_CHECK(std::none_of(fields.begin(), fields.end(),
                    [](const auto& field) { return field.name() == "transfer-encoding"; }));
            }
            const auto end = client.next_event();
            RUVIA_CHECK(end && end->message_end() && end->message_end()->stream_id() == 1);
            RUVIA_CHECK(!client.connection_error());
        }
    }
}

// Any other Transfer-Encoding claims a coding the writer never applies and
// stays a rejected connection-specific field, with nothing queued.
RUVIA_TEST(http2_public_response_head_rejects_transfer_encoding_other_than_chunked) {
    for (const std::string_view coding : {"gzip", "gzip, chunked", "identity", "", "chunked, chunked"}) {
        for (const bool streaming : {false, true}) {
            std::pmr::monotonic_buffer_resource resource;
            auto server = ruvia::http2_connection::server({.resource_ = &resource});
            (void)ruvia::testing::drain_output(server);
            const auto wire = server_request_wire(&resource, {});
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            const auto end = server.next_event();
            auto* request_head = request ? request->request_head() : nullptr;
            RUVIA_CHECK(request_head != nullptr);
            RUVIA_CHECK(end && end->message_end() != nullptr);
            (void)ruvia::testing::drain_output(server);

            ruvia::http_response response(&resource);
            response.header("Transfer-Encoding", coding);
            std::optional<ruvia::http2_response_head_submit_error> error;
            if (streaming) {
                const auto rejected = server.submit_streaming_response_head(1, std::move(response),
                    ruvia::http_response_trailer_intent::none);
                if (const auto* value = std::get_if<ruvia::http2_response_head_submit_error>(&rejected)) {
                    error = *value;
                }
            } else {
                const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::post, response);
                const auto rejected = server.submit_response_head(1, response, plan);
                if (const auto* value = std::get_if<ruvia::http2_response_head_submit_error>(&rejected)) {
                    error = *value;
                }
            }
            RUVIA_CHECK(error == ruvia::http2_response_head_submit_error::invalid_message);
            RUVIA_CHECK(!server.wants_write());
            if (request_head != nullptr) {
                RUVIA_CHECK(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
            }
        }
    }
}

RUVIA_TEST(http2_public_dropped_request_preserves_outstanding_data_credit) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
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
    (void)ruvia::testing::drain_output(server);
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    (void)ruvia::testing::drain_output(server);

    {
        const auto request = server.next_event();
        RUVIA_CHECK(request && request->request_head() != nullptr);
    }

    const auto output = ruvia::testing::drain_output(server);
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
    ruvia::testing::drain_output(server, initial_output);
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head() != nullptr);
    resource.reject();
    request.reset();

    resource.reject(false);
    // Getters never mutate; the writer's next output drain retries the release.
    std::pmr::string output(&resource);
    ruvia::testing::drain_output(server, output);
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
        (void)ruvia::testing::drain_output(server);
        const auto wire = server_request_wire(&resource, {});
        (void)server.feed(wire);
        return server.next_event();
    }();
    RUVIA_CHECK(escaped && escaped->request_head() != nullptr);

    auto other = ruvia::http2_connection::server({.resource_ = &resource});
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)other.release(std::move(*escaped->request_head())); }));
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
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)other.acknowledge(std::move(escaped_credit)); }));
    RUVIA_CHECK(escaped_credit.valid());
}

RUVIA_TEST(http2_public_request_views_survive_connection_move_assignment) {
    auto* resource = std::pmr::new_delete_resource();
    // Keep caller-owned input alive so this test isolates the connection's decoded storage.
    const auto wire = server_request_wire(resource, "x");
    std::optional<ruvia::http2_event> escaped_request;
    std::optional<ruvia::http2_event> escaped_chunk;

    auto server = ruvia::http2_connection::server({.resource_ = resource});
    (void)ruvia::testing::drain_output(server);
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
        const auto materialized = request_head->build_request(resource);
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
    (void)ruvia::testing::drain_output(server);
    const auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    const auto head_event = server.next_event();
    RUVIA_CHECK(head_event && head_event->request_head() != nullptr);
    resource.reject();
    bool threw = false;
    try {
        (void)head_event->request_head()->build_request(&resource);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);

    resource.reject(false);
    const auto retried = head_event->request_head()->build_request(&resource);
    RUVIA_CHECK(retried.method() == "POST");
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

RUVIA_TEST(http2_public_output_drain_preserves_control_limits_on_copy_failure) {
    for (const auto type : {ruvia::http2_frame_type::ping, ruvia::http2_frame_type::settings}) {
        for (const bool reject_copy : {false, true}) {
            std::pmr::unsynchronized_pool_resource resource;
            auto client = ruvia::http2_connection::client({.resource_ = &resource});
            std::pmr::string settings(&resource);
            append_peer_settings(settings);
            RUVIA_CHECK(client.feed(settings) == ruvia::http2_feed_result::accepted);
            (void)ruvia::testing::drain_output(client);

            std::pmr::string frame(&resource);
            append_frame(frame, type, 0, 0,
                type == ruvia::http2_frame_type::ping ? std::string_view("12345678") : std::string_view{});
            for (std::size_t count = 0; count < 1000; ++count) {
                RUVIA_CHECK(client.feed(frame) == ruvia::http2_feed_result::accepted);
            }
            std::pmr::string pending(&resource);
            for (std::size_t count = 0; count < 1000; ++count) {
                append_frame(pending, type, 0x1, 0,
                    type == ruvia::http2_frame_type::ping ? std::string_view("12345678") : std::string_view{});
            }

            toggle_allocation_resource output_resource;
            std::pmr::string output(&output_resource);
            output_resource.reject(reject_copy);
            bool threw = false;
            try {
                ruvia::testing::drain_output(client, output);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            RUVIA_CHECK(threw == reject_copy);
            if (reject_copy) {
                RUVIA_CHECK(output.empty());
                // The undrained ACKs still count against the control-frame limit.
                RUVIA_CHECK(client.wants_write());
                RUVIA_CHECK(client.feed(frame) == ruvia::http2_feed_result::protocol_failure);
                RUVIA_CHECK(client.connection_error() == ruvia::http2_error_code::enhance_your_calm);
            } else {
                RUVIA_CHECK(std::string_view(output) == pending);
                RUVIA_CHECK(!client.wants_write());
                for (std::size_t count = 0; count < 1000; ++count) {
                    RUVIA_CHECK(client.feed(frame) == ruvia::http2_feed_result::accepted);
                }
                RUVIA_CHECK(!client.connection_error());
                RUVIA_CHECK(client.feed(frame) == ruvia::http2_feed_result::protocol_failure);
                RUVIA_CHECK(client.connection_error() == ruvia::http2_error_code::enhance_your_calm);
            }
        }
    }
}

namespace {

ruvia::http2_connection started_server(std::pmr::memory_resource* resource) {
    auto server = ruvia::http2_connection::server({.resource_ = resource});
    std::pmr::string handshake(ruvia::http2_client_preface, resource);
    append_peer_settings(handshake);
    if (server.feed(handshake) != ruvia::http2_feed_result::accepted) {
        throw std::logic_error("test HTTP/2 handshake failed");
    }
    (void)ruvia::testing::drain_output(server);
    return server;
}

// A complete GET head that leaves the request stream open.
void append_open_get_request(std::pmr::string& wire, std::pmr::memory_resource* resource, std::uint32_t stream_id) {
    std::pmr::string block(resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, stream_id, block);
}

[[nodiscard]] std::string_view ping_payload(std::string_view frames) {
    while (frames.size() >= ruvia::http2_frame_header_bytes) {
        const auto header = ruvia::parse_http2_frame_header(std::span<const char>(frames.data(), frames.size()));
        if (!header) {
            break;
        }
        const auto payload = frames.substr(ruvia::http2_frame_header_bytes, header->length_);
        if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::ping) && header->flags_ == 0) {
            return payload;
        }
        frames.remove_prefix(ruvia::http2_frame_header_bytes + header->length_);
    }
    return {};
}

}  // namespace

RUVIA_TEST(http2_public_peer_induced_stream_errors_end_in_enhance_your_calm) {
    // Each round opens a fresh stream and provokes a stream error with a trailer
    // section that lacks END_STREAM (malformed, RFC 9113 §8.1). Drained output
    // leaves only the lifetime budget (1024 resets); undrained resets trip the
    // control-frame budget first.
    for (const bool drain_each_round : {true, false}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto server = started_server(&resource);
        const std::size_t admitted_resets = drain_each_round ? 1024 : 1000;
        std::pmr::string trailer(&resource);
        ruvia::hpack_encoder::encode_header(trailer, "x-trailer", "1");
        std::uint32_t stream_id = 1;
        for (std::size_t round = 0; round <= admitted_resets; ++round, stream_id += 2) {
            std::pmr::string wire(&resource);
            append_open_get_request(wire, &resource, stream_id);
            append_frame(wire, ruvia::http2_frame_type::headers, 0x4, stream_id, trailer);
            const auto result = server.feed(wire);
            while (server.next_event()) {
            }
            if (round < admitted_resets) {
                RUVIA_CHECK(result == ruvia::http2_feed_result::accepted);
                RUVIA_CHECK(!server.connection_error());
            } else {
                RUVIA_CHECK(result == ruvia::http2_feed_result::protocol_failure);
                RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::enhance_your_calm);
            }
            if (drain_each_round) {
                (void)ruvia::testing::drain_output(server);
            }
        }
    }
}

RUVIA_TEST(http2_public_owner_resets_and_drained_pings_do_not_consume_peer_error_budgets) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = started_server(&resource);
    std::uint32_t stream_id = 1;
    for (std::size_t round = 0; round != 1100; ++round, stream_id += 2) {
        std::pmr::string wire(&resource);
        append_open_get_request(wire, &resource, stream_id);
        append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, "keepaliv");
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        RUVIA_CHECK(request != nullptr);
        if (request == nullptr) {
            return;
        }
        RUVIA_CHECK(server.submit_reset(stream_id, ruvia::http2_error_code::cancel) ==
                    ruvia::http2_submit_status::accepted);
        RUVIA_CHECK(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
        while (server.next_event()) {
        }
        (void)ruvia::testing::drain_output(server);
    }
    RUVIA_CHECK(!server.connection_error());
}

RUVIA_TEST(http2_public_submit_ping_keeps_one_liveness_probe_outstanding) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = started_server(&resource);

    RUVIA_CHECK(server.submit_ping() == ruvia::http2_submit_status::accepted);
    const auto probe = ruvia::testing::drain_output(server);
    RUVIA_CHECK_EQ(probe.size(), ruvia::http2_frame_header_bytes + 8);
    const auto payload = ping_payload(probe);
    RUVIA_CHECK_EQ(payload.size(), std::size_t{8});

    // At most one probe is outstanding until its ACK arrives.
    RUVIA_CHECK(server.submit_ping() == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(!server.wants_write());

    std::pmr::string ack(&resource);
    append_frame(ack, ruvia::http2_frame_type::ping, 0x1, 0, payload);
    RUVIA_CHECK(server.feed(ack) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.next_event());
    RUVIA_CHECK(!server.draining());
    RUVIA_CHECK(server.submit_ping() == ruvia::http2_submit_status::accepted);
    RUVIA_CHECK(ruvia::testing::drain_output(server) == probe);

    // The graceful-drain PING carries a different payload, so neither ACK can
    // complete the other's round trip.
    server.begin_drain();
    const auto drain_frames = ruvia::testing::drain_output(server);
    const auto drain_payload = ping_payload(drain_frames);
    RUVIA_CHECK_EQ(drain_payload.size(), std::size_t{8});
    RUVIA_CHECK(drain_payload != payload);

    std::pmr::string invalid(&resource);
    append_frame(invalid, ruvia::http2_frame_type::ping, 0, 1, "12345678");
    RUVIA_CHECK(server.feed(invalid) == ruvia::http2_feed_result::protocol_failure);
    RUVIA_CHECK(server.submit_ping() == ruvia::http2_submit_status::closed);
}

RUVIA_TEST(http2_public_bounds_malformed_request_stream_resets_per_connection) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);

    std::pmr::string handshake(ruvia::http2_client_preface, &resource);
    append_peer_settings(handshake);
    RUVIA_CHECK(server.feed(handshake) == ruvia::http2_feed_result::accepted);
    (void)ruvia::testing::drain_output(server);

    std::pmr::string malformed(&resource);
    ruvia::hpack_encoder::encode_header(malformed, ":method", "GET");
    ruvia::hpack_encoder::encode_header(malformed, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(malformed, ":authority", "example.test");
    std::uint32_t next_stream_id = 1;
    for (std::uint32_t index = 0; index != 32; ++index) {
        std::pmr::string frame(&resource);
        append_frame(frame, ruvia::http2_frame_type::headers, 0x5, next_stream_id, malformed);
        next_stream_id += 2;
        const auto result = server.feed(frame);
        while (server.next_event()) {
        }
        (void)ruvia::testing::drain_output(server);
        if (index + 1 == 32) {
            RUVIA_CHECK(result == ruvia::http2_feed_result::protocol_failure);
            RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::enhance_your_calm);
        } else {
            RUVIA_CHECK(result == ruvia::http2_feed_result::accepted);
            RUVIA_CHECK(!server.connection_error());
        }

        if (index == 15) {
            std::pmr::string valid(malformed, &resource);
            ruvia::hpack_encoder::encode_header(valid, ":path", "/");
            std::pmr::string valid_frame(&resource);
            append_frame(valid_frame, ruvia::http2_frame_type::headers, 0x5, next_stream_id, valid);
            const auto valid_stream_id = next_stream_id;
            next_stream_id += 2;
            RUVIA_CHECK(server.feed(valid_frame) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            RUVIA_CHECK(request && request->request_head());
            const auto end = server.next_event();
            RUVIA_CHECK(end && end->message_end());
            if (request && request->request_head()) {
                ruvia::http_response response(&resource);
                response.status(ruvia::http_status::no_content);
                const auto submitted = server.submit_response_head(valid_stream_id, response,
                    ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
                RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
                (void)server.release(std::move(*request->request_head()));
            }
            (void)ruvia::testing::drain_output(server);
        }
    }
}

RUVIA_TEST(http2_public_malformed_response_streams_do_not_consume_server_abuse_budget) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    std::pmr::string settings(&resource);
    append_peer_settings(settings);
    RUVIA_CHECK(client.feed(settings) == ruvia::http2_feed_result::accepted);

    std::array<std::uint32_t, 32> stream_ids{};
    for (auto& stream_id : stream_ids) {
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
        if (const auto* id = std::get_if<std::uint32_t>(&submitted)) {
            stream_id = *id;
        }
    }
    (void)ruvia::testing::drain_output(client);

    std::pmr::string malformed(&resource);
    ruvia::hpack_encoder::encode_header(malformed, "content-type", "text/plain");
    for (const auto stream_id : stream_ids) {
        std::pmr::string frame(&resource);
        append_frame(frame, ruvia::http2_frame_type::headers, 0x5, stream_id, malformed);
        RUVIA_CHECK(client.feed(frame) == ruvia::http2_feed_result::accepted);
        while (client.next_event()) {
        }
        (void)ruvia::testing::drain_output(client);
        RUVIA_CHECK(!client.connection_error());
    }
}

RUVIA_TEST(http2_public_data_submission_stops_after_connection_failure) {
    for (const bool blocked_window : {false, true}) {
        for (const auto end_stream : {ruvia::http2_end_stream::keep_open, ruvia::http2_end_stream::end_stream}) {
            std::pmr::unsynchronized_pool_resource resource;
            auto client = ruvia::http2_connection::client({.resource_ = &resource});
            std::pmr::string settings(&resource);
            constexpr std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
            append_frame(settings, ruvia::http2_frame_type::settings, 0, 0,
                blocked_window ? std::string_view(zero_window.data(), zero_window.size()) : std::string_view{});
            RUVIA_CHECK(client.feed(settings) == ruvia::http2_feed_result::accepted);
            const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
                .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http2_request_content::streaming()});
            RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
            if (!std::holds_alternative<std::uint32_t>(submitted)) {
                continue;
            }
            const auto stream_id = std::get<std::uint32_t>(submitted);
            (void)ruvia::testing::drain_output(client);

            std::pmr::string invalid_ping(&resource);
            append_frame(invalid_ping, ruvia::http2_frame_type::ping, 0, 0, {});
            RUVIA_CHECK(client.feed(invalid_ping) == ruvia::http2_feed_result::protocol_failure);
            RUVIA_CHECK(client.connection_error() == ruvia::http2_error_code::frame_size_error);
            (void)ruvia::testing::drain_output(client);
            const auto before = client.send_window_state(stream_id);
            const auto payload = end_stream == ruvia::http2_end_stream::end_stream ? std::string_view{} : std::string_view("content");
            RUVIA_CHECK(client.submit_data(stream_id, payload, end_stream, 65535).status_ == ruvia::http2_data_submit_status::closed);
            RUVIA_CHECK(client.finish_request(stream_id) == ruvia::http2_finish_status::closed);
            RUVIA_CHECK(!client.wants_write());
            RUVIA_CHECK_EQ(client.pending_data_output_bytes(stream_id), std::size_t{0});
            const auto after = client.send_window_state(stream_id);
            if (before && after) {
                RUVIA_CHECK_EQ(after->connection_window_, before->connection_window_);
                RUVIA_CHECK_EQ(after->stream_window_, before->stream_window_);
            }
        }
    }
}

RUVIA_TEST(http2_public_data_submission_continues_existing_stream_during_graceful_drain) {
    for (const bool peer_goaway : {false, true}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto client = ruvia::http2_connection::client({.resource_ = &resource});
        std::pmr::string settings(&resource);
        append_peer_settings(settings);
        RUVIA_CHECK(client.feed(settings) == ruvia::http2_feed_result::accepted);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http2_request_content::streaming()});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            continue;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        if (peer_goaway) {
            constexpr std::array<char, 8> payload{0, 0, 0, 1, 0, 0, 0, 0};
            std::pmr::string goaway(&resource);
            append_frame(goaway, ruvia::http2_frame_type::goaway, 0, 0,
                std::string_view(payload.data(), payload.size()));
            RUVIA_CHECK(client.feed(goaway) == ruvia::http2_feed_result::accepted);
        } else {
            client.begin_drain();
        }
        RUVIA_CHECK(!client.connection_error());
        (void)ruvia::testing::drain_output(client);
        RUVIA_CHECK(client.submit_data(stream_id, "content", ruvia::http2_end_stream::end_stream, 65535).status_ ==
                    ruvia::http2_data_submit_status::complete);
        RUVIA_CHECK_EQ(client.pending_data_output_bytes(stream_id), std::size_t{7});
    }
}

RUVIA_TEST(http2_public_response_submission_stops_after_connection_failure) {
    for (const bool body_open : {false, true}) {
        toggle_allocation_resource resource;
        {
            auto server = ruvia::http2_connection::server({.resource_ = &resource});
            const auto wire = server_request_wire(&resource, {});
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            auto* head = request ? request->request_head() : nullptr;
            RUVIA_CHECK(head != nullptr);
            if (head == nullptr) {
                continue;
            }
            while (server.next_event()) {
            }
            if (body_open) {
                ruvia::http_response response(&resource);
                RUVIA_CHECK(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
                    server.submit_streaming_response_head(1, std::move(response))));
            }
            ruvia::http_response buffered(&resource);
            buffered.body("response");
            const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::post, buffered);
            ruvia::http_response streamed(&resource);
            const ruvia::http_interim_response_head interim(ruvia::http_status::continue_value);
            const std::array fields{ruvia::http_header_view{"x-final", "done"}};
            const auto trailers = ruvia::validate_http_response_trailers(fields);
            const auto built_request = head->build_request(&resource);
            const auto validation = ruvia::validate_http2_websocket_handshake(server, 1, built_request);
            (void)ruvia::testing::drain_output(server);
            std::pmr::string invalid_ping(&resource);
            append_frame(invalid_ping, ruvia::http2_frame_type::ping, 0, 0, {});
            RUVIA_CHECK(server.feed(invalid_ping) == ruvia::http2_feed_result::protocol_failure);
            (void)ruvia::testing::drain_output(server);

            if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
                resource.reject();
            }
            const auto attempts = resource.allocation_attempts();
            const auto buffered_head = server.submit_response_head(1, buffered, plan);
            RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(buffered_head) &&
                        std::get<ruvia::http2_response_head_submit_error>(buffered_head) ==
                            ruvia::http2_response_head_submit_error::closed);
            const auto streamed_head = server.submit_streaming_response_head(1, std::move(streamed),
                ruvia::http_response_trailer_intent::none);
            RUVIA_CHECK(std::holds_alternative<ruvia::http2_response_head_submit_error>(streamed_head) &&
                        std::get<ruvia::http2_response_head_submit_error>(streamed_head) ==
                            ruvia::http2_response_head_submit_error::closed);
            RUVIA_CHECK(submit_buffered_response(server, 1, buffered) == ruvia::http2_submit_status::closed);
            RUVIA_CHECK(server.submit_connect_response_head(1, buffered) == ruvia::http2_submit_status::closed);
            RUVIA_CHECK(server.submit_interim_response_head(1, interim) == ruvia::http2_submit_status::closed);
            RUVIA_CHECK(server.finish_response(1, trailers) == ruvia::http2_finish_status::closed);
            const auto websocket = server.submit_websocket_handshake(1, built_request, validation, {});
            RUVIA_CHECK(std::holds_alternative<ruvia::http2_websocket_handshake_submit_error>(websocket) &&
                        std::get<ruvia::http2_websocket_handshake_submit_error>(websocket) ==
                            ruvia::http2_websocket_handshake_submit_error::closed);
            RUVIA_CHECK_EQ(head->request_view().path_, std::string_view("/upload"));
            RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
            RUVIA_CHECK(!server.wants_write());
            if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
                RUVIA_CHECK_EQ(resource.allocation_attempts(), attempts);
            }
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(http2_public_failed_connection_releases_request_and_data_credit_without_output) {
    for (const bool release_first : {false, true}) {
        toggle_allocation_resource resource;
        {
            auto server = ruvia::http2_connection::server({.resource_ = &resource});
            std::pmr::string block(&resource);
            ruvia::hpack_encoder::encode_header(block, ":method", "POST");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
            ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
            ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
            std::pmr::string wire(ruvia::http2_client_preface, &resource);
            append_peer_settings(wire);
            // Acknowledged SETTINGS put the advertised 1 MiB stream window in force.
            append_frame(wire, ruvia::http2_frame_type::settings, 0x1, 0, {});
            append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
            const std::string payload(16384, 'x');
            for (std::size_t index = 0; index < 32; ++index) {
                append_frame(wire, ruvia::http2_frame_type::data, 0, 1, payload);
            }
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            auto* head = request ? request->request_head() : nullptr;
            RUVIA_CHECK(head != nullptr);
            if (head == nullptr) {
                continue;
            }
            std::optional<ruvia::http2_received_data_credit> credit;
            while (auto event = server.next_event()) {
                if (auto* body = event->message_body_chunk()) {
                    if (credit) {
                        credit->merge(body->take_credit());
                    } else {
                        credit.emplace(body->take_credit());
                    }
                }
            }
            RUVIA_CHECK(credit && credit->valid());
            if (!credit) {
                continue;
            }
            (void)ruvia::testing::drain_output(server);
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {});
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
            (void)ruvia::testing::drain_output(server);
            resource.reject();
            const auto attempts = resource.allocation_attempts();
            if (release_first) {
                RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
            } else {
                RUVIA_CHECK(server.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
            }
            RUVIA_CHECK(server.acknowledge(std::move(*credit)) ==
                        ruvia::http2_received_data_acknowledge_status::acknowledged);
            if (!release_first) {
                RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
            }
            RUVIA_CHECK(!credit->valid());
            RUVIA_CHECK(server.stream_aborted(1));
            RUVIA_CHECK(!server.wants_write());
            RUVIA_CHECK_EQ(resource.allocation_attempts(), attempts);
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(http2_public_failed_connection_retires_peer_finished_tunnel) {
    toggle_allocation_resource resource;
    {
        auto client = ruvia::http2_connection::client({.resource_ = &resource});
        const auto submitted = client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        std::pmr::string wire(&resource);
        append_peer_settings(wire);
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, stream_id, block);
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, stream_id, {});
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        RUVIA_CHECK(client.next_event().has_value());
        const auto ended = client.next_event();
        RUVIA_CHECK(ended && ended->tunnel_end());
        RUVIA_CHECK(client.send_window_state(stream_id).has_value());
        (void)ruvia::testing::drain_output(client);
        wire.clear();
        append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {});
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        const auto terminal_output = ruvia::testing::drain_output(client);
        resource.reject();
        const auto attempts = resource.allocation_attempts();
        const auto frame = ruvia::parse_http2_frame_header(std::span(terminal_output.data(), terminal_output.size()));
        RUVIA_CHECK(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        if (frame) {
            RUVIA_CHECK_EQ(terminal_output.size(), ruvia::http2_frame_header_bytes + frame->length_);
        }
        RUVIA_CHECK(!client.send_window_state(stream_id));
        RUVIA_CHECK(client.stream_aborted(stream_id));
        RUVIA_CHECK(!client.wants_write());
        RUVIA_CHECK_EQ(resource.allocation_attempts(), attempts);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http2_public_client_accepts_long_informational_sequence_before_final_response) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client(&resource);
    std::pmr::string wire(&resource);
    append_peer_settings(wire);
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_header(block, ":status", "103");
    ruvia::hpack_encoder::encode_header(block, "link", "</asset>; rel=preload");
    constexpr std::size_t informational_count = 300;
    for (std::size_t index = 0; index < informational_count; ++index) {
        append_header_section(wire, 1, block, false, false);
    }
    append_response(wire, &resource, 1, "200", "final body");
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    for (std::size_t index = 0; index < informational_count; ++index) {
        const auto event = client.next_event();
        const auto* informational = event ? event->informational_head() : nullptr;
        RUVIA_CHECK(informational != nullptr);
        if (informational == nullptr) {
            return;
        }
        RUVIA_CHECK_EQ(informational->stream_id(), std::uint32_t{1});
        RUVIA_CHECK_EQ(informational->head().status().value(), std::uint16_t{103});
        const auto fields = informational->head().headers();
        RUVIA_CHECK_EQ(fields.size(), std::size_t{1});
        if (fields.size() == 1) {
            RUVIA_CHECK(fields.front().name() == "link");
            RUVIA_CHECK(fields.front().value() == "</asset>; rel=preload");
        }
    }
    const auto head = client.next_event();
    RUVIA_CHECK(head && head->response_head());
    if (head && head->response_head()) {
        RUVIA_CHECK_EQ(head->response_head()->stream_id(), std::uint32_t{1});
        RUVIA_CHECK(head->response_head()->head().status() == ruvia::http_status::ok);
    }
    const auto body = client.next_event();
    RUVIA_CHECK(body && body->message_body_chunk());
    if (body && body->message_body_chunk()) {
        RUVIA_CHECK_EQ(body->message_body_chunk()->stream_id(), std::uint32_t{1});
        RUVIA_CHECK(body->message_body_chunk()->bytes() == "final body");
    }
    const auto end = client.next_event();
    RUVIA_CHECK(end && end->message_end());
    if (end && end->message_end()) {
        RUVIA_CHECK_EQ(end->message_end()->stream_id(), std::uint32_t{1});
        RUVIA_CHECK(end->message_end()->trailers().empty());
        RUVIA_CHECK(end->message_end()->content_semantics() ==
                    ruvia::http2_message_content_semantics::content);
    }
    RUVIA_CHECK(!client.next_event());
    RUVIA_CHECK(!client.connection_error());
}

RUVIA_TEST(http2_public_informational_metadata_does_not_frame_or_leak_into_final_responses) {
    for (const std::string_view length : {"0", "123"}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto client = prepared_client(&resource);
        const auto following = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .authority_ = "example.test"});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(following) && std::get<std::uint32_t>(following) == 3);
        std::pmr::string wire(&resource);
        append_peer_settings(wire);
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":status", "103");
        ruvia::hpack_encoder::encode_header(block, "content-length", length);
        ruvia::hpack_encoder::encode_header(block, "trailer", "x-checksum");
        ruvia::hpack_encoder::encode_header(block, "date", "Sun, 06 Nov 1994 08:49:37 GMT");
        ruvia::hpack_encoder::encode_header(block, "date", "Mon, 07 Nov 1994 08:49:37 GMT");
        append_header_section(wire, 1, block, false, true);
        append_response(wire, &resource, 1, "200", "abc");
        append_response(wire, &resource, 3, "200", "next");
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        const auto event = client.next_event();
        const auto* info = event ? event->informational_head() : nullptr;
        RUVIA_CHECK(info != nullptr);
        if (info == nullptr) {
            continue;
        }
        RUVIA_CHECK_EQ(info->stream_id(), std::uint32_t{1});
        RUVIA_CHECK_EQ(info->head().status().value(), std::uint16_t{103});
        const auto fields = info->head().headers();
        RUVIA_CHECK_EQ(fields.size(), std::size_t{4});
        if (fields.size() != 4) {
            continue;
        }
        RUVIA_CHECK_EQ(fields[0].name(), "content-length");
        RUVIA_CHECK_EQ(fields[0].value(), length);
        RUVIA_CHECK_EQ(fields[1].name(), "trailer");
        RUVIA_CHECK_EQ(fields[1].value(), "x-checksum");
        const auto consume_response = [&](std::uint32_t stream_id, std::string_view expected) {
            const auto head = client.next_event();
            RUVIA_CHECK(head && head->response_head());
            if (head && head->response_head()) {
                RUVIA_CHECK_EQ(head->response_head()->stream_id(), stream_id);
                RUVIA_CHECK(head->response_head()->head().status() == ruvia::http_status::ok);
                RUVIA_CHECK(head->response_head()->head().headers().empty());
            }
            auto body = client.next_event();
            auto* chunk = body ? body->message_body_chunk() : nullptr;
            RUVIA_CHECK(chunk != nullptr);
            if (chunk != nullptr) {
                RUVIA_CHECK_EQ(chunk->stream_id(), stream_id);
                RUVIA_CHECK_EQ(chunk->bytes(), expected);
                auto credit = chunk->take_credit();
                (void)client.acknowledge(std::move(credit));
            }
            const auto end = client.next_event();
            RUVIA_CHECK(end && end->message_end() && end->message_end()->stream_id() == stream_id);
            if (end && end->message_end()) {
                RUVIA_CHECK(end->message_end()->trailers().empty());
            }
        };
        consume_response(1, "abc");
        consume_response(3, "next");
        RUVIA_CHECK_EQ(fields[2].name(), "date");
        RUVIA_CHECK_EQ(fields[2].value(), "Sun, 06 Nov 1994 08:49:37 GMT");
        RUVIA_CHECK_EQ(fields[3].name(), "date");
        RUVIA_CHECK_EQ(fields[3].value(), "Mon, 07 Nov 1994 08:49:37 GMT");
        RUVIA_CHECK(!client.next_event());
        RUVIA_CHECK(!client.connection_error());
    }
}

RUVIA_TEST(http2_public_client_rejects_goaway_excluding_informational_response) {
    for (const std::size_t informational_count : {std::size_t{1}, std::size_t{256}}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = prepared_client(&resource);
        std::pmr::string wire(&resource);
        append_peer_settings(wire);
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":status", "103");
        for (std::size_t index = 0; index < informational_count; ++index) {
            append_header_section(wire, 1, block, false, false);
        }
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        for (std::size_t index = 0; index < informational_count; ++index) {
            const auto event = client.next_event();
            const auto* informational = event ? event->informational_head() : nullptr;
            RUVIA_CHECK(informational != nullptr);
            if (informational == nullptr) {
                return;
            }
            RUVIA_CHECK_EQ(informational->stream_id(), std::uint32_t{1});
            RUVIA_CHECK_EQ(informational->head().status().value(), std::uint16_t{103});
        }
        RUVIA_CHECK(!client.next_event());
        RUVIA_CHECK(!client.connection_error());
        (void)ruvia::testing::drain_output(client);
        wire.clear();
        constexpr std::array<char, 8> payload{};
        append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0,
            std::string_view(payload.data(), payload.size()));
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        RUVIA_CHECK(client.connection_error() == ruvia::http2_error_code::protocol_error);
        // A request_unprocessed event would incorrectly authorize replay.
        RUVIA_CHECK(!client.next_event());
        const auto output = ruvia::testing::drain_output(client);
        const auto frame = ruvia::parse_http2_frame_header(std::span(output.data(), output.size()));
        RUVIA_CHECK(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
    }
}

RUVIA_TEST(http2_public_client_rejects_informational_response_with_end_stream) {
    for (const auto status : {std::string_view("100"), std::string_view("103")}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = prepared_client(&resource);
        std::pmr::string wire(&resource);
        append_peer_settings(wire);
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":status", status);
        append_header_section(wire, 1, block, true, false);
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        const auto event = client.next_event();
        const auto* closed = event ? event->stream_closed() : nullptr;
        RUVIA_CHECK(closed != nullptr);
        if (closed != nullptr) {
            RUVIA_CHECK_EQ(closed->stream_id(), std::uint32_t{1});
            RUVIA_CHECK(closed->source() == ruvia::http2_stream_close_source::local);
            RUVIA_CHECK(closed->error() == ruvia::http2_error_code::protocol_error);
        }
        RUVIA_CHECK(!client.next_event());
        RUVIA_CHECK(!client.connection_error());
    }
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

// RFC 9110 section 6.5.1 binds only the sender: a received response trailer
// that is not permitted as a trailer is discarded, while a connection-specific
// field keeps the stream malformed (RFC 9113 section 8.2.2).
RUVIA_TEST(http2_public_client_discards_response_trailers_not_permitted_as_trailers) {
    for (const std::string_view name : {"content-type", "date", "connection"}) {
        const bool malformed = name == "connection";
        std::pmr::monotonic_buffer_resource resource;
        auto client = prepared_client(&resource);
        std::pmr::string wire(&resource);
        append_peer_settings(wire);
        append_response(wire, &resource, 1, "200", "abc", name, "text/plain");
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        unsigned ends = 0;
        unsigned resets = 0;
        std::size_t trailers = 1;
        while (auto event = client.next_event()) {
            if (auto* chunk = event->message_body_chunk()) {
                (void)client.acknowledge(chunk->take_credit());
            } else if (const auto* end = event->message_end()) {
                ++ends;
                trailers = end->trailers().size();
            } else if (const auto* closed = event->stream_closed()) {
                resets += closed->error() == ruvia::http2_error_code::protocol_error;
            }
        }
        RUVIA_CHECK_EQ(ends, malformed ? 0U : 1U);
        RUVIA_CHECK_EQ(resets, malformed ? 1U : 0U);
        if (!malformed) {
            RUVIA_CHECK_EQ(trailers, std::size_t{0});
        }
        RUVIA_CHECK(!client.connection_error());
    }
}

// RFC 9110 section 15: a received :status 000..099 or 600..999 is processed as
// 5xx.
RUVIA_TEST(http2_public_client_processes_out_of_range_status_as_server_error) {
    for (const auto& [status, value] : {std::pair{std::string_view("600"), 600},
             std::pair{std::string_view("099"), 99}, std::pair{std::string_view("000"), 0}}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = prepared_client(&resource);
        std::pmr::string wire(&resource);
        append_peer_settings(wire);
        append_response(wire, &resource, 1, status);
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head_event = client.next_event();
        RUVIA_CHECK(head_event && head_event->response_head() != nullptr);
        if (head_event && head_event->response_head() != nullptr) {
            const auto head = std::move(*head_event->response_head()).take_head();
            RUVIA_CHECK(head.status() == ruvia::http_status::internal_server_error);
            RUVIA_CHECK_EQ(head.received_status().value(), value);
        }
        RUVIA_CHECK(!client.connection_error());
    }
}

RUVIA_TEST(http2_public_single_and_fragmented_request_fields_are_owned_and_keep_hpack_indexes) {
    const std::string head_name = "x-request-" + std::string(48, 'h');
    const std::string trailer_name = "x-request-" + std::string(48, 't');
    const std::string head_value(256, 'h');
    const std::string trailer_value(256, 't');
    for (const bool fragmented : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        std::pmr::string head(&resource);
        ruvia::hpack_encoder::encode_header(head, ":method", "GET");
        ruvia::hpack_encoder::encode_header(head, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(head, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(head, ":path", "/owned");
        const auto field_offset = head.size();
        ruvia::hpack_encoder::encode_header(head, head_name, head_value);
        head[field_offset] = static_cast<char>(0x40);  // Literal with incremental indexing.
        std::pmr::string trailer(&resource);
        ruvia::hpack_encoder::encode_header(trailer, trailer_name, trailer_value);
        trailer[0] = static_cast<char>(0x40);
        std::pmr::string wire(ruvia::http2_client_preface, &resource);
        append_peer_settings(wire);
        append_header_section(wire, 1, head, false, fragmented);
        append_header_section(wire, 1, trailer, true, fragmented);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::fill(wire.begin(), wire.end(), '#');
        auto request = server.next_event();
        RUVIA_CHECK(request && request->request_head());
        if (!request || !request->request_head()) {
            continue;
        }
        const auto received = request->request_head()->build_request(&resource);
        RUVIA_CHECK(received.target() == "/owned");
        RUVIA_CHECK(received.header(head_name) == head_value);
        auto end = server.next_event();
        RUVIA_CHECK(end && end->message_end());
        if (!end || !end->message_end()) {
            continue;
        }
        RUVIA_CHECK_EQ(end->message_end()->trailers().size(), std::size_t{1});
        RUVIA_CHECK(end->message_end()->trailers().front().name() == trailer_name);
        RUVIA_CHECK(end->message_end()->trailers().front().value() == trailer_value);

        head.resize(field_offset);
        head.push_back(static_cast<char>(0xbf));     // Older head field: dynamic index 63.
        trailer.assign(1, static_cast<char>(0xbe));  // Newest trailer field: dynamic index 62.
        wire.clear();
        append_header_section(wire, 3, head, false, fragmented);
        append_header_section(wire, 3, trailer, true, fragmented);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::fill(wire.begin(), wire.end(), '#');
        auto indexed_request = server.next_event();
        RUVIA_CHECK(indexed_request && indexed_request->request_head());
        if (indexed_request && indexed_request->request_head()) {
            RUVIA_CHECK(indexed_request->request_head()->build_request(&resource).header(head_name) == head_value);
        }
        auto indexed_end = server.next_event();
        RUVIA_CHECK(indexed_end && indexed_end->message_end());
        if (indexed_end && indexed_end->message_end()) {
            RUVIA_CHECK_EQ(indexed_end->message_end()->trailers().size(), std::size_t{1});
            RUVIA_CHECK(indexed_end->message_end()->trailers().front().name() == trailer_name);
            RUVIA_CHECK(indexed_end->message_end()->trailers().front().value() == trailer_value);
        }
        RUVIA_CHECK(received.header(head_name) == head_value);
        RUVIA_CHECK(end->message_end()->trailers().front().value() == trailer_value);
    }
}

RUVIA_TEST(http2_public_single_and_fragmented_response_fields_are_owned_and_keep_hpack_indexes) {
    const std::string head_name = "x-response-" + std::string(48, 'h');
    const std::string trailer_name = "x-response-" + std::string(48, 't');
    const std::string head_value(256, 'h');
    const std::string trailer_value(256, 't');
    for (const bool fragmented : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = prepared_client(&resource);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/two"});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
        std::pmr::string head(&resource);
        ruvia::hpack_encoder::encode_status(head, ruvia::http_status::ok);
        const auto field_offset = head.size();
        ruvia::hpack_encoder::encode_header(head, head_name, head_value);
        head[field_offset] = static_cast<char>(0x40);
        std::pmr::string trailer(&resource);
        ruvia::hpack_encoder::encode_header(trailer, trailer_name, trailer_value);
        trailer[0] = static_cast<char>(0x40);
        std::pmr::string wire(&resource);
        append_peer_settings(wire);
        append_header_section(wire, 1, head, false, fragmented);
        append_header_section(wire, 1, trailer, true, fragmented);
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        std::fill(wire.begin(), wire.end(), '#');
        auto response = client.next_event();
        RUVIA_CHECK(response && response->response_head());
        if (!response || !response->response_head()) {
            continue;
        }
        auto owned_head = std::move(*response->response_head()).take_head();
        RUVIA_CHECK(owned_head.status() == ruvia::http_status::ok);
        RUVIA_CHECK_EQ(owned_head.headers().size(), std::size_t{1});
        RUVIA_CHECK(owned_head.headers().front().name() == head_name);
        RUVIA_CHECK(owned_head.headers().front().value() == head_value);
        auto end = client.next_event();
        RUVIA_CHECK(end && end->message_end());
        if (!end || !end->message_end()) {
            continue;
        }
        auto owned_trailers = std::move(*end->message_end()).take_trailers();
        RUVIA_CHECK_EQ(owned_trailers.size(), std::size_t{1});
        RUVIA_CHECK(owned_trailers.front().name() == trailer_name);
        RUVIA_CHECK(owned_trailers.front().value() == trailer_value);

        head.resize(field_offset);
        head.push_back(static_cast<char>(0xbf));
        trailer.assign(1, static_cast<char>(0xbe));
        wire.clear();
        append_header_section(wire, 3, head, false, fragmented);
        append_header_section(wire, 3, trailer, true, fragmented);
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        std::fill(wire.begin(), wire.end(), '#');
        auto indexed_response = client.next_event();
        RUVIA_CHECK(indexed_response && indexed_response->response_head());
        if (indexed_response && indexed_response->response_head()) {
            const auto& indexed_head = indexed_response->response_head()->head();
            RUVIA_CHECK_EQ(indexed_head.headers().size(), std::size_t{1});
            RUVIA_CHECK(indexed_head.headers().front().name() == head_name);
            RUVIA_CHECK(indexed_head.headers().front().value() == head_value);
        }
        auto indexed_end = client.next_event();
        RUVIA_CHECK(indexed_end && indexed_end->message_end());
        if (indexed_end && indexed_end->message_end()) {
            RUVIA_CHECK_EQ(indexed_end->message_end()->trailers().size(), std::size_t{1});
            RUVIA_CHECK(indexed_end->message_end()->trailers().front().name() == trailer_name);
            RUVIA_CHECK(indexed_end->message_end()->trailers().front().value() == trailer_value);
        }
        RUVIA_CHECK(owned_head.headers().front().value() == head_value);
        RUVIA_CHECK(owned_trailers.front().value() == trailer_value);
    }
}

RUVIA_TEST(http2_public_head_205_keeps_nonzero_content_length_as_metadata) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client_method(&resource, "HEAD");
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::reset_content);
    ruvia::hpack_encoder::encode_header(block, "content-length", "7");
    std::pmr::string wire(&resource);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    const auto head = client.next_event();
    RUVIA_CHECK(head && head->response_head());
    if (head && head->response_head()) {
        const auto& received = head->response_head()->head();
        RUVIA_CHECK(received.status() == ruvia::http_status::reset_content);
        RUVIA_CHECK(!received.headers().empty());
        if (!received.headers().empty()) {
            RUVIA_CHECK_EQ(received.headers().front().value(), "7");
        }
    }
    const auto end = client.next_event();
    RUVIA_CHECK(end && end->message_end());
    if (end && end->message_end()) {
        RUVIA_CHECK(end->message_end()->content_semantics() ==
                    ruvia::http2_message_content_semantics::metadata_only);
    }
    RUVIA_CHECK(!client.next_event());
    RUVIA_CHECK(!client.connection_error());
}

RUVIA_TEST(http2_public_205_content_is_discarded_with_flow_control_credit_returned) {
    // RFC 9110 section 15.3.6 binds only the 205 sender; DATA framing is
    // unambiguous, so the client consumes and discards content.
    for (const bool declared : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        auto client = prepared_client_method(&resource, "GET");
        (void)ruvia::testing::drain_output(client);
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::reset_content);
        // Enough discarded bytes to cross the half-window WINDOW_UPDATE threshold.
        const std::string bulk(16384, 'e');
        constexpr int bulk_frames = 33;
        if (declared) {
            ruvia::hpack_encoder::encode_header(
                block, "content-length", std::to_string(bulk.size() * bulk_frames));
        }
        std::pmr::string wire(&resource);
        append_peer_settings(wire);
        // Acknowledged SETTINGS put the advertised 1 MiB stream window in force.
        append_frame(wire, ruvia::http2_frame_type::settings, 0x1, 0, {});
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        for (int frame = 0; frame != bulk_frames; ++frame) {
            append_frame(wire, ruvia::http2_frame_type::data, frame + 1 == bulk_frames ? 0x1 : 0x0, 1, bulk);
        }
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        const auto head = client.next_event();
        RUVIA_CHECK(head && head->response_head() &&
                    head->response_head()->head().status() == ruvia::http_status::reset_content);
        const auto end = client.next_event();
        RUVIA_CHECK(end && end->message_end());
        RUVIA_CHECK(!client.next_event());
        RUVIA_CHECK(!client.connection_error());
        // The discarded bytes' connection credit is returned without an owner.
        constexpr std::string_view connection_window_update{"\0\0\x04\x08\0\0\0\0\0", 9};
        const auto output = ruvia::testing::drain_output(client);
        RUVIA_CHECK(std::string_view(output).find(connection_window_update) != std::string_view::npos);
    }

    // Content-Length still binds the DATA sum (RFC 9113 section 8.1.1).
    std::pmr::monotonic_buffer_resource resource;
    auto client = prepared_client_method(&resource, "GET");
    (void)ruvia::testing::drain_output(client);
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::reset_content);
    ruvia::hpack_encoder::encode_header(block, "content-length", "2");
    std::pmr::string wire(&resource);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "hello");
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    const auto head = client.next_event();
    RUVIA_CHECK(head && head->response_head());
    while (auto event = client.next_event()) {
        RUVIA_CHECK(!event->message_body_chunk());
        RUVIA_CHECK(!event->message_end());
    }
    RUVIA_CHECK(!client.connection_error());
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

// nghttp2 offers no rollback once input processing allocates, so an allocation
// failure while receiving is fatal: bad_alloc reaches the caller and the
// connection is failed with INTERNAL_ERROR instead of becoming retryable.
RUVIA_TEST(http2_public_receive_allocation_failure_fails_the_connection) {
    toggle_allocation_resource resource;
    auto client = prepared_client(&resource);
    std::pmr::string wire(&resource);
    append_peer_settings(wire);
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(client.next_event().has_value());
    (void)ruvia::testing::drain_output(client);
    block.clear();
    const std::string trailer_value(256, 't');
    ruvia::hpack_encoder::encode_header(block, "x-trace", trailer_value);
    std::pmr::string trailers(&resource);
    append_frame(trailers, ruvia::http2_frame_type::headers, 0x5, 1, block);
    if (!ruvia::test::stl_allocation_failure_injection_enabled(ruvia_ctx)) {
        RUVIA_CHECK(client.feed(trailers) == ruvia::http2_feed_result::accepted);
        const auto end = client.next_event();
        RUVIA_CHECK(end && end->message_end() != nullptr);
        if (end && end->message_end()) {
            const auto fields = end->message_end()->trailers();
            RUVIA_CHECK_EQ(fields.size(), std::size_t{1});
            if (fields.size() == 1) {
                RUVIA_CHECK_EQ(fields.front().name(), "x-trace");
                RUVIA_CHECK_EQ(fields.front().value(), trailer_value);
            }
        }
        RUVIA_CHECK(!client.connection_error().has_value());
        return;
    }
    resource.reject();
    bool threw = false;
    try {
        (void)client.feed(trailers);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    resource.reject(false);
    RUVIA_CHECK(client.connection_error() == ruvia::http2_error_code::internal_error);
    RUVIA_CHECK(!client.next_event().has_value());
    RUVIA_CHECK(client.feed(trailers) == ruvia::http2_feed_result::protocol_failure);
    RUVIA_CHECK(!client.next_event().has_value());
    RUVIA_CHECK(client.submit_ping() == ruvia::http2_submit_status::closed);
}

// Errors nghttp2 reports as fatal from its receive call still end the connection
// with a GOAWAY carrying the mapped error code.
RUVIA_TEST(http2_public_fatal_receive_errors_end_in_goaway) {
    const auto goaway_error = [](std::string_view output) -> std::optional<std::uint32_t> {
        while (output.size() >= ruvia::http2_frame_header_bytes) {
            const auto header = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
            if (!header || output.size() < ruvia::http2_frame_header_bytes + header->length_) {
                return std::nullopt;
            }
            const auto payload = output.substr(ruvia::http2_frame_header_bytes, header->length_);
            if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) && payload.size() >= 8) {
                std::uint32_t code = 0;
                for (const auto byte : payload.substr(4, 4)) {
                    code = (code << 8) | static_cast<unsigned char>(byte);
                }
                return code;
            }
            output.remove_prefix(ruvia::http2_frame_header_bytes + header->length_);
        }
        return std::nullopt;
    };
    {
        std::pmr::unsynchronized_pool_resource resource;
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        (void)ruvia::testing::drain_output(server);
        RUVIA_CHECK(server.feed("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n") == ruvia::http2_feed_result::protocol_failure);
        RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::protocol_error);
        RUVIA_CHECK(goaway_error(ruvia::testing::drain_output(server)) ==
                    static_cast<std::uint32_t>(ruvia::http2_error_code::protocol_error));
    }
    {
        // CVE-2024-27316: a header block may not continue indefinitely.
        std::pmr::unsynchronized_pool_resource resource;
        auto server = started_server(&resource);
        std::pmr::string wire(&resource);
        append_frame(wire, ruvia::http2_frame_type::headers, 0, 1, {});
        for (std::size_t index = 0; index != 1025; ++index) {
            append_frame(wire, ruvia::http2_frame_type::continuation, 0, 1, {});
        }
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        RUVIA_CHECK(server.connection_error() == ruvia::http2_error_code::enhance_your_calm);
        RUVIA_CHECK(goaway_error(ruvia::testing::drain_output(server)) ==
                    static_cast<std::uint32_t>(ruvia::http2_error_code::enhance_your_calm));
    }
}

RUVIA_TEST(http2_public_data_credit_merge_is_linear) {
    toggle_allocation_resource resource;
    auto client = prepared_client(&resource);
    auto wire = client_response_wire(&resource, "x", false, false);
    append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "x");
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    std::pmr::string drained(&resource);
    ruvia::testing::drain_output(client, drained);
    RUVIA_CHECK(client.next_event().has_value());
    auto first = client.next_event();
    RUVIA_CHECK(first && first->message_body_chunk() != nullptr);
    auto first_credit = first->message_body_chunk()->take_credit();
    constexpr std::size_t frame_count = 2;
    for (std::size_t index = 1; index < frame_count; ++index) {
        auto chunk = client.next_event();
        RUVIA_CHECK(chunk && chunk->message_body_chunk() != nullptr);
        auto next_credit = chunk->message_body_chunk()->take_credit();
        first_credit.merge(std::move(next_credit));
        RUVIA_CHECK(first_credit.valid());
        RUVIA_CHECK(!next_credit.valid());
        RUVIA_CHECK(!client.wants_write());
    }
    auto end = client.next_event();
    RUVIA_CHECK(end && end->message_end() != nullptr);
    RUVIA_CHECK(!client.wants_write());
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

    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)first_credit.merge(std::move(second_credit)); }));
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
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));

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

    RUVIA_CHECK(ruvia::testing::throws_on([&] { first_credit.merge(std::move(second_credit)); }));
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
    (void)ruvia::testing::drain_output(server);
    auto wire = server_request_with_trailers_wire(&resource);
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head() != nullptr);
    RUVIA_CHECK(!request->request_head()->build_request(&resource).header("x-request-trace"));
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
    const auto built_initial = request->request_head()->build_request(&resource);
    const auto borrowed = built_initial.header("x-initial");
    RUVIA_CHECK(borrowed && *borrowed == initial_value);

    block.clear();
    ruvia::hpack_encoder::encode_header(block, "x-trailer", trailer_value);
    std::pmr::string trailers(&resource);
    append_frame(trailers, ruvia::http2_frame_type::headers, 0x5, 1, block);
    RUVIA_CHECK(server.feed(trailers) == ruvia::http2_feed_result::accepted);
    auto end = server.next_event();
    RUVIA_CHECK(end && end->message_end() != nullptr);
    RUVIA_CHECK(borrowed && *borrowed == initial_value);
    const auto built_after = request->request_head()->build_request(&resource);
    RUVIA_CHECK(!built_after.header("x-trailer"));
    const auto fields_value = end->message_end()->trailers();
    RUVIA_CHECK(fields_value.size() == 1);
    if (!fields_value.empty()) {
        RUVIA_CHECK(fields_value.front().value() == trailer_value);
    }
    RUVIA_CHECK(server.release(std::move(*request->request_head())) ==
                ruvia::http2_server_request_release_status::released);
}

// A trailer declaration never authorizes forbidden actual trailers. Fields the
// sender was not permitted to generate (RFC 9110 section 6.5.1) are discarded
// and never merged into the head; connection-specific fields stay malformed
// (RFC 9113 section 8.2.2).
RUVIA_TEST(http2_public_trailer_declaration_does_not_authorize_forbidden_actual_fields) {
    for (const std::string_view forbidden : {"content-length", "host", "cache-control", "connection"}) {
        const bool malformed = forbidden == "connection";
        std::pmr::unsynchronized_pool_resource resource;
        std::pmr::string wire(ruvia::http2_client_preface, &resource);
        append_peer_settings(wire);
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/");
        ruvia::hpack_encoder::encode_header(block, "content-length", "3");
        ruvia::hpack_encoder::encode_header(block, "trailer", "Content-Length, Host, bad field");
        append_header_section(wire, 1, block, false, true);
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1, "abc");
        block.clear();
        ruvia::hpack_encoder::encode_header(block, "x-checksum", "kept");
        ruvia::hpack_encoder::encode_header(block, forbidden, forbidden == "host" ? "other.test" : "99");
        append_header_section(wire, 1, block, true, true);
        block.clear();
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/next");
        append_header_section(wire, 3, block, true, false);
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::optional<ruvia::http2_request_head_event> first;
        std::optional<ruvia::http2_request_head_event> next;
        std::string body;
        unsigned resets = 0;
        unsigned first_ends = 0;
        unsigned next_ends = 0;
        std::size_t first_trailers = 0;
        bool kept_checksum = false;
        while (auto event = server.next_event()) {
            if (auto* head = event->request_head()) {
                if (head->stream_id() == 1) {
                    first.emplace(std::move(*head));
                } else if (head->stream_id() == 3) {
                    next.emplace(std::move(*head));
                }
            } else if (auto* chunk = event->message_body_chunk()) {
                RUVIA_CHECK(chunk->stream_id() == 1);
                body.append(chunk->bytes());
                auto credit = chunk->take_credit();
                (void)server.acknowledge(std::move(credit));
            } else if (const auto* end = event->message_end()) {
                if (end->stream_id() == 1) {
                    ++first_ends;
                    first_trailers = end->trailers().size();
                    kept_checksum = first_trailers != 0 && end->trailers().front().name() == "x-checksum" &&
                                    end->trailers().front().value() == "kept";
                }
                next_ends += end->stream_id() == 3;
            } else if (const auto* closed = event->stream_closed()) {
                resets += closed->stream_id() == 1 && closed->error() == ruvia::http2_error_code::protocol_error;
            }
        }
        RUVIA_CHECK(first && next && next_ends == 1);
        RUVIA_CHECK_EQ(resets, malformed ? 1U : 0U);
        RUVIA_CHECK_EQ(first_ends, malformed ? 0U : 1U);
        if (!malformed) {
            RUVIA_CHECK_EQ(first_trailers, std::size_t{1});
            RUVIA_CHECK(kept_checksum);
        }
        RUVIA_CHECK_EQ(body, "abc");
        RUVIA_CHECK(!server.connection_error());
        if (first) {
            const auto built_first = first->build_request(&resource);
            RUVIA_CHECK_EQ(built_first.header("Trailer").value_or(""), "Content-Length, Host, bad field");
            RUVIA_CHECK_EQ(built_first.header("Content-Length").value_or(""), "3");
            RUVIA_CHECK_EQ(built_first.header("Host").value_or(""), "example.test");
            (void)server.release(std::move(*first));
        }
        if (next) {
            RUVIA_CHECK_EQ(next->request_view().path_, "/next");
            ruvia::http_response reply(&resource);
            RUVIA_CHECK(submit_buffered_response(server, 3, reply) == ruvia::http2_submit_status::accepted);
            (void)server.release(std::move(*next));
        }
    }
}

RUVIA_TEST(http2_public_server_release_before_message_end_keeps_terminal_event) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
    auto wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    RUVIA_CHECK(request && request->request_head() != nullptr);
    ruvia::http_response response(&resource);
    RUVIA_CHECK(submit_buffered_response(server, 1, response) ==
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
    (void)ruvia::testing::drain_output(server);
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
    RUVIA_CHECK(plan == ruvia::http_server_expectation_plan::send_continue);
}

RUVIA_TEST(http2_public_semantically_invalid_expectations_preserve_upload_boundaries) {
    for (const std::string_view expectation :
        {"bad field", "100-continue;foo=bar", "custom=\"unterminated", "100-continue, bad field"}) {
        std::pmr::monotonic_buffer_resource resource;
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
        ruvia::hpack_encoder::encode_header(block, "content-length", "3");
        ruvia::hpack_encoder::encode_header(block, "expect", expectation);
        std::pmr::string wire(ruvia::http2_client_preface, &resource);
        append_peer_settings(wire);
        append_header_section(wire, 1, block, false, true);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        RUVIA_CHECK(request != nullptr);
        if (!request) {
            continue;
        }
        RUVIA_CHECK_EQ(request->stream_id(), std::uint32_t{1});
        RUVIA_CHECK(request->build_request(&resource).header("Expect") == expectation);
        RUVIA_CHECK(request->snapshot().body_open_);
        RUVIA_CHECK(request->snapshot().content_ == ruvia::http_request_content_indication::will_follow);
        const auto rejected = request->expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
        RUVIA_CHECK(rejected == ruvia::http_server_expectation_plan::reject_unsupported);
        RUVIA_CHECK_EQ(
            ruvia::http_unsupported_expectation_error().status(), ruvia::http_status::expectation_failed);
        const auto ignored = request->expectation_plan(ruvia::http_unsupported_expectation_policy::ignore);
        const bool known_continue = expectation == "100-continue, bad field";
        RUVIA_CHECK(ignored == (known_continue ? ruvia::http_server_expectation_plan::send_continue
                                               : ruvia::http_server_expectation_plan::no_action));
        RUVIA_CHECK(!server.next_event());

        wire.clear();
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1, "a");
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "bc");
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::string body;
        unsigned ends = 0;
        while (auto content = server.next_event()) {
            if (auto* chunk = content->message_body_chunk()) {
                RUVIA_CHECK_EQ(chunk->stream_id(), std::uint32_t{1});
                body.append(chunk->bytes());
                RUVIA_CHECK(server.acknowledge(chunk->take_credit()) ==
                            ruvia::http2_received_data_acknowledge_status::acknowledged);
            } else {
                const auto* end = content->message_end();
                RUVIA_CHECK(end != nullptr);
                if (end) {
                    RUVIA_CHECK_EQ(end->stream_id(), std::uint32_t{1});
                    RUVIA_CHECK(end->trailers().empty());
                    ++ends;
                }
            }
        }
        RUVIA_CHECK_EQ(body, "abc");
        RUVIA_CHECK_EQ(ends, 1u);
        ruvia::http_response reply(&resource);
        RUVIA_CHECK(submit_buffered_response(server, 1, reply) == ruvia::http2_submit_status::accepted);
        RUVIA_CHECK(server.release(std::move(*request)) ==
                    ruvia::http2_server_request_release_status::released);

        block.clear();
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/next");
        wire.clear();
        append_header_section(wire, 3, block, true, true);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto next = server.next_event();
        RUVIA_CHECK(next && next->request_head());
        if (next && next->request_head()) {
            RUVIA_CHECK_EQ(next->request_head()->stream_id(), std::uint32_t{3});
            RUVIA_CHECK_EQ(next->request_head()->request_view().path_, "/next");
            RUVIA_CHECK(submit_buffered_response(server, 3, reply) == ruvia::http2_submit_status::accepted);
            RUVIA_CHECK(server.release(std::move(*next->request_head())) ==
                        ruvia::http2_server_request_release_status::released);
        }
        auto end = server.next_event();
        RUVIA_CHECK(end && end->message_end());
        if (end && end->message_end()) {
            RUVIA_CHECK_EQ(end->message_end()->stream_id(), std::uint32_t{3});
        }
        RUVIA_CHECK(!server.next_event());
        RUVIA_CHECK(!server.connection_error());
    }
}

// body_open_ describes the head as published: a HEADERS frame without
// END_STREAM promises content events even when the DATA that completes the
// request arrived in the same feed() and the core already consumed it.
RUVIA_TEST(http2_public_server_request_snapshot_body_open_survives_batched_content) {
    for (const bool end_stream_on_headers : {false, true}) {
        std::pmr::monotonic_buffer_resource resource;
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
        std::pmr::string wire(ruvia::http2_client_preface, &resource);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers,
            static_cast<std::uint8_t>(end_stream_on_headers ? 0x5 : 0x4), 1, block);
        if (!end_stream_on_headers) {
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "x");
        }

        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        (void)ruvia::testing::drain_output(server);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        RUVIA_CHECK(request != nullptr);
        if (request == nullptr) {
            return;
        }
        RUVIA_CHECK(request->snapshot().body_open_ == !end_stream_on_headers);
        RUVIA_CHECK(!request->snapshot().connect_pending_);
        if (!end_stream_on_headers) {
            const auto chunk = server.next_event();
            RUVIA_CHECK(chunk && chunk->message_body_chunk() != nullptr &&
                        chunk->message_body_chunk()->bytes() == "x");
        }
        const auto end = server.next_event();
        RUVIA_CHECK(end && end->message_end() != nullptr);
        RUVIA_CHECK(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
    }
}

RUVIA_TEST(http2_public_dropped_request_before_message_end_keeps_terminal_event) {
    std::pmr::monotonic_buffer_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
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
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
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
        ruvia::testing::output_relay relay;
        auto exchange_value = [&](auto& from, auto& to) {
            RUVIA_CHECK(relay.feed(from, to) == ruvia::http2_feed_result::accepted);
        };
        exchange_value(client, server);
        exchange_value(server, client);
        const auto head = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http2_request_content::streaming(length)});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(head));
        exchange_value(client, server);
        bool saw_end = false;
        std::optional<ruvia::http2_request_head_event> lease;
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                RUVIA_CHECK_EQ(request->build_request(&resource).header("content-length").value_or(""), length == 0 ? "0" : "3");
                lease.emplace(std::move(*request));
            }
            saw_end |= event->message_end() != nullptr;
        }
        RUVIA_CHECK(!saw_end);
        if (length != 0) {
            RUVIA_CHECK(client.submit_data(1, "abc", ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
        }
        const std::array<ruvia::http_header_view, 1> trailers{{{"x-end", "retained"}}};
        RUVIA_CHECK(client.finish_request(1, trailers) == ruvia::http2_finish_status::accepted);
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
        ruvia::testing::output_relay relay;
        auto exchange_value = [&](auto& from, auto& to) {
            RUVIA_CHECK(relay.feed(from, to) == ruvia::http2_feed_result::accepted);
        };
        exchange_value(client, server);
        exchange_value(server, client);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
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
            auto pushed = server.submit_push_request(std::get<std::uint32_t>(submitted),
                {.authority_ = "example.test", .path_ = "/asset?version=1", .headers_ = headers});
            RUVIA_CHECK((pushed.index() == 0));
            if ((pushed.index() != 0)) {
                break;
            }
            const auto request = std::get<0>(pushed).build_request(&resource);
            RUVIA_CHECK(request.scheme() == "https" && request.authority() == "example.test");
            RUVIA_CHECK(request.path() == "/asset" && request.query_string() == "version=1");
            RUVIA_CHECK(request.header("cookie") == "a=1");
            RUVIA_CHECK(request.header("host") == "EXAMPLE.test:443");
            RUVIA_CHECK(request.header("x-push") == "request");
            ruvia::http_response response;
            RUVIA_CHECK(submit_buffered_response(server, std::get<0>(pushed).stream_id(), response) == ruvia::http2_submit_status::accepted);
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

RUVIA_TEST(http2_public_request_reset_keeps_events_consumable) {
    for (const bool split_input : {false, true}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        (void)ruvia::testing::drain_output(server);
        auto wire = server_request_wire(&resource, {});
        if (split_input) {
            const auto prefix_size = wire.size() - 1;
            RUVIA_CHECK(server.feed(std::string_view(wire).substr(0, prefix_size)) ==
                        ruvia::http2_feed_result::accepted);
            wire.erase(0, prefix_size);
        }
        const char cancel[] = {0, 0, 0, 8};
        append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1,
            std::string_view(cancel, sizeof(cancel)));
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        RUVIA_CHECK(!server.connection_error().has_value());
        bool saw_close = false;
        std::size_t request_heads = 0;
        while (auto event = server.next_event()) {
            saw_close = saw_close || event->stream_closed() != nullptr;
            if (auto* request = event->request_head()) {
                ++request_heads;
                RUVIA_CHECK_EQ(request->request_view().path_, std::string_view("/upload"));
                RUVIA_CHECK(server.release(std::move(*request)) ==
                            ruvia::http2_server_request_release_status::released);
            }
        }
        RUVIA_CHECK(saw_close);
        RUVIA_CHECK_EQ(request_heads, std::size_t{1});
    }
}

RUVIA_TEST(http2_public_closed_request_lease_does_not_overflow_settings) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
    const auto request_wire = server_request_wire(&resource, {});
    RUVIA_CHECK(server.feed(request_wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    RUVIA_CHECK(request_head != nullptr);
    while (server.next_event()) {
    }
    std::pmr::string wire(&resource);
    const char increment[] = {0x7f, static_cast<char>(0xff), 0, 0};
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1,
        std::string_view(increment, sizeof(increment)));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    ruvia::http_response response(&resource);
    RUVIA_CHECK(submit_buffered_response(server, 1, response) ==
                ruvia::http2_submit_status::accepted);
    while (server.next_event()) {
    }
    (void)ruvia::testing::drain_output(server);
    const char settings[] = {0, 4, 0, 1, 0, 0};
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
        std::string_view(settings, sizeof(settings)));
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());
    if (request_head != nullptr) {
        RUVIA_CHECK(server.release(std::move(*request_head)) ==
                    ruvia::http2_server_request_release_status::released);
    }
}

RUVIA_TEST(http2_public_server_refuses_stream_beyond_advertised_concurrency) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");

    // The server advertises SETTINGS_MAX_CONCURRENT_STREAMS = 128. One HEADERS
    // beyond that limit is a stream error (RFC 9113 5.1.2), not a connection error.
    constexpr std::uint32_t advertised_limit = 128;
    std::pmr::string wire(ruvia::http2_client_preface, &resource);
    append_peer_settings(wire);
    for (std::uint32_t index = 0; index <= advertised_limit; ++index) {
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, index * 2 + 1, block);
    }
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!server.connection_error().has_value());

    std::pmr::string refused(&resource);
    constexpr std::array<char, 4> refused_stream_payload{0, 0, 0, 7};
    append_frame(refused, ruvia::http2_frame_type::rst_stream, 0, advertised_limit * 2 + 1,
        std::string_view(refused_stream_payload.data(), refused_stream_payload.size()));
    RUVIA_CHECK(ruvia::testing::drain_output(server).ends_with(std::string_view(refused)));

    std::size_t request_heads = 0;
    while (auto event = server.next_event()) {
        if (event->request_head() != nullptr) {
            ++request_heads;
        }
    }
    RUVIA_CHECK_EQ(request_heads, std::size_t{advertised_limit});
}

RUVIA_TEST(http2_public_reports_available_client_request_streams_without_reserving) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    RUVIA_CHECK_EQ(server.available_request_streams(4), std::size_t{0});

    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    RUVIA_CHECK_EQ(client.available_request_streams(1000), std::size_t{128});
    RUVIA_CHECK_EQ(client.available_request_streams(0), std::size_t{0});
    std::pmr::string settings(&resource);
    constexpr std::array<char, 6> peer_limit{0, 3, 0, 0, 0, 2};
    append_frame(settings, ruvia::http2_frame_type::settings, 0, 0,
        std::string_view(peer_limit.data(), peer_limit.size()));
    RUVIA_CHECK(client.feed(settings) == ruvia::http2_feed_result::accepted);
    while (client.next_event()) {
    }
    RUVIA_CHECK_EQ(client.available_request_streams(4), std::size_t{2});
    RUVIA_CHECK_EQ(client.available_request_streams(1), std::size_t{1});

    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    const auto* stream_id = std::get_if<std::uint32_t>(&submitted);
    RUVIA_CHECK(stream_id != nullptr);
    RUVIA_CHECK_EQ(client.available_request_streams(4), std::size_t{1});
    if (stream_id != nullptr) {
        RUVIA_CHECK(client.submit_reset(*stream_id, ruvia::http2_error_code::cancel) ==
                    ruvia::http2_submit_status::accepted);
    }
    RUVIA_CHECK_EQ(client.available_request_streams(4), std::size_t{2});

    client.begin_drain();
    RUVIA_CHECK_EQ(client.available_request_streams(4), std::size_t{0});
}

namespace {

bool http2_client_submits_field(std::string_view name, std::string_view value) {
    std::pmr::monotonic_buffer_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    const std::array headers{ruvia::http_header_view{name, value}};
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST",
        .authority_ = "example.test",
        .target_ = "/",
        .headers_ = headers,
        .content_ = ruvia::http2_request_content::streaming()});
    return std::holds_alternative<std::uint32_t>(submitted);
}

bool http2_server_accepts_field(std::string_view name, std::string_view value) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");
    ruvia::hpack_encoder::encode_header(block, name, value);
    std::pmr::string wire(ruvia::http2_client_preface, &resource);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);

    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);
    if (server.feed(wire) != ruvia::http2_feed_result::accepted) {
        return false;
    }
    while (auto event = server.next_event()) {
        if (event->request_head() != nullptr) {
            return true;
        }
    }
    return false;
}

auto submit_field_request(ruvia::http2_connection& client, unsigned entry,
    std::span<const ruvia::http_header_view> fields, ruvia::http_request_field_source source) {
    switch (entry) {
        case 0:
            return client.submit_request_head(ruvia::http2_regular_request_head_view{
                .authority_ = "example.test", .headers_ = fields, .field_source_ = source});
        case 1:
            return client.submit_request_head(ruvia::http2_regular_request_head_view{
                .method_ = "POST", .authority_ = "example.test", .headers_ = fields, .content_ = ruvia::http2_request_content::streaming(), .field_source_ = source});
        case 2:
            return client.submit_request_head(ruvia::http2_connect_request_head_view{
                .authority_ = "example.test:443", .headers_ = fields, .field_source_ = source});
        default:
            return client.submit_request_head(ruvia::http2_extended_connect_request_head_view{
                .protocol_ = "example", .authority_ = "example.test:443", .target_ = "/tunnel", .headers_ = fields, .field_source_ = source});
    }
}

}  // namespace

RUVIA_TEST(http2_public_client_generates_framing_and_expectation_fields_itself) {
    for (const auto& [name, value] : {std::pair<std::string_view, std::string_view>{"expect", "100-continue"},
             {"Expect", "foo=bar"}, {"content-length", "0"}, {"Content-Length", "3"}}) {
        RUVIA_CHECK(!http2_client_submits_field(name, value));
    }
}

RUVIA_TEST(http2_public_te_allows_only_trailers_with_list_ows) {
    for (const std::string_view value : {"trailers", "TRAILERS", "trailers , trailers"}) {
        RUVIA_CHECK(http2_client_submits_field("te", value));
        RUVIA_CHECK(http2_server_accepts_field("te", value));
    }
    for (const std::string_view value : {"gzip", "trailers, gzip", "trailers;q=1"}) {
        RUVIA_CHECK(!http2_client_submits_field("te", value));
        RUVIA_CHECK(!http2_server_accepts_field("te", value));
    }
}

RUVIA_TEST(http2_public_received_te_accepts_empty_lists_without_dropping_fields) {
    for (const std::string_view value : {"", ",,", ", trailers,,"}) {
        std::pmr::unsynchronized_pool_resource resource;
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/");
        ruvia::hpack_encoder::encode_header(block, "te", value);
        ruvia::hpack_encoder::encode_header(block, "x-after", "kept");
        std::pmr::string wire(ruvia::http2_client_preface, &resource);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);

        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        (void)ruvia::testing::drain_output(server);
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        RUVIA_CHECK(request != nullptr);
        if (request == nullptr) {
            continue;
        }
        const auto built_req = request->build_request(&resource);
        RUVIA_CHECK(built_req.header("te") == std::optional<std::string_view>{value});
        RUVIA_CHECK(built_req.header("x-after") == std::string_view("kept"));
        const auto end = server.next_event();
        RUVIA_CHECK(end && end->message_end() && end->message_end()->stream_id() == 1);
        RUVIA_CHECK(!server.connection_error());
        ruvia::http_response response(&resource);
        RUVIA_CHECK(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
        RUVIA_CHECK(server.release(std::move(*request)) ==
                    ruvia::http2_server_request_release_status::released);
    }
}

RUVIA_TEST(http2_public_normalized_request_capacity_accepts_many_fields_within_byte_budget) {
    for (const std::size_t fields : {std::size_t{63}, std::size_t{80}}) {
        for (const bool explicit_host : {false, true}) {
            for (const unsigned cookies : {0U, 1U, 2U}) {
                std::pmr::unsynchronized_pool_resource resource;
                std::pmr::string wire(ruvia::http2_client_preface, &resource);
                append_peer_settings(wire);
                std::pmr::string block(&resource);
                const auto encode_target = [&](std::string_view path) {
                    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
                    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
                    ruvia::hpack_encoder::encode_header(block, ":path", path);
                };
                encode_target("/capacity?x=kept");
                if (explicit_host) {
                    ruvia::hpack_encoder::encode_header(block, "host", "EXAMPLE.test:443");
                }
                for (unsigned i = 0; i < cookies; ++i) {
                    ruvia::hpack_encoder::encode_header(block, "cookie", i == 0 ? "a=1" : "b=2");
                }
                for (std::size_t i = static_cast<std::size_t>(explicit_host) + cookies; i < fields; ++i) {
                    ruvia::hpack_encoder::encode_header(block, "x-pad", "value");
                }
                append_header_section(wire, 1, block, true, true);
                block.clear();
                encode_target("/next");
                append_header_section(wire, 3, block, true, false);
                auto server = ruvia::http2_connection::server({.resource_ = &resource});
                RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
                const auto normalized = fields - cookies + static_cast<std::size_t>(cookies != 0) +
                                        static_cast<std::size_t>(!explicit_host);
                unsigned first_heads = 0;
                unsigned first_ends = 0;
                unsigned resets = 0;
                unsigned following_heads = 0;
                unsigned following_ends = 0;
                while (auto event = server.next_event()) {
                    if (auto* head = event->request_head()) {
                        const auto id = head->stream_id();
                        if (id == 1) {
                            ++first_heads;
                            const auto built_req = head->build_request(&resource);
                            RUVIA_CHECK_EQ(built_req.headers().size(), normalized);
                            RUVIA_CHECK_EQ(built_req.header("Host").value_or(""),
                                explicit_host ? "EXAMPLE.test:443" : "example.test");
                            RUVIA_CHECK_EQ(built_req.query_string(), "x=kept");
                            if (cookies != 0) {
                                RUVIA_CHECK_EQ(built_req.header("Cookie").value_or(""),
                                    cookies == 1 ? "a=1" : "a=1; b=2");
                            }
                        } else {
                            ++following_heads;
                            RUVIA_CHECK(id == 3 && head->request_view().path_ == "/next");
                        }
                        ruvia::http_response reply(&resource);
                        RUVIA_CHECK(submit_buffered_response(server, id, reply) == ruvia::http2_submit_status::accepted);
                        RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
                    } else if (const auto* end = event->message_end()) {
                        if (end->stream_id() == 1) {
                            ++first_ends;
                        } else if (end->stream_id() == 3) {
                            ++following_ends;
                        }
                    } else if (const auto* closed = event->stream_closed()) {
                        if (closed->stream_id() == 1 && closed->source() == ruvia::http2_stream_close_source::local &&
                            closed->error() == ruvia::http2_error_code::protocol_error) {
                            ++resets;
                        }
                    }
                }
                RUVIA_CHECK(first_heads == 1U);
                RUVIA_CHECK(first_ends == 1U);
                RUVIA_CHECK(resets == 0U);
                RUVIA_CHECK(following_heads == 1 && following_ends == 1 && !server.connection_error());
            }
        }
    }
}

RUVIA_TEST(http2_public_forwards_received_application_values_through_hpack) {
    const std::array cases{
        ruvia::http_header_view{"content-type", "not-a-media-type"},
        ruvia::http_header_view{"origin", "https://example.test/path"},
        ruvia::http_header_view{"access-control-request-method", "GET, POST"},
        ruvia::http_header_view{"access-control-request-headers", "X-Good, Bad Header"},
        ruvia::http_header_view{"content-encoding", ""},
        ruvia::http_header_view{"content-encoding", ",,"},
        ruvia::http_header_view{"content-encoding", ", gzip,, br,"},
        ruvia::http_header_view{"content-encoding", ", unknown-coding,,"}};
    for (const auto& field : cases) {
        std::pmr::unsynchronized_pool_resource resource;
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/");
        ruvia::hpack_encoder::encode_header(block, field.name(), field.value());
        std::pmr::string wire(ruvia::http2_client_preface, &resource);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        auto incoming = ruvia::http2_connection::server({.resource_ = &resource});
        RUVIA_CHECK(incoming.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = incoming.next_event();
        auto* received_head = event ? event->request_head() : nullptr;
        RUVIA_CHECK(received_head != nullptr);
        if (!received_head) {
            continue;
        }
        const auto received_request = received_head->build_request(&resource);
        const auto received = received_request.header(field.name());
        RUVIA_CHECK(received == std::optional<std::string_view>{field.value()});
        if (!received) {
            continue;
        }
        const std::array fields{ruvia::http_header_view{field.name(), *received}};
        for (unsigned entry = 0; entry != 4; ++entry) {
            ruvia::testing::output_relay relay;
            auto client = ruvia::http2_connection::client({.resource_ = &resource});
            auto upstream = ruvia::http2_connection::server({.resource_ = &resource});
            relay.transfer(client, upstream);
            relay.transfer(upstream, client);
            relay.transfer(client, upstream);
            const auto generated = submit_field_request(client, entry, fields, ruvia::http_request_field_source::generated);
            RUVIA_CHECK(std::get_if<ruvia::http2_request_head_submit_error>(&generated) != nullptr);
            RUVIA_CHECK(!client.wants_write());
            const auto forwarded = submit_field_request(client, entry, fields, ruvia::http_request_field_source::forwarded);
            const auto* id = std::get_if<std::uint32_t>(&forwarded);
            RUVIA_CHECK(id != nullptr);
            if (!id) {
                continue;
            }
            RUVIA_CHECK_EQ(*id, std::uint32_t{1});
            relay.transfer(client, upstream);
            auto forwarded_event = upstream.next_event();
            auto* forwarded_head = forwarded_event ? forwarded_event->request_head() : nullptr;
            RUVIA_CHECK(forwarded_head != nullptr);
            if (forwarded_head) {
                RUVIA_CHECK_EQ(forwarded_head->stream_id(), *id);
                const auto request = forwarded_head->build_request(&resource);
                RUVIA_CHECK(request.header(field.name()) == received);
            }
            RUVIA_CHECK(!upstream.connection_error());
        }
    }
}

RUVIA_TEST(http2_public_forwarding_never_relaxes_owned_hop_by_hop_or_singleton_fields) {
    const std::array invalid{
        ruvia::http_header_view{"x-test", "ok\r\ninjected: yes"},
        ruvia::http_header_view{"bad name", "value"},
        ruvia::http_header_view{"host", "other.test"},
        ruvia::http_header_view{"content-length", "0"},
        ruvia::http_header_view{"expect", "100-continue"},
        ruvia::http_header_view{"transfer-encoding", "chunked"},
        ruvia::http_header_view{"connection", "upgrade"},
        ruvia::http_header_view{"upgrade", ",websocket,"},
        ruvia::http_header_view{"keep-alive", "timeout=5"},
        ruvia::http_header_view{"proxy-connection", "keep-alive"},
        ruvia::http_header_view{"te", ",trailers,"},
        ruvia::http_header_view{"content-encoding", ",gzip;level=9,"},
        ruvia::http_header_view{"content-encoding", ",bad coding,"}};
    for (unsigned entry = 0; entry != 4; ++entry) {
        ruvia::testing::output_relay relay;
        auto client = ruvia::http2_connection::client();
        auto server = ruvia::http2_connection::server();
        relay.transfer(client, server);
        relay.transfer(server, client);
        relay.transfer(client, server);
        const auto reject = [&](std::span<const ruvia::http_header_view> fields,
                                ruvia::http_request_field_source source) {
            const auto result = submit_field_request(client, entry, fields, source);
            const auto* error = std::get_if<ruvia::http2_request_head_submit_error>(&result);
            RUVIA_CHECK(error != nullptr);
            if (error) {
                RUVIA_CHECK(*error == ruvia::http2_request_head_submit_error::invalid_message);
            }
            RUVIA_CHECK(!client.wants_write());
        };
        for (const auto& field : invalid) {
            const std::array fields{field};
            reject(fields, ruvia::http_request_field_source::forwarded);
        }
        for (const auto& field : std::array{
                 ruvia::http_header_view{"content-type", "invalid"},
                 ruvia::http_header_view{"origin", "invalid"},
                 ruvia::http_header_view{"access-control-request-method", "GET, POST"},
                 ruvia::http_header_view{"authorization", "Bearer token"},
                 ruvia::http_header_view{"user-agent", "agent"},
                 ruvia::http_header_view{"if-modified-since", "invalid"},
                 ruvia::http_header_view{"if-unmodified-since", "invalid"}}) {
            const std::array fields{field, field};
            reject(fields, ruvia::http_request_field_source::forwarded);
        }
        reject({}, static_cast<ruvia::http_request_field_source>(0xff));
        const std::array valid{ruvia::http_header_view{"content-encoding", ",gzip,,"}};
        const auto accepted = submit_field_request(client, entry, valid, ruvia::http_request_field_source::forwarded);
        const auto* id = std::get_if<std::uint32_t>(&accepted);
        RUVIA_CHECK(id && *id == 1);
        relay.transfer(client, server);
        auto event = server.next_event();
        RUVIA_CHECK(event && event->request_head());
    }
}

RUVIA_TEST(http2_public_forwarded_options_content_keeps_missing_media_type) {
    for (const auto content : {ruvia::http2_request_content::known_length(1),
             ruvia::http2_request_content::streaming(1), ruvia::http2_request_content::streaming()}) {
        std::pmr::unsynchronized_pool_resource resource;
        const auto incoming_wire = server_request_wire(&resource, "x", "OPTIONS");
        auto incoming = ruvia::http2_connection::server({.resource_ = &resource});
        RUVIA_CHECK(incoming.feed(incoming_wire) == ruvia::http2_feed_result::accepted);
        auto received = incoming.next_event();
        RUVIA_CHECK(received && received->request_head());
        std::string body;
        while (auto next = incoming.next_event()) {
            if (const auto* chunk = next->message_body_chunk()) {
                body.append(chunk->bytes());
            }
        }
        RUVIA_CHECK_EQ(body, "x");
        ruvia::testing::output_relay relay;
        auto client = ruvia::http2_connection::client({.resource_ = &resource});
        auto upstream = ruvia::http2_connection::server({.resource_ = &resource});
        relay.transfer(client, upstream);
        relay.transfer(upstream, client);
        relay.transfer(client, upstream);
        ruvia::http2_regular_request_head_view request{
            .method_ = "OPTIONS", .authority_ = "example.test", .content_ = content};
        const auto generated = client.submit_request_head(request);
        RUVIA_CHECK(std::get_if<ruvia::http2_request_head_submit_error>(&generated) != nullptr);
        request.method_ = "TRACE";
        request.field_source_ = ruvia::http_request_field_source::forwarded;
        const auto trace = client.submit_request_head(request);
        RUVIA_CHECK(std::get_if<ruvia::http2_request_head_submit_error>(&trace) != nullptr);
        RUVIA_CHECK(!client.wants_write());
        request.method_ = "OPTIONS";
        const auto forwarded = client.submit_request_head(request);
        const auto* id = std::get_if<std::uint32_t>(&forwarded);
        RUVIA_CHECK(id && *id == 1);
        if (!id) {
            continue;
        }
        RUVIA_CHECK(client.submit_data(*id, body, ruvia::http2_end_stream::end_stream, 65535).status_ ==
                    ruvia::http2_data_submit_status::complete);
        relay.transfer(client, upstream);
        auto event = upstream.next_event();
        auto* head = event ? event->request_head() : nullptr;
        RUVIA_CHECK(head != nullptr);
        if (head) {
            const auto forwarded_request = head->build_request(&resource);
            RUVIA_CHECK(!forwarded_request.header("content-type"));
        }
        bool ended = false;
        std::string forwarded_body;
        while (auto next = upstream.next_event()) {
            if (const auto* chunk = next->message_body_chunk()) {
                forwarded_body.append(chunk->bytes());
            }
            ended |= next->message_end() != nullptr;
        }
        RUVIA_CHECK(ended);
        RUVIA_CHECK_EQ(forwarded_body, body);
        RUVIA_CHECK(!upstream.connection_error());
    }
}

RUVIA_TEST(http2_public_trace_zero_length_accepts_header_or_empty_data_endings) {
    for (const bool head_end : {false, true}) {
        std::pmr::unsynchronized_pool_resource resource;
        auto wire = server_request_wire(&resource, {}, "TRACE", {}, head_end);
        if (!head_end) {
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, {});
        }
        auto server = ruvia::http2_connection::server({.resource_ = &resource});
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = server.next_event();
        auto* request = head ? head->request_head() : nullptr;
        RUVIA_CHECK(request != nullptr);
        if (!request) {
            continue;
        }
        const auto built_req = request->build_request(&resource);
        RUVIA_CHECK_EQ(built_req.method(), "TRACE");
        RUVIA_CHECK_EQ(built_req.header("Content-Length").value_or(""), "0");
        const auto end = server.next_event();
        RUVIA_CHECK(end && end->message_end() && end->message_end()->stream_id() == 1);
        RUVIA_CHECK(!server.next_event());
        ruvia::http_response reply(&resource);
        RUVIA_CHECK(submit_buffered_response(server, 1, reply) == ruvia::http2_submit_status::accepted);
        RUVIA_CHECK(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
    }
}

RUVIA_TEST(http2_public_known_empty_requests_never_wait_for_continue_or_send_content) {
    for (const std::string_view method : {"GET", "OPTIONS", "TRACE"}) {
        for (const bool streaming : {false, true}) {
            std::pmr::unsynchronized_pool_resource resource;
            auto client = ruvia::http2_connection::client({.resource_ = &resource});
            std::pmr::string wire(ruvia::testing::drain_output(client), &resource);
            ruvia::http2_regular_request_head_view head{
                .method_ = method, .authority_ = "example.test", .content_ = streaming ? ruvia::http2_request_content::streaming(0) : ruvia::http2_request_content::known_length(0), .expectation_ = ruvia::http_client_request_expectation::continue_value};
            const auto rejected = client.submit_request_head(head);
            RUVIA_CHECK(std::holds_alternative<ruvia::http2_request_head_submit_error>(rejected) &&
                        std::get<ruvia::http2_request_head_submit_error>(rejected) == ruvia::http2_request_head_submit_error::invalid_message);
            RUVIA_CHECK(!client.wants_write());
            head.expectation_ = ruvia::http_client_request_expectation::none;
            const auto submitted = client.submit_request_head(head);
            RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted) && std::get<std::uint32_t>(submitted) == 1);
            if (!std::holds_alternative<std::uint32_t>(submitted)) {
                continue;
            }
            RUVIA_CHECK(ruvia::testing::throws_on(
                [&] { (void)client.submit_data(1, "x", ruvia::http2_end_stream::keep_open, 65535); }));
            if (streaming) {
                RUVIA_CHECK(client.submit_data(1, {}, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
            }
            wire.append(ruvia::testing::drain_output(client));
            auto server = ruvia::http2_connection::server({.resource_ = &resource});
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request_event = server.next_event();
            auto* request = request_event ? request_event->request_head() : nullptr;
            RUVIA_CHECK(request != nullptr);
            if (!request) {
                continue;
            }
            const auto built_req = request->build_request(&resource);
            RUVIA_CHECK_EQ(built_req.method(), method);
            RUVIA_CHECK(!built_req.header("Content-Type"));
            RUVIA_CHECK(!built_req.header("Expect"));
            RUVIA_CHECK(built_req.header("Content-Length") ==
                        (method == "TRACE" ? std::nullopt : std::optional<std::string_view>{"0"}));
            const auto end = server.next_event();
            RUVIA_CHECK(end && end->message_end() && end->message_end()->stream_id() == 1);
            RUVIA_CHECK(!server.next_event());
            ruvia::http_response reply(&resource);
            RUVIA_CHECK(submit_buffered_response(server, 1, reply) == ruvia::http2_submit_status::accepted);
            RUVIA_CHECK(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
        }
    }
}

RUVIA_TEST(http2_public_options_without_content_type_preserves_body_and_following_stream) {
    for (const std::string_view body : {"", "abc"}) {
        for (const bool declared_length : {false, true}) {
            std::pmr::unsynchronized_pool_resource resource;
            std::pmr::string wire(ruvia::http2_client_preface, &resource);
            append_peer_settings(wire);
            std::pmr::string block(&resource);
            ruvia::hpack_encoder::encode_header(block, ":method", "OPTIONS");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
            ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
            ruvia::hpack_encoder::encode_header(block, ":path", "*");
            if (declared_length) {
                const auto length = std::to_string(body.size());
                ruvia::hpack_encoder::encode_header(block, "content-length", length);
            }
            append_header_section(wire, 1, block, false, true);
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, body);
            block.clear();
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
            ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
            ruvia::hpack_encoder::encode_header(block, ":path", "/next");
            append_header_section(wire, 3, block, true, false);
            auto server = ruvia::http2_connection::server({.resource_ = &resource});
            RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto first = server.next_event();
            auto* options = first ? first->request_head() : nullptr;
            RUVIA_CHECK(options != nullptr);
            if (options == nullptr) {
                continue;
            }
            const auto built_options = options->build_request(&resource);
            RUVIA_CHECK_EQ(built_options.method(), "OPTIONS");
            RUVIA_CHECK_EQ(built_options.path(), "*");
            RUVIA_CHECK(!built_options.header("Content-Type"));
            if (!body.empty()) {
                auto data = server.next_event();
                auto* chunk = data ? data->message_body_chunk() : nullptr;
                RUVIA_CHECK(chunk && chunk->stream_id() == 1 && chunk->bytes() == body);
                if (chunk) {
                    auto credit = chunk->take_credit();
                    (void)server.acknowledge(std::move(credit));
                }
            }
            const auto end = server.next_event();
            RUVIA_CHECK(end && end->message_end() && end->message_end()->stream_id() == 1);
            auto next = server.next_event();
            auto* following = next ? next->request_head() : nullptr;
            RUVIA_CHECK(following && following->stream_id() == 3 && following->request_view().path_ == "/next");
            const auto next_end = server.next_event();
            RUVIA_CHECK(next_end && next_end->message_end() && next_end->message_end()->stream_id() == 3);
            RUVIA_CHECK(!server.connection_error());
            for (auto* request : {options, following}) {
                if (request) {
                    ruvia::http_response reply(&resource);
                    RUVIA_CHECK(submit_buffered_response(server, request->stream_id(), reply) == ruvia::http2_submit_status::accepted);
                    RUVIA_CHECK(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
                }
            }
        }
    }
}

RUVIA_TEST(http2_public_opaque_metadata_preserves_request_and_response_bodies) {
    for (const bool server_role : {true, false}) {
        for (const std::string_view field_name : {"content-type", "content-encoding",
                 "origin", "access-control-request-method", "access-control-request-headers", "trailer"}) {
            for (const std::string_view value : {"", "not a media type", "text/plain; charset=\"unfinished"}) {
                std::pmr::unsynchronized_pool_resource resource;
                std::pmr::string block(&resource);
                if (server_role) {
                    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
                    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
                    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
                    ruvia::hpack_encoder::encode_header(block, ":path", "/");
                } else {
                    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
                }
                ruvia::hpack_encoder::encode_header(block, field_name, value);
                ruvia::hpack_encoder::encode_header(block, "content-length", "3");
                ruvia::hpack_encoder::encode_header(block, "x-after", "kept");
                std::pmr::string wire(&resource);
                if (server_role) {
                    wire.append(ruvia::http2_client_preface);
                }
                append_peer_settings(wire);
                append_header_section(wire, 1, block, false, true);
                append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "abc");
                auto connection = server_role ? ruvia::http2_connection::server({.resource_ = &resource})
                                              : ruvia::http2_connection::client({.resource_ = &resource});
                if (!server_role) {
                    const auto submitted = connection.submit_request_head(ruvia::http2_regular_request_head_view{
                        .authority_ = "example.test"});
                    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
                }
                RUVIA_CHECK(connection.feed(wire) == ruvia::http2_feed_result::accepted);
                auto head = connection.next_event();
                auto* request = head ? head->request_head() : nullptr;
                const auto* response = head ? head->response_head() : nullptr;
                RUVIA_CHECK(server_role ? request != nullptr : response != nullptr);
                if (server_role ? request == nullptr : response == nullptr) {
                    continue;
                }
                if (request != nullptr) {
                    const auto built_req = request->build_request(&resource);
                    RUVIA_CHECK(built_req.header(field_name) == std::optional<std::string_view>{value});
                    RUVIA_CHECK(built_req.header("x-after") == std::string_view("kept"));
                } else {
                    const auto fields = response->head().headers();
                    const auto metadata = std::find_if(fields.begin(), fields.end(), [field_name](const auto& field) {
                        return field.name() == field_name;
                    });
                    RUVIA_CHECK(metadata != fields.end() && metadata->value() == value);
                    const auto after = std::find_if(fields.begin(), fields.end(), [](const auto& field) {
                        return field.name() == "x-after";
                    });
                    RUVIA_CHECK(after != fields.end() && after->value() == "kept");
                }
                auto body = connection.next_event();
                auto* chunk = body ? body->message_body_chunk() : nullptr;
                RUVIA_CHECK(chunk != nullptr);
                if (chunk != nullptr) {
                    RUVIA_CHECK_EQ(chunk->bytes(), "abc");
                    auto credit = chunk->take_credit();
                    (void)connection.acknowledge(std::move(credit));
                }
                const auto end = connection.next_event();
                RUVIA_CHECK(end && end->message_end() && end->message_end()->stream_id() == 1);
                RUVIA_CHECK(!connection.connection_error());
                if (request != nullptr) {
                    ruvia::http_response reply(&resource);
                    RUVIA_CHECK(submit_buffered_response(connection, 1, reply) == ruvia::http2_submit_status::accepted);
                    RUVIA_CHECK(connection.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
                }
            }
        }
    }
}

RUVIA_TEST(http2_public_sender_rejects_invalid_request_metadata) {
    for (const auto& [name, value] :
        {std::pair<std::string_view, std::string_view>{"origin", "https://app.example/path"},
            {"access-control-request-method", "POST GET"}, {"access-control-request-headers", "x bad"},
            {"trailer", "Content-Length"}, {"trailer", "bad field"}, {"trailer", "\"x-name\""}}) {
        RUVIA_CHECK(!http2_client_submits_field(name, value));
    }
    RUVIA_CHECK(http2_client_submits_field("origin", "https://app.example"));
    RUVIA_CHECK(http2_server_accepts_field("origin", "https://app.example"));
}

RUVIA_TEST(http2_public_completed_response_preserves_request_send_half_and_retires_credits) {
    accounting_allocation_resource resource;
    for (const bool reset : {false, true}) {
        auto client = ruvia::http2_connection::client({.resource_ = &resource});
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http2_request_content::streaming(3)});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        const auto id = std::get<std::uint32_t>(request);
        auto wire = client_response_wire(&resource, "ok");
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        std::optional<ruvia::http2_received_data_credit> credit;
        bool ended = false;
        std::string received;
        while (auto event = client.next_event()) {
            if (auto* chunk = event->message_body_chunk()) {
                received.append(chunk->bytes());
                credit.emplace(chunk->take_credit());
            }
            ended |= event->message_end() != nullptr;
        }
        RUVIA_CHECK_EQ(received, "ok");
        RUVIA_CHECK(ended);
        if (!reset) {
            credit.reset();
        }
        RUVIA_CHECK(client.submit_data(id, "abc", ruvia::http2_end_stream::keep_open, 65535).status_ ==
                    ruvia::http2_data_submit_status::complete);
        if (reset) {
            RUVIA_CHECK(client.submit_reset(id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        } else {
            RUVIA_CHECK(client.finish_request(id) == ruvia::http2_finish_status::accepted);
        }
        credit.reset();
        while (client.next_event()) {
        }
        RUVIA_CHECK(!client.connection_error());
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http2_public_completed_response_keeps_unconsumed_request_suffix_retryable) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http2_request_content::known_length(65536)});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
    if (!std::holds_alternative<std::uint32_t>(request)) {
        return;
    }
    const auto id = std::get<std::uint32_t>(request);
    const std::string payload(65536, 'b');
    const auto submitted = client.submit_data(id, payload, ruvia::http2_end_stream::end_stream, payload.size());
    RUVIA_CHECK(submitted.status_ == ruvia::http2_data_submit_status::backpressured);
    RUVIA_CHECK_EQ(submitted.consumed_bytes_, std::size_t{65535});
    std::pmr::string output(&resource);
    ruvia::testing::drain_output(client, output);
    auto wire = client_response_wire(&resource, {});
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    bool ended = false;
    while (auto event = client.next_event()) {
        ended |= event->message_end() != nullptr;
    }
    RUVIA_CHECK(ended);
    RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), std::size_t{0});
    (void)ruvia::testing::drain_output(client);
    wire.clear();
    const std::string_view increment("\0\0\0\1", 4);
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, increment);
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, id, increment);
    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!client.wants_write());
    const auto retried = client.submit_data(id, std::string_view(payload).substr(submitted.consumed_bytes_),
        ruvia::http2_end_stream::end_stream, 1);
    RUVIA_CHECK(retried.status_ == ruvia::http2_data_submit_status::complete);
    RUVIA_CHECK_EQ(retried.consumed_bytes_, std::size_t{1});
    RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), std::size_t{1});
    const auto final_output = ruvia::testing::drain_output(client);
    const auto frame = ruvia::parse_http2_frame_header(final_output);
    RUVIA_CHECK(frame && frame->length_ == 1 && frame->stream_id_ == id && (frame->flags_ & 1U) != 0);
    RUVIA_CHECK_EQ(final_output.size(), ruvia::http2_frame_header_bytes + 1);
}

RUVIA_TEST(http2_public_data_consumption_is_bounded_and_does_not_retain_input_suffixes) {
    for (const std::uint32_t window : {0U, 2U, 65535U}) {
        for (const std::size_t allowance : {std::size_t{0}, std::size_t{1}, std::size_t{6}}) {
            toggle_allocation_resource resource;
            auto client = ruvia::http2_connection::client({.resource_ = &resource});
            std::pmr::string wire;
            const std::array<char, 6> setting{0, 4, static_cast<char>(window >> 24),
                static_cast<char>(window >> 16), static_cast<char>(window >> 8), static_cast<char>(window)};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view(setting.data(), setting.size()));
            RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
            const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
                .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http2_request_content::known_length(6)});
            RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
            if (!std::holds_alternative<std::uint32_t>(request)) {
                continue;
            }
            const auto id = std::get<std::uint32_t>(request);
            (void)ruvia::testing::drain_output(client);
            const auto expected_count = std::min({std::size_t{6}, static_cast<std::size_t>(window), allowance});
            ruvia::http2_data_submit_result submitted{};
            {
                std::string input = "abcdef";
                if (expected_count == 0) {
                    resource.reject();
                }
                submitted = client.submit_data(id, input, ruvia::http2_end_stream::end_stream, allowance);
                resource.reject(false);
                input.assign(input.size(), 'x');
            }
            RUVIA_CHECK_EQ(submitted.consumed_bytes_, expected_count);
            RUVIA_CHECK(submitted.status_ == (expected_count == 6 ? ruvia::http2_data_submit_status::complete
                                                                  : ruvia::http2_data_submit_status::backpressured));
            RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), expected_count);
            auto output = ruvia::testing::drain_output(client);
            std::string expected(std::string_view("abcdef").substr(0, expected_count));
            if (expected_count != 6) {
                wire.clear();
                append_frame(wire, ruvia::http2_frame_type::window_update, 0, id, std::string_view("\0\0\0\6", 4));
                RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
                RUVIA_CHECK(!client.wants_write());
                RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), std::size_t{0});
                const std::string replacement(6 - expected_count, 'z');
                const auto retried = client.submit_data(id, replacement, ruvia::http2_end_stream::end_stream, replacement.size());
                RUVIA_CHECK(retried.status_ == ruvia::http2_data_submit_status::complete);
                RUVIA_CHECK_EQ(retried.consumed_bytes_, replacement.size());
                expected.append(replacement);
                output.append(ruvia::testing::drain_output(client));
            }
            std::string received;
            unsigned terminal_frames = 0;
            std::string_view remaining(output);
            while (!remaining.empty()) {
                const auto frame = ruvia::parse_http2_frame_header(remaining);
                RUVIA_CHECK(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
                if (!frame || remaining.size() < ruvia::http2_frame_header_bytes + frame->length_) {
                    break;
                }
                received.append(remaining.substr(ruvia::http2_frame_header_bytes, frame->length_));
                terminal_frames += (frame->flags_ & 1U) != 0;
                if ((frame->flags_ & 1U) != 0) {
                    RUVIA_CHECK_EQ(received.size(), std::size_t{6});
                }
                remaining.remove_prefix(ruvia::http2_frame_header_bytes + frame->length_);
            }
            RUVIA_CHECK(remaining.empty());
            RUVIA_CHECK_EQ(received, expected);
            RUVIA_CHECK_EQ(terminal_frames, 1U);
            RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        }
    }
}

// RFC 9113 §6.9: even an empty DATA frame waits for positive connection and
// stream windows. An empty terminal submission or finish without trailers is
// backpressured without changing state until both windows open.
RUVIA_TEST(http2_public_empty_end_stream_waits_for_both_send_windows) {
    for (const bool finish : {false, true}) {
        auto client = ruvia::http2_connection::client();
        const auto filled_request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http2_request_content::known_length(65535)});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(filled_request));
        if (!std::holds_alternative<std::uint32_t>(filled_request)) {
            return;
        }
        const std::string filling(65535, 'f');
        const auto filled = client.submit_data(std::get<std::uint32_t>(filled_request), filling,
            ruvia::http2_end_stream::end_stream, filling.size());
        RUVIA_CHECK(filled.status_ == ruvia::http2_data_submit_status::complete);
        RUVIA_CHECK_EQ(filled.consumed_bytes_, filling.size());
        std::pmr::string wire;
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\0", 6));
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http2_request_content::streaming(0)});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::testing::drain_output(client);
        const auto end_stream = [&](bool sent) {
            if (finish) {
                RUVIA_CHECK(client.finish_request(id) ==
                            (sent ? ruvia::http2_finish_status::accepted : ruvia::http2_finish_status::backpressured));
                return;
            }
            const auto result = client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0);
            RUVIA_CHECK_EQ(result.consumed_bytes_, std::size_t{0});
            RUVIA_CHECK(result.status_ ==
                        (sent ? ruvia::http2_data_submit_status::complete : ruvia::http2_data_submit_status::backpressured));
        };
        const auto credit = [&](std::uint32_t stream_id) {
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::window_update, 0, stream_id, std::string_view("\0\0\0\1", 4));
            RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        };
        for (const std::uint32_t opened : {0U, id}) {
            const auto window = client.send_window_state(id);
            RUVIA_CHECK(window && (window->connection_window_ == 0 || window->stream_window_ == 0));
            end_stream(false);
            RUVIA_CHECK(!client.wants_write());
            RUVIA_CHECK(client.send_window_state(id).has_value());
            credit(opened);
        }
        end_stream(true);
        const auto output = ruvia::testing::drain_output(client);
        const auto frame = ruvia::parse_http2_frame_header(output);
        RUVIA_CHECK(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
                    frame->length_ == 0 && frame->stream_id_ == id && (frame->flags_ & 1U) != 0);
        RUVIA_CHECK_EQ(output.size(), ruvia::http2_frame_header_bytes);
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            (void)client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0);
        }));
        RUVIA_CHECK(!client.wants_write());
    }
}

RUVIA_TEST(http2_public_data_validates_terminal_length_before_partial_consumption) {
    for (const std::size_t allowance : {std::size_t{0}, std::size_t{2}, std::size_t{8}}) {
        auto client = ruvia::http2_connection::client();
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http2_request_content::known_length(4)});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            continue;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::testing::drain_output(client);
        const auto before = client.send_window_state(id);
        for (const std::string_view invalid : {"abc", "abcde"}) {
            RUVIA_CHECK(ruvia::testing::throws_on([&] {
                (void)client.submit_data(id, invalid, ruvia::http2_end_stream::end_stream, allowance);
            }));
            RUVIA_CHECK(!client.wants_write());
            RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), std::size_t{0});
            const auto after = client.send_window_state(id);
            RUVIA_CHECK(before && after && before->connection_window_ == after->connection_window_ &&
                        before->stream_window_ == after->stream_window_);
        }
        const auto first = client.submit_data(id, "abcd", ruvia::http2_end_stream::end_stream, 2);
        RUVIA_CHECK(first.status_ == ruvia::http2_data_submit_status::backpressured);
        RUVIA_CHECK_EQ(first.consumed_bytes_, std::size_t{2});
        RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), std::size_t{2});
        auto output = ruvia::testing::drain_output(client);
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            (void)client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0);
        }));
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            (void)client.submit_data(id, "cde", ruvia::http2_end_stream::end_stream, 0);
        }));
        RUVIA_CHECK(!client.wants_write());
        const auto second = client.submit_data(id, "cd", ruvia::http2_end_stream::end_stream, 1);
        RUVIA_CHECK(second.status_ == ruvia::http2_data_submit_status::backpressured);
        RUVIA_CHECK_EQ(second.consumed_bytes_, std::size_t{1});
        const auto last = client.submit_data(id, "d", ruvia::http2_end_stream::end_stream, 1);
        RUVIA_CHECK(last.status_ == ruvia::http2_data_submit_status::complete);
        RUVIA_CHECK_EQ(last.consumed_bytes_, std::size_t{1});
        output.append(ruvia::testing::drain_output(client));
        std::string_view remaining(output);
        for (const auto payload : {std::string_view("ab"), std::string_view("c"), std::string_view("d")}) {
            const auto frame = ruvia::parse_http2_frame_header(remaining);
            RUVIA_CHECK(frame && frame->length_ == payload.size() && ((frame->flags_ & 1U) != 0) == (payload == "d"));
            if (!frame || remaining.size() < ruvia::http2_frame_header_bytes + frame->length_) {
                break;
            }
            RUVIA_CHECK(remaining.substr(ruvia::http2_frame_header_bytes, frame->length_) == payload);
            remaining.remove_prefix(ruvia::http2_frame_header_bytes + frame->length_);
        }
        RUVIA_CHECK(remaining.empty());
    }
}

RUVIA_TEST(http2_public_response_data_counts_only_consumed_bytes_toward_content_length) {
    for (const bool buffered : {false, true}) {
        auto server = ruvia::http2_connection::server();
        const auto wire = server_request_wire(std::pmr::new_delete_resource(), {}, "GET");
        RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto request = server.next_event();
        auto* head = request ? request->request_head() : nullptr;
        RUVIA_CHECK(head != nullptr);
        if (head == nullptr) {
            continue;
        }
        RUVIA_CHECK(server.next_event()->message_end() != nullptr);
        ruvia::http_response response;
        if (buffered) {
            response.body("abcd");
            RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
                server.submit_response_head(1, response,
                    ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response))));
        } else {
            response.header("content-length", "4");
            RUVIA_CHECK(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
                server.submit_streaming_response_head(1, std::move(response))));
        }
        (void)ruvia::testing::drain_output(server);
        for (const std::string_view invalid : {"abc", "abcde"}) {
            RUVIA_CHECK(ruvia::testing::throws_on([&] {
                (void)server.submit_data(1, invalid, ruvia::http2_end_stream::end_stream, 2);
            }));
            RUVIA_CHECK(!server.wants_write());
            RUVIA_CHECK(server.send_window_state(1)->available_ == 65535);
        }
        const auto first = server.submit_data(1, "abcd", ruvia::http2_end_stream::end_stream, 2);
        RUVIA_CHECK(first.status_ == ruvia::http2_data_submit_status::backpressured);
        RUVIA_CHECK_EQ(first.consumed_bytes_, std::size_t{2});
        RUVIA_CHECK_EQ(server.pending_data_output_bytes(1), std::size_t{2});
        const auto prefix = ruvia::testing::drain_output(server);
        const auto prefix_frame = ruvia::parse_http2_frame_header(prefix);
        RUVIA_CHECK(prefix_frame && prefix_frame->length_ == 2 && (prefix_frame->flags_ & 1U) == 0);
        RUVIA_CHECK(std::string_view(prefix).substr(ruvia::http2_frame_header_bytes) == "ab");
        const auto trailers = ruvia::validate_http_response_trailers(std::span<const ruvia::http_header_view>{});
        RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)server.finish_response(1, trailers); }));
        RUVIA_CHECK(!server.wants_write());
        const auto final = server.submit_data(1, "cd", ruvia::http2_end_stream::end_stream, 2);
        RUVIA_CHECK(final.status_ == ruvia::http2_data_submit_status::complete);
        RUVIA_CHECK_EQ(final.consumed_bytes_, std::size_t{2});
        RUVIA_CHECK_EQ(server.pending_data_output_bytes(1), std::size_t{2});
        const auto suffix = ruvia::testing::drain_output(server);
        const auto suffix_frame = ruvia::parse_http2_frame_header(suffix);
        RUVIA_CHECK(suffix_frame && suffix_frame->length_ == 2 && (suffix_frame->flags_ & 1U) != 0);
        RUVIA_CHECK(std::string_view(suffix).substr(ruvia::http2_frame_header_bytes) == "cd");
        RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    }
}

RUVIA_TEST(http2_public_expectation_gate_does_not_consume_or_retain_data) {
    for (const bool timeout_release : {false, true}) {
        auto client = ruvia::http2_connection::client();
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http2_request_content::known_length(4), .expectation_ = ruvia::http_client_request_expectation::continue_value});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            continue;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::testing::drain_output(client);
        {
            const std::string input = "body";
            const auto pending = client.submit_data(id, input, ruvia::http2_end_stream::end_stream, input.size());
            RUVIA_CHECK(pending.status_ == ruvia::http2_data_submit_status::expectation_pending);
            RUVIA_CHECK_EQ(pending.consumed_bytes_, std::size_t{0});
        }
        RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        RUVIA_CHECK(!client.wants_write());
        if (timeout_release) {
            (void)client.release_request_content(id);
        } else {
            std::pmr::string wire;
            append_peer_settings(wire);
            std::pmr::string block;
            ruvia::hpack_encoder::encode_header(block, ":status", "100");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x4, id, block);
            RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
            while (client.next_event()) {
            }
            (void)ruvia::testing::drain_output(client);
        }
        RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        const auto retried = client.submit_data(id, "next", ruvia::http2_end_stream::end_stream, 4);
        RUVIA_CHECK(retried.status_ == ruvia::http2_data_submit_status::complete);
        RUVIA_CHECK_EQ(retried.consumed_bytes_, std::size_t{4});
        const auto output = ruvia::testing::drain_output(client);
        const auto frame = ruvia::parse_http2_frame_header(output);
        RUVIA_CHECK(frame && frame->length_ == 4 && (frame->flags_ & 1U) != 0);
        RUVIA_CHECK(std::string_view(output).substr(ruvia::http2_frame_header_bytes) == "next");
    }
}

RUVIA_TEST(http2_public_reset_and_goaway_discard_no_caller_owned_suffix) {
    for (const bool goaway : {false, true}) {
        auto client = ruvia::http2_connection::client();
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http2_request_content::known_length(4)});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            continue;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::testing::drain_output(client);
        const auto submitted = client.submit_data(id, "body", ruvia::http2_end_stream::end_stream, 2);
        RUVIA_CHECK(submitted.status_ == ruvia::http2_data_submit_status::backpressured);
        RUVIA_CHECK_EQ(submitted.consumed_bytes_, std::size_t{2});
        (void)ruvia::testing::drain_output(client);
        std::pmr::string wire;
        append_peer_settings(wire);
        if (goaway) {
            append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, std::string_view("\0\0\0\0\0\0\0\0", 8));
        } else {
            append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, id, std::string_view("\0\0\0\10", 4));
        }
        RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        bool closed = false;
        while (auto event = client.next_event()) {
            closed |= goaway ? event->request_unprocessed() != nullptr : event->stream_closed() != nullptr;
        }
        RUVIA_CHECK(closed);
        RUVIA_CHECK(client.stream_aborted(id));
        (void)ruvia::testing::drain_output(client);
        const auto retry = client.submit_data(id, "dy", ruvia::http2_end_stream::end_stream, 2);
        RUVIA_CHECK(retry.status_ == ruvia::http2_data_submit_status::closed);
        RUVIA_CHECK_EQ(retry.consumed_bytes_, std::size_t{0});
        RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        RUVIA_CHECK(!client.wants_write());
    }
}

// Window-clamped DATA is serialized completely within each submission, so
// streams keep their submission order and both send windows stay exact.
RUVIA_TEST(http2_public_window_clamped_data_preserves_stream_order_and_windows) {
    {
        std::pmr::unsynchronized_pool_resource resource;
        auto client = ruvia::http2_connection::client({.resource_ = &resource});
        std::pmr::string wire;
        const auto set_window = [&](std::uint32_t size) {
            wire.clear();
            const std::array<char, 6> setting{0, 4, static_cast<char>(size >> 24),
                static_cast<char>(size >> 16), static_cast<char>(size >> 8), static_cast<char>(size)};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view(setting.data(), setting.size()));
            RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
        };
        set_window(0);
        const std::array bodies{std::string(4097, 'a'), std::string(1024, 'b'), std::string(8192, 'd')};
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
                .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http2_request_content::known_length(bodies[i].size())});
            RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
            const auto id = static_cast<std::uint32_t>(2 * i + 1);
            const auto blocked = client.submit_data(id, bodies[i], ruvia::http2_end_stream::end_stream, bodies[i].size());
            RUVIA_CHECK(blocked.status_ == ruvia::http2_data_submit_status::backpressured);
            RUVIA_CHECK_EQ(blocked.consumed_bytes_, std::size_t{0});
        }
        {
            std::pmr::string discarded(&resource);
            ruvia::testing::drain_output(client, discarded);
        }
        std::vector<std::uint32_t> frame_order;
        std::array<std::string, 3> received;
        std::array<unsigned, 3> terminal_frames{};
        const auto collect = [&] {
            const auto output = ruvia::testing::drain_output(client);
            std::string_view remaining(output);
            while (!remaining.empty()) {
                const auto frame = ruvia::parse_http2_frame_header(std::span(remaining.data(), remaining.size()));
                RUVIA_CHECK(frame && remaining.size() >= ruvia::http2_frame_header_bytes + frame->length_);
                if (!frame || remaining.size() < ruvia::http2_frame_header_bytes + frame->length_) {
                    break;
                }
                if (frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    const auto index = (frame->stream_id_ - 1) / 2;
                    RUVIA_CHECK(index < received.size());
                    if (index < received.size()) {
                        frame_order.push_back(frame->stream_id_);
                        received[index].append(remaining.substr(ruvia::http2_frame_header_bytes, frame->length_));
                        terminal_frames[index] += (frame->flags_ & 1U) != 0;
                    }
                }
                remaining.remove_prefix(ruvia::http2_frame_header_bytes + frame->length_);
            }
        };
        set_window(4096);
        collect();
        RUVIA_CHECK(frame_order.empty());
        std::array<std::size_t, 3> consumed{};
        const auto first = client.submit_data(1, bodies[0], ruvia::http2_end_stream::end_stream, 4096);
        consumed[0] = first.consumed_bytes_;
        RUVIA_CHECK(first.status_ == ruvia::http2_data_submit_status::backpressured);
        RUVIA_CHECK_EQ(consumed[0], std::size_t{4096});
        for (std::size_t i = 1; i < bodies.size(); ++i) {
            const auto result = client.submit_data(static_cast<std::uint32_t>(2 * i + 1),
                bodies[i], ruvia::http2_end_stream::end_stream, 4096);
            consumed[i] = result.consumed_bytes_;
            RUVIA_CHECK(result.status_ == (i == 1 ? ruvia::http2_data_submit_status::complete
                                                  : ruvia::http2_data_submit_status::backpressured));
        }
        collect();
        RUVIA_CHECK_EQ(consumed[1], std::size_t{1024});
        RUVIA_CHECK_EQ(consumed[2], std::size_t{4096});
        set_window(8192);
        collect();
        RUVIA_CHECK((frame_order == std::vector<std::uint32_t>{1, 3, 5}));
        for (const std::size_t i : {std::size_t{0}, std::size_t{2}}) {
            const auto suffix = std::string_view(bodies[i]).substr(consumed[i]);
            const auto result = client.submit_data(static_cast<std::uint32_t>(2 * i + 1),
                suffix, ruvia::http2_end_stream::end_stream, suffix.size());
            RUVIA_CHECK(result.status_ == ruvia::http2_data_submit_status::complete);
            RUVIA_CHECK_EQ(result.consumed_bytes_, suffix.size());
            consumed[i] += result.consumed_bytes_;
        }
        collect();
        RUVIA_CHECK((frame_order == std::vector<std::uint32_t>{1, 3, 5, 1, 5}));
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const auto id = static_cast<std::uint32_t>(2 * i + 1);
            RUVIA_CHECK(received[i] == bodies[i]);
            RUVIA_CHECK_EQ(terminal_frames[i], 1U);
            RUVIA_CHECK_EQ(consumed[i], bodies[i].size());
            const auto window = client.send_window_state(id);
            RUVIA_CHECK(window && window->connection_window_ == 52222 &&
                        window->stream_window_ == static_cast<std::int32_t>(8192 - bodies[i].size()));
        }
    }
}

RUVIA_TEST(http2_output_observer_can_submit_new_data_without_losing_it) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http2_request_content::streaming()});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
    if (!std::holds_alternative<std::uint32_t>(request)) {
        return;
    }
    const auto id = std::get<std::uint32_t>(request);
    (void)ruvia::testing::drain_output(client);
    RUVIA_CHECK(client.submit_data(id, "first", ruvia::http2_end_stream::keep_open, 65535).status_ ==
                ruvia::http2_data_submit_status::complete);
    struct observation final {
        ruvia::http2_connection* client_;
        std::optional<ruvia::http2_data_submit_result> submitted_;
        bool failed_{};
    } observed{&client};
    std::pmr::string first(&resource);
    const auto batch = client.take_output_batch(16384, first, [](void* context, std::uint32_t stream, std::size_t) noexcept {
            auto& state = *static_cast<observation*>(context);
            try {
                state.submitted_ = state.client_->submit_data(
                    stream, "second", ruvia::http2_end_stream::end_stream, 65535);
            } catch (...) {
                state.failed_ = true;
            } }, &observed);
    RUVIA_CHECK(batch.status_ == ruvia::http2_output_batch_status::taken);
    RUVIA_CHECK(!observed.failed_);
    RUVIA_CHECK(observed.submitted_ && observed.submitted_->status_ == ruvia::http2_data_submit_status::complete);
    RUVIA_CHECK(observed.submitted_ && observed.submitted_->consumed_bytes_ == 6);
    RUVIA_CHECK(client.wants_write());
    std::pmr::string second(&resource);
    ruvia::testing::drain_output(client, second);
    const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(second.data(), second.size()));
    RUVIA_CHECK(frame && frame->stream_id_ == id && frame->length_ == 6);
    if (frame) {
        RUVIA_CHECK(std::string_view(second).substr(ruvia::http2_frame_header_bytes) == "second");
    }
}

RUVIA_TEST(http2_output_observer_can_recursively_drain_and_append_frames) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http2_request_content::streaming()});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
    if (!std::holds_alternative<std::uint32_t>(request)) {
        return;
    }
    const auto id = std::get<std::uint32_t>(request);
    (void)ruvia::testing::drain_output(client);
    for (const auto payload : {"a", "b", "c", "d"}) {
        RUVIA_CHECK(client.submit_data(id, payload, ruvia::http2_end_stream::keep_open, 65535).status_ ==
                    ruvia::http2_data_submit_status::complete);
    }
    struct observation final {
        ruvia::http2_connection* client_;
        std::pmr::string* nested_;
        std::size_t calls_{};
        bool failed_{};
        static void on_data(void* context, std::uint32_t stream, std::size_t bytes) noexcept {
            auto& state = *static_cast<observation*>(context);
            ++state.calls_;
            if (bytes != 1) {
                state.failed_ = true;
            }
            if (state.calls_ != 1) {
                return;
            }
            try {
                const auto batch = state.client_->take_output_batch(16384, *state.nested_, &on_data, context);
                state.failed_ |= batch.status_ != ruvia::http2_output_batch_status::taken;
                for (int i = 0; i < 40; ++i) {
                    state.failed_ |= state.client_->submit_data(stream, "x", ruvia::http2_end_stream::keep_open, 65535).status_ !=
                                     ruvia::http2_data_submit_status::complete;
                }
                state.failed_ |= state.client_->submit_data(stream, "z", ruvia::http2_end_stream::end_stream, 65535).status_ !=
                                 ruvia::http2_data_submit_status::complete;
            } catch (...) {
                state.failed_ = true;
            }
        }
    };
    std::pmr::string nested(&resource);
    observation observed{&client, &nested};
    std::pmr::string first(&resource);
    const auto batch = client.take_output_batch(20, first, &observation::on_data, &observed);
    RUVIA_CHECK(batch.status_ == ruvia::http2_output_batch_status::taken);
    RUVIA_CHECK(!observed.failed_);
    RUVIA_CHECK_EQ(observed.calls_, std::size_t{4});
    RUVIA_CHECK_EQ(first.size(), std::size_t{20});
    RUVIA_CHECK_EQ(nested.size(), std::size_t{20});
    std::pmr::string remaining(&resource);
    ruvia::testing::drain_output(client, remaining);
    std::string payload;
    std::size_t offset = 0;
    while (offset < remaining.size()) {
        const auto frame = ruvia::parse_http2_frame_header(
            std::span<const char>(remaining.data() + offset, remaining.size() - offset));
        if (!frame || frame->stream_id_ != id || frame->length_ != 1 ||
            remaining.size() - offset < ruvia::http2_frame_header_bytes + frame->length_) {
            break;
        }
        payload.push_back(remaining[offset + ruvia::http2_frame_header_bytes]);
        offset += ruvia::http2_frame_header_bytes + frame->length_;
    }
    RUVIA_CHECK_EQ(offset, remaining.size());
    RUVIA_CHECK(payload == std::string(40, 'x') + "z");
}

RUVIA_TEST(http2_public_full_output_batch_transfers_storage_and_accounts_data) {
    toggle_allocation_resource resource;
    {
        auto client = ruvia::http2_connection::client({.resource_ = &resource});
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http2_request_content::streaming()});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::testing::drain_output(client);
        const std::string body(8192, 'b');
        RUVIA_CHECK(client.submit_data(id, body, ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
        std::pmr::string output(&resource);
        data_output_observations observed;
        ruvia::http2_output_batch_result batch;
        bool threw = false;
        if (ruvia::test::stl_allocation_failure_injection_enabled(ruvia_ctx)) {
            resource.reject();
        }
        try {
            batch = client.take_output_batch(16384, output, &data_output_observations::observe, &observed);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        resource.reject(false);
        RUVIA_CHECK(!threw);
        if (threw) {
            return;
        }
        RUVIA_CHECK(batch.status_ == ruvia::http2_output_batch_status::taken);
        RUVIA_CHECK_EQ(batch.bytes_, body.size() + ruvia::http2_frame_header_bytes);
        const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
        RUVIA_CHECK(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
                    frame->stream_id_ == id && frame->length_ == body.size() && frame->flags_ == 0);
        if (frame) {
            RUVIA_CHECK(std::string_view(output).substr(ruvia::http2_frame_header_bytes) == body);
        }
        RUVIA_CHECK(!client.wants_write());
        RUVIA_CHECK_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        RUVIA_CHECK_EQ(observed.count_, std::size_t{1});
        RUVIA_CHECK(observed.entries_[0].stream_id_ == id && observed.entries_[0].payload_bytes_ == body.size());

        output.clear();
        RUVIA_CHECK(client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
        batch = client.take_output_batch(0, output, &data_output_observations::observe, &observed);
        RUVIA_CHECK(batch.status_ == ruvia::http2_output_batch_status::taken);
        RUVIA_CHECK_EQ(batch.bytes_, ruvia::http2_frame_header_bytes);
        const auto empty_end = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
        RUVIA_CHECK(empty_end && empty_end->stream_id_ == id && empty_end->length_ == 0 && empty_end->flags_ == 1);
        RUVIA_CHECK_EQ(observed.count_, std::size_t{2});
        RUVIA_CHECK(observed.entries_[1].stream_id_ == id && observed.entries_[1].payload_bytes_ == 0);

        const auto next = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/next", .content_ = ruvia::http2_request_content::streaming()});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(next));
        if (!std::holds_alternative<std::uint32_t>(next)) {
            return;
        }
        const auto next_id = std::get<std::uint32_t>(next);
        (void)ruvia::testing::drain_output(client);
        RUVIA_CHECK(client.submit_data(next_id, "tail", ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
        batch = client.take_output_batch(16384, output, &data_output_observations::observe, &observed);
        RUVIA_CHECK(batch.status_ == ruvia::http2_output_batch_status::taken);
        RUVIA_CHECK_EQ(batch.bytes_, ruvia::http2_frame_header_bytes + 4);
        RUVIA_CHECK_EQ(output.size(), 2 * ruvia::http2_frame_header_bytes + 4);
        if (output.size() >= ruvia::http2_frame_header_bytes) {
            const auto tail = ruvia::parse_http2_frame_header(
                std::span<const char>(output.data(), output.size()).subspan(ruvia::http2_frame_header_bytes));
            RUVIA_CHECK(tail && tail->stream_id_ == next_id && tail->length_ == 4 && tail->flags_ == 1);
            if (tail) {
                RUVIA_CHECK(std::string_view(output).substr(2 * ruvia::http2_frame_header_bytes) == "tail");
            }
        }
        RUVIA_CHECK_EQ(observed.count_, std::size_t{3});
        RUVIA_CHECK(observed.entries_[2].stream_id_ == next_id && observed.entries_[2].payload_bytes_ == 4);
        RUVIA_CHECK(!observed.overflow_);
        RUVIA_CHECK(client.take_output_batch(16384, output, &data_output_observations::observe, &observed).status_ == ruvia::http2_output_batch_status::empty);
        RUVIA_CHECK_EQ(observed.count_, std::size_t{3});
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http2_public_output_batch_copy_failure_preserves_frame_boundaries) {
    std::pmr::unsynchronized_pool_resource resource;
    toggle_allocation_resource output_resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    std::array<std::uint32_t, 3> ids{};
    const std::array<std::string, 3> bodies{std::string(32, 'a'), std::string(16, 'b'), std::string(8, 'c')};
    for (auto& id : ids) {
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http2_request_content::streaming()});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        id = std::get<std::uint32_t>(request);
    }
    (void)ruvia::testing::drain_output(client);
    for (std::size_t index = 0; index < ids.size(); ++index) {
        RUVIA_CHECK(client.submit_data(ids[index], bodies[index], ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
    }
    std::pmr::string copied(&output_resource);
    data_output_observations observed;
    output_resource.reject();
    bool threw = false;
    try {
        (void)client.take_output_batch((std::numeric_limits<std::size_t>::max)(), copied, &data_output_observations::observe, &observed);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    output_resource.reject(false);
    RUVIA_CHECK(threw);
    RUVIA_CHECK(copied.empty());
    RUVIA_CHECK(client.wants_write());
    RUVIA_CHECK_EQ(observed.count_, std::size_t{0});

    copied = "prefix";
    const auto first_bytes = ruvia::http2_frame_header_bytes + bodies[0].size();
    auto batch = client.take_output_batch(first_bytes, copied, &data_output_observations::observe, &observed);
    RUVIA_CHECK(batch.status_ == ruvia::http2_output_batch_status::taken);
    RUVIA_CHECK_EQ(batch.bytes_, first_bytes);
    RUVIA_CHECK(std::string_view(copied).starts_with("prefix"));
    if (copied.size() >= 6) {
        const auto first = ruvia::parse_http2_frame_header(std::span<const char>(copied.data(), copied.size()).subspan(6));
        RUVIA_CHECK(first && first->stream_id_ == ids[0] && first->length_ == bodies[0].size());
        if (first) {
            RUVIA_CHECK(std::string_view(copied).substr(6 + ruvia::http2_frame_header_bytes) == bodies[0]);
        }
    }
    RUVIA_CHECK_EQ(client.pending_data_output_bytes(ids[0]), std::size_t{0});
    RUVIA_CHECK_EQ(client.pending_data_output_bytes(ids[1]), bodies[1].size());
    RUVIA_CHECK_EQ(observed.count_, std::size_t{1});
    RUVIA_CHECK(observed.entries_[0].stream_id_ == ids[0] && observed.entries_[0].payload_bytes_ == bodies[0].size());

    // A unit budget still moves exactly one whole frame per batch.
    std::pmr::string suffix(&resource);
    batch = client.take_output_batch(1, suffix, &data_output_observations::observe, &observed);
    RUVIA_CHECK(batch.status_ == ruvia::http2_output_batch_status::taken);
    RUVIA_CHECK_EQ(batch.bytes_, ruvia::http2_frame_header_bytes + bodies[1].size());
    RUVIA_CHECK_EQ(client.pending_data_output_bytes(ids[1]), std::size_t{0});
    RUVIA_CHECK_EQ(observed.count_, std::size_t{2});
    suffix.clear();
    batch = client.take_output_batch(1, suffix, &data_output_observations::observe, &observed);
    RUVIA_CHECK(batch.status_ == ruvia::http2_output_batch_status::taken);
    RUVIA_CHECK_EQ(batch.bytes_, ruvia::http2_frame_header_bytes + bodies[2].size());
    const auto last = ruvia::parse_http2_frame_header(std::span<const char>(suffix.data(), suffix.size()));
    RUVIA_CHECK(last && last->stream_id_ == ids[2] && last->length_ == bodies[2].size());
    if (last) {
        RUVIA_CHECK(std::string_view(suffix).substr(ruvia::http2_frame_header_bytes) == bodies[2]);
    }
    RUVIA_CHECK(!client.wants_write());
    RUVIA_CHECK_EQ(observed.count_, std::size_t{3});
    RUVIA_CHECK(observed.entries_[1].stream_id_ == ids[1] && observed.entries_[1].payload_bytes_ == bodies[1].size());
    RUVIA_CHECK(observed.entries_[2].stream_id_ == ids[2] && observed.entries_[2].payload_bytes_ == bodies[2].size());
    RUVIA_CHECK(!observed.overflow_);
}

RUVIA_TEST(http2_public_websocket_handshake_validates_required_subprotocols_and_configures_deflate) {
    std::pmr::unsynchronized_pool_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);

    std::pmr::string wire(ruvia::http2_client_preface, &resource);
    append_peer_settings(wire);

    std::pmr::string block1(&resource);
    ruvia::hpack_encoder::encode_header(block1, ":method", "CONNECT");
    ruvia::hpack_encoder::encode_header(block1, ":protocol", "websocket");
    ruvia::hpack_encoder::encode_header(block1, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block1, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block1, ":path", "/ws");
    ruvia::hpack_encoder::encode_header(block1, "sec-websocket-version", "13");
    ruvia::hpack_encoder::encode_header(block1, "sec-websocket-protocol", "other, chat.v1");
    ruvia::hpack_encoder::encode_header(
        block1, "sec-websocket-extensions", "permessage-deflate; client_no_context_takeover");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block1);

    std::pmr::string block3(&resource);
    ruvia::hpack_encoder::encode_header(block3, ":method", "CONNECT");
    ruvia::hpack_encoder::encode_header(block3, ":protocol", "websocket");
    ruvia::hpack_encoder::encode_header(block3, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block3, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block3, ":path", "/ws");
    ruvia::hpack_encoder::encode_header(block3, "sec-websocket-version", "13");
    ruvia::hpack_encoder::encode_header(block3, "sec-websocket-extensions", "permessage-deflate");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 3, block3);

    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto event1 = server.next_event();
    auto* head1 = event1 ? event1->request_head() : nullptr;
    RUVIA_CHECK(head1 != nullptr);
    if (head1 == nullptr) {
        return;
    }

    const auto built_req1 = head1->build_request(&resource);
    constexpr std::array<std::string_view, 1> unmatched{"chat.v2"};
    const auto rejected = ruvia::validate_http2_websocket_handshake(server, 1, built_req1,
        {.supported_subprotocols_ = unmatched, .require_subprotocol_ = true});
    RUVIA_CHECK(rejected.accepted() == nullptr);
    RUVIA_CHECK(rejected.failure() != nullptr);
    if (const auto* failure = rejected.failure()) {
        RUVIA_CHECK_EQ(failure->protocol_error().status(), ruvia::http_status::bad_request);
    }

    const auto optional_subprotocol = ruvia::validate_http2_websocket_handshake(server, 1, built_req1,
        {.supported_subprotocols_ = unmatched, .require_subprotocol_ = false});
    RUVIA_CHECK(optional_subprotocol.accepted() != nullptr);

    constexpr std::array<std::string_view, 2> supported{"chat.v2", "chat.v1"};
    const auto accepted1 = ruvia::validate_http2_websocket_handshake(server, 1, built_req1,
        {.supported_subprotocols_ = supported, .require_subprotocol_ = true});
    RUVIA_CHECK(accepted1.accepted() != nullptr);

    const auto submitted1 = server.submit_websocket_handshake(1, built_req1, accepted1,
        {
            .supported_subprotocols_ = supported,
            .deflate_ = {.enabled_ = true, .context_takeover_ = true},
        });
    RUVIA_CHECK(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted1));
    if (const auto* negotiation = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted1)) {
        RUVIA_CHECK_EQ(negotiation->subprotocol(), std::string_view("chat.v1"));
        RUVIA_CHECK(negotiation->compression().enabled_);
        RUVIA_CHECK(!negotiation->compression().server_no_context_takeover_);
        RUVIA_CHECK(negotiation->compression().client_no_context_takeover_);
    }
    RUVIA_CHECK(server.release(std::move(*head1)) == ruvia::http2_server_request_release_status::released);

    auto event3 = server.next_event();
    auto* head3 = event3 ? event3->request_head() : nullptr;
    RUVIA_CHECK(head3 != nullptr);
    if (head3 == nullptr) {
        return;
    }

    const auto built_req3 = head3->build_request(&resource);
    const auto rejected3 = ruvia::validate_http2_websocket_handshake(server, 3, built_req3,
        {.supported_subprotocols_ = supported, .require_subprotocol_ = true});
    RUVIA_CHECK(rejected3.accepted() == nullptr);
    RUVIA_CHECK(rejected3.failure() != nullptr);

    const auto accepted3 = ruvia::validate_http2_websocket_handshake(server, 3, built_req3);
    RUVIA_CHECK(accepted3.accepted() != nullptr);
    const auto submitted3 = server.submit_websocket_handshake(3, built_req3, accepted3,
        {
            .deflate_ = {.enabled_ = false},
        });
    RUVIA_CHECK(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted3));
    if (const auto* negotiation = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted3)) {
        RUVIA_CHECK(negotiation->subprotocol().empty());
        RUVIA_CHECK(!negotiation->compression().enabled_);
    }
    RUVIA_CHECK(server.release(std::move(*head3)) == ruvia::http2_server_request_release_status::released);
}

RUVIA_TEST(http2_public_websocket_handshake_ignores_rfc6455_key_exchange_fields) {
    // RFC 8441 section 5: :protocol supersedes Sec-WebSocket-Key/-Accept, which
    // a recipient does not process; it neither validates nor rejects them.
    std::pmr::unsynchronized_pool_resource resource;
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    (void)ruvia::testing::drain_output(server);

    std::pmr::string wire(ruvia::http2_client_preface, &resource);
    append_peer_settings(wire);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> extra{{
        {"sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ=="},
        {"sec-websocket-accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="},
        {"sec-websocket-protocol", "chat, chat"},
    }};
    std::uint32_t stream_id = 1;
    for (const auto& [name, value] : extra) {
        std::pmr::string block(&resource);
        ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
        ruvia::hpack_encoder::encode_header(block, ":protocol", "websocket");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/ws");
        ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
        ruvia::hpack_encoder::encode_header(block, name, value);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, stream_id, block);
        stream_id += 2;
    }
    RUVIA_CHECK(server.feed(wire) == ruvia::http2_feed_result::accepted);

    constexpr std::array<std::string_view, 1> supported{"chat"};
    for (std::size_t i = 0; i < extra.size(); ++i) {
        auto event = server.next_event();
        auto* head = event ? event->request_head() : nullptr;
        RUVIA_CHECK(head != nullptr);
        if (head == nullptr) {
            return;
        }
        const auto id = head->stream_id();
        const auto request = head->build_request(&resource);
        const auto validation = ruvia::validate_http2_websocket_handshake(server, id, request,
            {.supported_subprotocols_ = supported, .require_subprotocol_ = i == 2});
        RUVIA_CHECK(validation.accepted() != nullptr);
        const auto submitted =
            server.submit_websocket_handshake(id, request, validation, {.supported_subprotocols_ = supported});
        RUVIA_CHECK(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted));
        if (const auto* negotiation = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted)) {
            RUVIA_CHECK_EQ(negotiation->subprotocol(), std::string_view(i == 2 ? "chat" : ""));
        }
        RUVIA_CHECK(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    }
}

RUVIA_TEST(http2_public_outbound_request_headers_and_trailers_lowercase_standard_tokens) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    ruvia::testing::output_relay relay;
    auto exchange_value = [&](auto& from, auto& to) {
        RUVIA_CHECK(relay.feed(from, to) == ruvia::http2_feed_result::accepted);
    };
    exchange_value(client, server);
    exchange_value(server, client);

    const std::string long_mixed_name = "X-Mixed-" + std::string(80, 'N');
    const std::string long_lower_name = "x-mixed-" + std::string(80, 'n');
    const std::array<ruvia::http_header_view, 2> headers{{{long_mixed_name, "head-value"}, {"X-Custom-Head", "short"}}};
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/mixed", .headers_ = headers, .content_ = ruvia::http2_request_content::streaming()});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
    const std::array<ruvia::http_header_view, 2> trailers{
        {{"X-Mixed-Trailer", "tail-value"}, {"X-Second-Trailer", "tail-two"}}};
    RUVIA_CHECK(client.finish_request(1, trailers) == ruvia::http2_finish_status::accepted);
    exchange_value(client, server);

    std::optional<ruvia::http2_request_head_event> parent;
    while (auto event = server.next_event()) {
        if (auto* head = event->request_head()) {
            const auto built = head->build_request(&resource);
            RUVIA_CHECK_EQ(built.header(long_lower_name).value_or(""), "head-value");
            RUVIA_CHECK_EQ(built.header("x-custom-head").value_or(""), "short");
            parent.emplace(std::move(*head));
        } else if (const auto* end = event->message_end()) {
            RUVIA_CHECK_EQ(end->trailers().size(), std::size_t{2});
            if (end->trailers().size() == 2) {
                RUVIA_CHECK_EQ(end->trailers()[0].name(), "x-mixed-trailer");
                RUVIA_CHECK_EQ(end->trailers()[0].value(), "tail-value");
                RUVIA_CHECK_EQ(end->trailers()[1].name(), "x-second-trailer");
                RUVIA_CHECK_EQ(end->trailers()[1].value(), "tail-two");
            }
        }
    }
    RUVIA_CHECK(parent.has_value());
    const std::array<ruvia::http_header_view, 1> push_headers{{{"X-Push-Mixed", "pushed-value"}}};
    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/pushed", .headers_ = push_headers});
    RUVIA_CHECK(pushed.index() == 0);
    if (pushed.index() == 0) {
        const auto built_push = std::get<0>(pushed).build_request(&resource);
        RUVIA_CHECK_EQ(built_push.header("x-push-mixed").value_or(""), "pushed-value");
        (void)server.release(std::move(std::get<0>(pushed)));
    }
    exchange_value(server, client);
    auto promise = client.next_event();
    RUVIA_CHECK(promise && promise->push_promise());
    if (promise && promise->push_promise()) {
        RUVIA_CHECK_EQ(promise->push_promise()->request_.headers_.size(), std::size_t{1});
        if (!promise->push_promise()->request_.headers_.empty()) {
            RUVIA_CHECK_EQ(promise->push_promise()->request_.headers_[0].name(), "x-push-mixed");
        }
    }
    if (parent) {
        (void)server.release(std::move(*parent));
    }
}

RUVIA_TEST(http2_public_rejected_connect_with_invalid_terminal_length_resets_without_data_frame) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    const auto submitted = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
    if (!std::holds_alternative<std::uint32_t>(submitted)) {
        return;
    }
    const auto stream_id = std::get<std::uint32_t>(submitted);
    (void)ruvia::testing::drain_output(client);

    std::pmr::string wire(&resource);
    append_peer_settings(wire);
    std::pmr::string block(&resource);
    ruvia::hpack_encoder::encode_header(block, ":status", "403");
    ruvia::hpack_encoder::encode_header(block, "content-length", "5");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, stream_id, block);

    RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
    const auto closed = client.next_event();
    RUVIA_CHECK(closed && closed->stream_closed());
    if (closed && closed->stream_closed()) {
        RUVIA_CHECK_EQ(closed->stream_closed()->stream_id(), stream_id);
        RUVIA_CHECK(closed->stream_closed()->error() == ruvia::http2_error_code::protocol_error);
    }

    const auto out = ruvia::testing::drain_output(client);
    // Peer SETTINGS generates a 9-byte SETTINGS ACK, followed only by the 13-byte RST_STREAM
    // (no spurious 9-byte DATA(END_STREAM) frame may be emitted before RST_STREAM).
    RUVIA_CHECK_EQ(out.size(), ruvia::http2_frame_header_bytes + ruvia::http2_frame_header_bytes + 4);
    if (out.size() >= ruvia::http2_frame_header_bytes * 2 + 4) {
        const auto rst = ruvia::parse_http2_frame_header(
            std::span(out.data() + ruvia::http2_frame_header_bytes, ruvia::http2_frame_header_bytes));
        RUVIA_CHECK(rst.has_value());
        if (rst) {
            RUVIA_CHECK_EQ(rst->type_, static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
            RUVIA_CHECK_EQ(rst->stream_id_, stream_id);
            RUVIA_CHECK_EQ(rst->length_, std::uint32_t{4});
        }
    }
}

// H2-02: Client receiving RST_STREAM(REFUSED_STREAM) after a 1xx informational
// response treats it as a stream reset rather than an unprocessed request.
RUVIA_TEST(http2_public_client_refused_stream_after_informational_is_not_unprocessed) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto sub = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(sub));
    const auto sid = std::get<std::uint32_t>(sub);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> sreq;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            sreq.emplace(std::move(*rh));
        }
    }
    RUVIA_CHECK(server.submit_interim_response_head(
                    sid, ruvia::http_interim_response_head(ruvia::http_status::early_hints)) ==
                ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    bool saw_1xx = false;
    while (auto ev = client.next_event()) {
        if (ev->informational_head()) {
            saw_1xx = true;
        }
    }
    RUVIA_CHECK(saw_1xx);

    std::pmr::string rst(&resource);
    constexpr std::array<char, 4> refused_payload{0, 0, 0, 7};
    append_frame(rst, ruvia::http2_frame_type::rst_stream, 0, sid,
        std::string_view(refused_payload.data(), refused_payload.size()));
    RUVIA_CHECK(client.feed(rst) == ruvia::http2_feed_result::accepted);
    bool saw_unprocessed = false;
    bool saw_closed = false;
    while (auto ev = client.next_event()) {
        if (ev->request_unprocessed()) {
            saw_unprocessed = true;
        }
        if (const auto* closed = ev->stream_closed()) {
            saw_closed = closed->error() == ruvia::http2_error_code::refused_stream;
        }
    }
    RUVIA_CHECK(!saw_unprocessed);
    RUVIA_CHECK(saw_closed);
    if (sreq) {
        (void)server.release(std::move(*sreq));
    }
}

// CONNECT 成功时保留但忽略长度；拒绝响应仍按普通 HTTP 正文分帧处理。
RUVIA_TEST(http2_public_client_connect_response_ignores_success_length_and_validates_rejection) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto sub1 = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    const auto sid1 = std::get<std::uint32_t>(sub1);
    const auto sub2 = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    const auto sid2 = std::get<std::uint32_t>(sub2);
    (void)ruvia::testing::drain_output(client);

    // Valid Content-Length on 200 CONNECT is retained in response_head.headers()
    std::pmr::string rblock1(&resource);
    ruvia::hpack_encoder::encode_header(rblock1, ":status", "200");
    ruvia::hpack_encoder::encode_header(rblock1, "content-length", "5");
    std::pmr::string rwire1(&resource);
    append_frame(rwire1, ruvia::http2_frame_type::headers, 0x4, sid1, rblock1);
    RUVIA_CHECK(client.feed(rwire1) == ruvia::http2_feed_result::accepted);
    bool has_cl = false;
    while (auto ev = client.next_event()) {
        if (auto* rh = ev->response_head()) {
            for (const auto& h : rh->head().headers()) {
                if (h.name() == "content-length" && h.value() == "5") {
                    has_cl = true;
                }
            }
        }
    }
    RUVIA_CHECK(has_cl);
    RUVIA_CHECK(!client.stream_aborted(sid1));

    // 非 2xx CONNECT 的非法 Content-Length 仍触发 PROTOCOL_ERROR。
    std::pmr::string rblock2(&resource);
    ruvia::hpack_encoder::encode_header(rblock2, ":status", "400");
    ruvia::hpack_encoder::encode_header(rblock2, "content-length", "not-a-number");
    std::pmr::string rwire2(&resource);
    append_frame(rwire2, ruvia::http2_frame_type::headers, 0x4, sid2, rblock2);
    RUVIA_CHECK(client.feed(rwire2) == ruvia::http2_feed_result::accepted);
    while (client.next_event()) {
    }
    RUVIA_CHECK(client.stream_aborted(sid2));
}

RUVIA_TEST(http2_public_asterisk_form_request_never_generates_authority) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    (void)ruvia::testing::drain_output(client);
    const auto rejected = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "OPTIONS", .authority_ = "example.test", .target_ = "*"});
    RUVIA_CHECK(std::holds_alternative<ruvia::http2_request_head_submit_error>(rejected) &&
                std::get<ruvia::http2_request_head_submit_error>(rejected) ==
                    ruvia::http2_request_head_submit_error::invalid_message);
    RUVIA_CHECK(!client.wants_write());
    const auto accepted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.method_ = "OPTIONS", .target_ = "*"});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(accepted));
}

RUVIA_TEST(http2_public_built_request_exposes_extended_protocol) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    auto server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const std::array websocket_headers{ruvia::http_header_view{"sec-websocket-version", "13"}};
    for (const std::string_view protocol : {"websocket", "connect-udp"}) {
        const ruvia::http2_extended_connect_request_head_view head{
            .protocol_ = protocol,
            .authority_ = "example.test",
            .target_ = "/proxy",
            .headers_ = protocol == "websocket"
                            ? std::span<const ruvia::http_header_view>(websocket_headers)
                            : std::span<const ruvia::http_header_view>{}};
        const auto submitted = client.submit_request_head(head);
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        relay.transfer(client, server);
        std::optional<ruvia::http2_request_head_event> request_head_event;
        while (auto event_value = server.next_event()) {
            if (auto* request_head = event_value->request_head()) {
                request_head_event.emplace(std::move(*request_head));
            }
        }
        RUVIA_CHECK(request_head_event.has_value());
        if (!request_head_event) {
            return;
        }
        const auto request = request_head_event->build_request(&resource);
        RUVIA_CHECK_EQ(request.protocol(), protocol);
        // RFC 8441/9298: :protocol selects the tunnel protocol, so a WebSocket
        // CONNECT must not validate as CONNECT-UDP.
        RUVIA_CHECK_EQ(ruvia::validate_http_connect_udp_request(request).has_value(),
            protocol == "websocket");
        (void)server.release(std::move(*request_head_event));
    }
}

RUVIA_TEST(http2_public_late_responses_after_many_client_cancellations_keep_connection) {
    std::pmr::unsynchronized_pool_resource resource;
    auto client = ruvia::http2_connection::client({.resource_ = &resource});
    std::pmr::string settings(&resource);
    append_peer_settings(settings);
    RUVIA_CHECK(client.feed(settings) == ruvia::http2_feed_result::accepted);
    (void)ruvia::testing::drain_output(client);

    std::uint32_t first_canceled_stream = 0;
    for (std::size_t index = 0; index != 2100; ++index) {
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/canceled"});
        RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        if (index == 0) {
            first_canceled_stream = stream_id;
        }
        RUVIA_CHECK(client.submit_reset(stream_id, ruvia::http2_error_code::cancel) ==
                    ruvia::http2_submit_status::accepted);
        (void)ruvia::testing::drain_output(client);
    }

    const auto active = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/active"});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(active));
    if (!std::holds_alternative<std::uint32_t>(active)) {
        return;
    }
    const auto active_stream = std::get<std::uint32_t>(active);
    (void)ruvia::testing::drain_output(client);

    std::pmr::string response_block(&resource);
    ruvia::hpack_encoder::encode_header(response_block, ":status", "200");
    std::pmr::string late_head(&resource);
    append_frame(
        late_head, ruvia::http2_frame_type::headers, 0x4, first_canceled_stream, response_block);
    const auto late_head_result = client.feed(late_head);
    RUVIA_CHECK(late_head_result == ruvia::http2_feed_result::accepted);
    if (late_head_result != ruvia::http2_feed_result::accepted) {
        return;
    }
    RUVIA_CHECK(!client.next_event());

    std::pmr::string late_data(&resource);
    append_frame(late_data, ruvia::http2_frame_type::data, 0x1, first_canceled_stream, "late");
    RUVIA_CHECK(client.feed(late_data) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK(!client.next_event());
    RUVIA_CHECK(!client.connection_error());

    std::pmr::string active_response(&resource);
    append_frame(active_response, ruvia::http2_frame_type::headers, 0x5, active_stream, response_block);
    RUVIA_CHECK(client.feed(active_response) == ruvia::http2_feed_result::accepted);
    const auto head = client.next_event();
    RUVIA_CHECK(head && head->response_head() && head->response_head()->stream_id() == active_stream);
    const auto end = client.next_event();
    RUVIA_CHECK(end && end->message_end() && end->message_end()->stream_id() == active_stream);
    RUVIA_CHECK(!client.connection_error());
}

// A promised stream refused while decoding its PUSH_PROMISE is already reset, so
// submit_reset(PROTOCOL_ERROR) does not emit a duplicate RST_STREAM.
RUVIA_TEST(http2_public_rejected_push_promise_does_not_emit_duplicate_late_rst_stream) {
    std::pmr::unsynchronized_pool_resource resource;
    ruvia::testing::output_relay relay;
    auto client = ruvia::http2_connection::client({.resource_ = &resource, .enable_push_ = true});
    auto push_server = ruvia::http2_connection::server({.resource_ = &resource});
    relay.transfer(client, push_server);
    relay.transfer(push_server, client);
    relay.transfer(client, push_server);
    const auto sub = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    RUVIA_CHECK(std::holds_alternative<std::uint32_t>(sub));
    (void)ruvia::testing::drain_output(client);

    // An unsafe method (POST) cannot be pushed (RFC 9113 §8.4).
    std::pmr::string post_push(&resource);
    ruvia::hpack_encoder::encode_header(post_push, ":method", "POST");
    ruvia::hpack_encoder::encode_header(post_push, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(post_push, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(post_push, ":path", "/upload");
    std::string post_payload("\x00\x00\x00\x02", 4);
    post_payload.append(post_push);
    std::pmr::string post_wire(&resource);
    append_frame(post_wire, ruvia::http2_frame_type::push_promise, 0x4, 1, post_payload);
    RUVIA_CHECK(client.feed(post_wire) == ruvia::http2_feed_result::accepted);
    RUVIA_CHECK_EQ(ruvia::testing::drain_output(client).size(), ruvia::http2_frame_header_bytes + 4);
    RUVIA_CHECK(client.submit_reset(2, ruvia::http2_error_code::protocol_error) ==
                ruvia::http2_submit_status::closed);
    RUVIA_CHECK(ruvia::testing::drain_output(client).empty());
}

// RFC 9110 §9.3.6：成功 CONNECT 的长度字段仅保留为元数据，不能阻止进入隧道。
RUVIA_TEST(http2_public_successful_connect_ignores_content_length_semantics) {
    for (const bool extended : {false, true}) {
        for (const std::string_view length : {"", "0", "42", "not-a-number", "18446744073709551616", "1, 2"}) {
            std::pmr::unsynchronized_pool_resource resource;
            auto client = ruvia::http2_connection::client({.resource_ = &resource});
            (void)ruvia::testing::drain_output(client);
            constexpr std::array<char, 6> enable_connect{0, 8, 0, 0, 0, 1};
            std::pmr::string settings(&resource);
            append_frame(settings, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view(enable_connect.data(), enable_connect.size()));
            RUVIA_CHECK(client.feed(settings) == ruvia::http2_feed_result::accepted);

            const auto submitted = extended
                                       ? client.submit_request_head(ruvia::http2_extended_connect_request_head_view{
                                             .protocol_ = "example", .authority_ = "example.test", .target_ = "/tunnel"})
                                       : client.submit_request_head(ruvia::http2_connect_request_head_view{
                                             .authority_ = "example.test:443"});
            RUVIA_CHECK(std::holds_alternative<std::uint32_t>(submitted));
            if (!std::holds_alternative<std::uint32_t>(submitted)) {
                continue;
            }
            const auto stream_id = std::get<std::uint32_t>(submitted);
            (void)ruvia::testing::drain_output(client);

            std::pmr::string block(&resource);
            ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
            ruvia::hpack_encoder::encode_header(block, "content-length", length);
            std::pmr::string wire(&resource);
            append_frame(wire, ruvia::http2_frame_type::headers, 0x4, stream_id, block);
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, stream_id, "tunnel bytes");
            RUVIA_CHECK(client.feed(wire) == ruvia::http2_feed_result::accepted);
            bool received_head = false;
            bool received_data = false;
            bool received_end = false;
            while (auto event = client.next_event()) {
                RUVIA_CHECK(event->stream_closed() == nullptr);
                if (const auto* head = event->response_head()) {
                    received_head = true;
                    const auto headers = head->head().headers();
                    RUVIA_CHECK_EQ(headers.size(), std::size_t{1});
                    if (!headers.empty()) {
                        RUVIA_CHECK_EQ(headers.front().name(), std::string_view("content-length"));
                        RUVIA_CHECK_EQ(headers.front().value(), length);
                    }
                } else if (const auto* data = event->tunnel_data()) {
                    received_data = true;
                    RUVIA_CHECK_EQ(data->bytes(), std::string_view("tunnel bytes"));
                } else if (event->tunnel_end() != nullptr) {
                    received_end = true;
                }
            }
            RUVIA_CHECK(received_head);
            RUVIA_CHECK(received_data);
            RUVIA_CHECK(received_end);
            RUVIA_CHECK(client.submit_reset(stream_id, ruvia::http2_error_code::cancel) ==
                        ruvia::http2_submit_status::accepted);
        }
    }
}

namespace {

template <typename headers_type>
concept push_request_view_accepts_headers = requires(headers_type&& headers) {
    ruvia::http_push_request_view{.headers_ = std::forward<headers_type>(headers)};
};

template <typename headers_type>
concept regular_request_view_accepts_headers = requires(headers_type&& headers) {
    ruvia::http2_regular_request_head_view{.headers_ = std::forward<headers_type>(headers)};
};

static_assert(push_request_view_accepts_headers<const std::vector<ruvia::http_header_view>&>);
static_assert(!push_request_view_accepts_headers<std::vector<ruvia::http_header_view>>);
static_assert(!push_request_view_accepts_headers<std::array<ruvia::http_header_view, 1>>);
static_assert(regular_request_view_accepts_headers<const std::vector<ruvia::http_header_view>&>);
static_assert(!regular_request_view_accepts_headers<std::vector<ruvia::http_header_view>>);
static_assert(!regular_request_view_accepts_headers<std::array<ruvia::http_header_view, 1>>);

}  // namespace
