#include "ruvia/http/http2_connection.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
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

#include "ruvia/http/http_client.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

#include "http_server_fixture.h"

namespace {

template <typename head_type, typename body_type>
constexpr bool can_build_http2_request = requires(head_type&& head, body_type&& body) {
    std::forward<head_type>(head).build_request(std::forward<body_type>(body));
};

static_assert(can_build_http2_request<ruvia::http2_request_head_event&, std::string&>);
static_assert(can_build_http2_request<const ruvia::http2_request_head_event&, const std::string&>);
static_assert(can_build_http2_request<ruvia::http2_request_head_event&, std::string_view>);
static_assert(can_build_http2_request<ruvia::http2_request_head_event&, const char (&)[5]>);
static_assert(!can_build_http2_request<ruvia::http2_request_head_event&, std::string>);
static_assert(!can_build_http2_request<ruvia::http2_request_head_event&, const std::string>);
static_assert(!can_build_http2_request<ruvia::http2_request_head_event, std::string_view>);
static_assert(!can_build_http2_request<const ruvia::http2_request_head_event, std::string_view>);

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload_value) {
    std::array<char, ruvia::http2_frame_header_bytes> header_value{};
    if (!ruvia::encode_http2_frame_header(
            header_value, static_cast<std::uint32_t>(payload_value.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header_value.data(), header_value.size());
    wire.append(payload_value);
}

void append_header_section(std::string& wire, std::uint32_t stream_id,
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

void append_peer_settings(std::string& wire) {
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
}

void append_response(std::string& wire,
    std::uint32_t stream_id, std::string_view status, std::string_view body = {},
    std::string_view trailer_name = {}, std::string_view trailer_value = {}) {
    std::string block;
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

std::string client_response_wire(std::string_view body,
    bool include_header = false, bool end_stream = true) {
    std::string block;
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
    if (include_header) {
        ruvia::hpack_encoder::encode_header(block, "x-test", "value");
    }

    std::string wire;
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    append_frame(wire, ruvia::http2_frame_type::data, end_stream ? 0x1 : 0, 1, body);
    return wire;
}

std::string client_response_with_trailers_wire() {
    std::string wire;
    append_peer_settings(wire);
    std::string block;
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
    ruvia::hpack_encoder::encode_header(block, "x-test", "owned");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    block.clear();
    ruvia::hpack_encoder::encode_header(block, "x-trace", "done");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    return wire;
}

std::string server_request_wire(std::string_view body,
    std::string_view method = "POST", std::string_view expectation = {},
    bool end_stream = true) {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", method);
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", body.empty() ? "0" : "1");
    if (!expectation.empty()) {
        ruvia::hpack_encoder::encode_header(block, "expect", expectation);
    }

    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    const auto head_flags = static_cast<std::uint8_t>(0x4 | (end_stream && body.empty() ? 0x1 : 0));
    append_frame(wire, ruvia::http2_frame_type::headers, head_flags, 1, block);
    if (!body.empty()) {
        append_frame(wire, ruvia::http2_frame_type::data,
            end_stream ? 0x1 : 0, 1, body);
    }
    return wire;
}

std::string server_request_with_trailers_wire() {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", "1");

    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    append_frame(wire, ruvia::http2_frame_type::data, 0, 1, "x");
    block.clear();
    ruvia::hpack_encoder::encode_header(block, "x-request-trace", "done");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    return wire;
}

ruvia::http2_connection prepared_client() {
    auto client = ruvia::http2_connection::client({});
    (void)ruvia::test::drain_output(client);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
    if (!std::holds_alternative<std::uint32_t>(submitted)) {
        throw std::logic_error("test request was not submitted");
    }
    (void)ruvia::test::drain_output(client);
    return client;
}

ruvia::http2_connection prepared_client_method(std::string_view method) {
    auto client = ruvia::http2_connection::client({});
    (void)ruvia::test::drain_output(client);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = method, .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
    if (!std::holds_alternative<std::uint32_t>(submitted)) {
        throw std::logic_error("test request was not submitted");
    }
    (void)ruvia::test::drain_output(client);
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

TEST(Ruvia, http2_public_request_submission_exposes_one_exclusive_success_or_failure_contract) {
    auto client = ruvia::http2_connection::client();
    const auto invalid = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "", .authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_request_head_submit_error>(invalid));
    if (const auto* failure = std::get_if<ruvia::http2_request_head_submit_error>(&invalid)) {
        EXPECT_TRUE(*failure == ruvia::http2_request_head_submit_error::invalid_message);
    }
    const auto extended = client.submit_request_head(ruvia::http2_extended_connect_request_head_view{
        .protocol_ = "websocket", .authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_request_head_submit_error>(extended));
    if (const auto* failure = std::get_if<ruvia::http2_request_head_submit_error>(&extended)) {
        EXPECT_TRUE(*failure == ruvia::http2_request_head_submit_error::peer_capability_unavailable);
    }
    const auto regular = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(regular));
    if (const auto* submitted = std::get_if<std::uint32_t>(&regular)) {
        EXPECT_EQ(*submitted, std::uint32_t{1});
    }
    const auto connect = client.submit_request_head(ruvia::http2_connect_request_head_view{
        .authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect));
    if (const auto* submitted = std::get_if<std::uint32_t>(&connect)) {
        EXPECT_EQ(*submitted, std::uint32_t{3});
    }
    auto server = ruvia::http2_connection::server();
    const auto wrong_role = server.submit_request_head(ruvia::http2_regular_request_head_view{
        .authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_request_head_submit_error>(wrong_role));
    if (const auto* failure = std::get_if<ruvia::http2_request_head_submit_error>(&wrong_role)) {
        EXPECT_TRUE(*failure == ruvia::http2_request_head_submit_error::invalid_state);
    }
}

TEST(Ruvia, http2_public_escaped_events_remain_usable_after_move_assignment) {
    auto wire = server_request_wire("x");
    std::optional<ruvia::http2_event> request;
    std::optional<ruvia::http2_received_data_credit> credit;
    {
        auto connection = ruvia::http2_connection::server({});
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        request.emplace(std::move(*connection.next_event()));
        auto body = connection.next_event();
        EXPECT_TRUE(body && body->message_body_chunk() != nullptr);
        credit.emplace(body->message_body_chunk()->take_credit());
        connection = ruvia::http2_connection::server({});
        EXPECT_TRUE(request->request_head()->request_view().method_ == "POST");
    }
    request.reset();
    credit.reset();
}

TEST(Ruvia, http2_public_dropped_data_credit_returns_debt_and_releases_closed_stream) {
    auto client = prepared_client();
    const auto wire = client_response_wire("x");
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);

    (void)client.next_event();
    {
        auto chunk = client.next_event();
        auto* body = chunk ? chunk->message_body_chunk() : nullptr;
        EXPECT_TRUE(body != nullptr);
        auto credit = body->take_credit();
        const auto end = client.next_event();
        EXPECT_TRUE(end && end->message_end() != nullptr);
        EXPECT_TRUE(credit.valid());
    }

    EXPECT_TRUE(
        client.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::closed);
}

TEST(Ruvia, http2_public_client_reset_preserves_outstanding_data_credit) {
    auto client = prepared_client();
    const auto wire = client_response_wire("x", false, false);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);

    (void)client.next_event();
    auto chunk = client.next_event();
    auto* body = chunk ? chunk->message_body_chunk() : nullptr;
    EXPECT_TRUE(body != nullptr);
    auto credit = body->take_credit();

    EXPECT_TRUE(client.submit_reset(1, ruvia::http2_error_code::cancel) ==
                ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(credit.valid());
    (void)ruvia::http2_received_data_credit(std::move(credit));
    EXPECT_TRUE(!credit.valid());
    EXPECT_TRUE(!client.connection_error());
}

TEST(Ruvia, http2_public_peer_reset_preserves_outstanding_data_credit) {
    auto client = prepared_client();
    auto wire = client_response_wire("x", false, false);
    constexpr std::array<char, 4> cancel_payload{0, 0, 0, 8};
    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1,
        std::string_view(cancel_payload.data(), cancel_payload.size()));
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);

    (void)client.next_event();
    auto chunk = client.next_event();
    auto* body = chunk ? chunk->message_body_chunk() : nullptr;
    EXPECT_TRUE(body != nullptr);
    auto credit = body->take_credit();
    const auto closed = client.next_event();
    EXPECT_TRUE(closed && closed->stream_closed() != nullptr);

    EXPECT_TRUE(credit.valid());
    (void)ruvia::http2_received_data_credit(std::move(credit));
    EXPECT_TRUE(!client.connection_error());
}

// RFC 9113 section 8.7: REFUSED_STREAM means the server did not process the
// request, so it is published as unprocessed (retryable), not as a reset stream,
// and the connection stays usable. After a final response head it stays a reset.
TEST(Ruvia, http2_public_client_refused_stream_reports_request_unprocessed) {
    auto client = prepared_client();
    std::string wire;
    append_peer_settings(wire);
    constexpr std::array<char, 4> refused_payload{0, 0, 0, 7};
    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1,
        std::string_view(refused_payload.data(), refused_payload.size()));
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    bool unprocessed = false;
    while (auto event = client.next_event()) {
        EXPECT_TRUE(event->stream_closed() == nullptr);
        if (const auto* value = event->request_unprocessed()) {
            unprocessed = value->stream_id() == 1;
        }
    }
    EXPECT_TRUE(unprocessed);
    EXPECT_TRUE(!client.connection_error().has_value());
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"})));

    auto started = prepared_client();
    auto started_wire = client_response_wire("", false, false);
    append_frame(started_wire, ruvia::http2_frame_type::rst_stream, 0, 1,
        std::string_view(refused_payload.data(), refused_payload.size()));
    EXPECT_TRUE(started.feed(started_wire) == ruvia::http2_feed_result::accepted);
    bool reset = false;
    while (auto event = started.next_event()) {
        EXPECT_TRUE(event->request_unprocessed() == nullptr);
        if (const auto* closed = event->stream_closed()) {
            reset = closed->error() == ruvia::http2_error_code::refused_stream;
        }
    }
    EXPECT_TRUE(reset);
}

TEST(Ruvia, http2_public_server_request_view_hides_stream_storage) {
    auto server = ruvia::http2_connection::server({});
    EXPECT_TRUE(!server.header_block_in_progress());
    (void)ruvia::test::drain_output(server);
    const auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto head_event = server.next_event();
    EXPECT_TRUE(head_event && head_event->request_head() != nullptr);
    const auto request_view = head_event->request_head()->request_view();
    EXPECT_TRUE(request_view.method_ == "POST");
    EXPECT_TRUE(request_view.path_ == "/upload");
    const auto window = server.send_window_state(1);
    EXPECT_TRUE(window.has_value());
    EXPECT_TRUE(window->available_ == 65535);
    EXPECT_TRUE(!server.stream_aborted(1));
    EXPECT_TRUE(server.stream_aborted(3));

    auto request = head_event->request_head()->build_request({});
    EXPECT_TRUE(request.method() == "POST");
    const auto handshake = ruvia::validate_http2_websocket_handshake(server, 1, request);
    EXPECT_TRUE(handshake.accepted() == nullptr);
}

// RFC 9113 section 8.2.3: cookie crumbs are joined with "; ". A crumb with an
// empty value is not a cookie-pair and must not leave a leading or trailing
// separator in the joined field value.
TEST(Ruvia, http2_public_server_request_joins_cookie_crumbs_without_empty_members) {
    for (const auto crumbs : {std::array<std::string_view, 3>{"", "a=b", "c=d"}, std::array<std::string_view, 3>{"a=b", "", "c=d"},
             std::array<std::string_view, 3>{"a=b", "c=d", ""}}) {
        auto server = ruvia::http2_connection::server({});
        (void)ruvia::test::drain_output(server);
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/");
        for (const auto crumb : crumbs) {
            ruvia::hpack_encoder::encode_header(block, "cookie", crumb);
        }
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head_event = server.next_event();
        EXPECT_TRUE(head_event && head_event->request_head() != nullptr);
        auto request = head_event->request_head()->build_request({});
        EXPECT_TRUE(request.header("cookie") == std::string_view("a=b; c=d"));
    }
}

TEST(Ruvia, http2_public_server_release_preserves_outstanding_data_credit) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    const auto wire = server_request_wire("x");
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    auto chunk = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    auto* body = chunk ? chunk->message_body_chunk() : nullptr;
    EXPECT_TRUE(request_head != nullptr);
    EXPECT_TRUE(body != nullptr && body->bytes() == "x");
    EXPECT_TRUE(end && end->message_end() != nullptr);
    auto credit = body->take_credit();

    ruvia::http_response response;
    EXPECT_TRUE(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
    // The outstanding credit keeps the finished stream until it is returned.
    EXPECT_TRUE(server.stream_receive_status(1) == ruvia::http2_stream_receive_status::ended);
    (void)ruvia::http2_received_data_credit(std::move(credit));
    EXPECT_TRUE(server.stream_receive_status(1) == ruvia::http2_stream_receive_status::closed);
}

TEST(Ruvia, http2_public_send_window_observation_ends_with_protocol_closure) {
    auto server = ruvia::http2_connection::server({});
    const auto wire = server_request_wire({}, "GET");
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    auto* head = request ? request->request_head() : nullptr;
    EXPECT_TRUE(head != nullptr);
    if (head == nullptr) {
        return;
    }
    const auto end = server.next_event();
    EXPECT_TRUE(end && end->message_end());
    EXPECT_TRUE(!server.next_event());
    EXPECT_TRUE(server.send_window_state(1).has_value());

    ruvia::http_response response;
    EXPECT_TRUE(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(head->request_view().method_ == "GET");
    EXPECT_TRUE(!server.send_window_state(1));
    EXPECT_EQ(server.pending_data_output_bytes(1), std::size_t{0});
    EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    EXPECT_TRUE(!server.send_window_state(1));
}

// RFC 9113 section 5.1 (closed): WINDOW_UPDATE frames received on a closed
// stream are ignored, including a zero increment, both while the stream is
// still retained by its owner and after it has been released.
TEST(Ruvia, http2_public_window_update_on_closed_stream_is_ignored) {
    for (const bool released : {false, true}) {
        auto server = ruvia::http2_connection::server({});
        (void)ruvia::test::drain_output(server);
        const auto request_wire = server_request_wire({}, "GET");
        EXPECT_TRUE(server.feed(request_wire) == ruvia::http2_feed_result::accepted);
        auto request = server.next_event();
        auto* head = request ? request->request_head() : nullptr;
        EXPECT_TRUE(head != nullptr);
        if (head == nullptr) {
            return;
        }
        (void)server.next_event();
        ruvia::http_response response;
        EXPECT_TRUE(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
        if (released) {
            EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
        }
        (void)ruvia::test::drain_output(server);

        std::string wire;
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, std::string_view("\x00\x00\x00\x00", 4));
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, std::string_view("\x00\x00\x01\x00", 4));
        // A later request proves the connection stayed usable.
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/next");
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 3, block);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        EXPECT_TRUE(!server.connection_error().has_value());
        EXPECT_TRUE(!server.wants_write());
        auto next = server.next_event();
        EXPECT_TRUE(next && next->request_head() && next->request_head()->stream_id() == 3);
        if (!released) {
            EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
        }
        if (next && next->request_head()) {
            (void)server.next_event();
            EXPECT_TRUE(server.release(std::move(*next->request_head())) == ruvia::http2_server_request_release_status::released);
        }
    }
}

TEST(Ruvia, http2_public_send_window_observation_excludes_reset_streams_with_credit) {
    for (const bool peer_reset : {false, true}) {
        auto server = ruvia::http2_connection::server({});
        auto wire = server_request_wire("x", "POST", {}, false);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto request = server.next_event();
        auto* head = request ? request->request_head() : nullptr;
        auto chunk = server.next_event();
        auto* body = chunk ? chunk->message_body_chunk() : nullptr;
        EXPECT_TRUE(head != nullptr && body != nullptr);
        if (head == nullptr || body == nullptr) {
            continue;
        }
        auto credit = body->take_credit();
        chunk.reset();
        EXPECT_TRUE(!server.next_event());

        constexpr std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
        wire.clear();
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
            std::string_view(zero_window.data(), zero_window.size()));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        ruvia::http_response response;
        EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
            server.submit_streaming_response_head(1, std::move(response))));
        const auto blocked = server.submit_data(1, "blocked", ruvia::http2_end_stream::keep_open, 7);
        EXPECT_TRUE(blocked.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(blocked.consumed_bytes_, std::size_t{0});
        EXPECT_TRUE(server.send_window_state(1).has_value());
        EXPECT_EQ(server.pending_data_output_bytes(1), std::size_t{0});

        if (peer_reset) {
            constexpr std::array<char, 4> cancel{0, 0, 0, 8};
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1,
                std::string_view(cancel.data(), cancel.size()));
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            const auto closed = server.next_event();
            EXPECT_TRUE(closed && closed->stream_closed());
            EXPECT_TRUE(!server.next_event());
        } else {
            EXPECT_TRUE(server.submit_reset(1, ruvia::http2_error_code::cancel) ==
                        ruvia::http2_submit_status::accepted);
        }

        EXPECT_TRUE(credit.valid());
        EXPECT_TRUE(head->request_view().method_ == "POST");
        EXPECT_TRUE(!server.send_window_state(1));
        EXPECT_TRUE(server.stream_aborted(1));
        const auto retry = server.submit_data(1, "blocked", ruvia::http2_end_stream::keep_open, 7);
        EXPECT_TRUE(retry.status_ == ruvia::http2_data_submit_status::closed);
        EXPECT_EQ(retry.consumed_bytes_, std::size_t{0});
        EXPECT_EQ(server.pending_data_output_bytes(1), std::size_t{0});
        EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
        EXPECT_TRUE(!server.send_window_state(1));
        (void)ruvia::http2_received_data_credit(std::move(credit));
        EXPECT_TRUE(!server.send_window_state(1));
    }
}

TEST(Ruvia, http2_public_send_window_observation_keeps_unconsumed_half_closed_streams) {
    auto server = ruvia::http2_connection::server({});
    auto wire = server_request_wire({}, "GET");
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    auto* head = request ? request->request_head() : nullptr;
    EXPECT_TRUE(head != nullptr);
    if (head == nullptr) {
        return;
    }
    const auto end = server.next_event();
    EXPECT_TRUE(end && end->message_end());
    EXPECT_TRUE(!server.next_event());
    EXPECT_TRUE(server.stream_receive_status(1) == ruvia::http2_stream_receive_status::ended);

    constexpr std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
        std::string_view(zero_window.data(), zero_window.size()));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    ruvia::http_response response;
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
        server.submit_streaming_response_head(1, std::move(response))));
    (void)ruvia::test::drain_output(server);
    const auto blocked = server.submit_data(1, "body", ruvia::http2_end_stream::end_stream, 4);
    EXPECT_TRUE(blocked.status_ == ruvia::http2_data_submit_status::backpressured);
    EXPECT_EQ(blocked.consumed_bytes_, std::size_t{0});
    const auto window = server.send_window_state(1);
    EXPECT_TRUE(window.has_value());
    if (window) {
        EXPECT_EQ(window->stream_window_, 0);
        EXPECT_EQ(window->available_, std::uint32_t{0});
    }
    EXPECT_EQ(server.pending_data_output_bytes(1), std::size_t{0});

    constexpr std::array<char, 4> increment{0, 0, 0, 4};
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1,
        std::string_view(increment.data(), increment.size()));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.wants_write());
    EXPECT_TRUE(server.send_window_state(1)->available_ == 4);
    const auto retried = server.submit_data(1, "body", ruvia::http2_end_stream::end_stream, 4);
    EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete);
    EXPECT_EQ(retried.consumed_bytes_, std::size_t{4});
    EXPECT_TRUE(!server.send_window_state(1));
    EXPECT_EQ(server.pending_data_output_bytes(1), std::size_t{4});
    const auto output = ruvia::test::drain_output(server);
    const auto frame = ruvia::parse_http2_frame_header(output);
    EXPECT_TRUE(frame && frame->length_ == 4 && (frame->flags_ & 1U) != 0);
    EXPECT_EQ(output.size(), ruvia::http2_frame_header_bytes + 4);
    EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    EXPECT_TRUE(!server.send_window_state(1));
}

TEST(Ruvia, http2_public_server_submits_streaming_response_head_and_data) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    const auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    EXPECT_TRUE(request_head != nullptr);
    EXPECT_TRUE(end && end->message_end() != nullptr);
    (void)ruvia::test::drain_output(server);

    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
        server.submit_streaming_response_head(1, std::move(response))));

    const auto head_output = ruvia::test::drain_output(server);
    const auto head = ruvia::parse_http2_frame_header(
        std::span<const char>(head_output.data(), head_output.size()));
    EXPECT_TRUE(head.has_value());
    EXPECT_TRUE(head && head->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    EXPECT_TRUE(head && head->stream_id_ == 1);
    EXPECT_TRUE(head && (head->flags_ & 0x1U) == 0);

    EXPECT_TRUE(server.submit_data(1, "event: update\n\ndata: ok\n\n",
                          ruvia::http2_end_stream::end_stream, 65535)
                    .status_ ==
                ruvia::http2_data_submit_status::complete);
    const auto data_output = ruvia::test::drain_output(server);
    const auto data = ruvia::parse_http2_frame_header(
        std::span<const char>(data_output.data(), data_output.size()));
    EXPECT_TRUE(data.has_value());
    EXPECT_TRUE(data && data->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
    EXPECT_TRUE(data && (data->flags_ & 0x1U) != 0);

    EXPECT_TRUE(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_server_streaming_response_commit_plan_and_finish_are_public) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    const auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    EXPECT_TRUE(request_head != nullptr);
    EXPECT_TRUE(end && end->message_end() != nullptr);
    (void)ruvia::test::drain_output(server);

    ruvia::http_response response;
    const auto committed = server.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_trailer_intent::present);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
    if (const auto* plan = std::get_if<ruvia::http_response_stream_commit_plan>(&committed)) {
        EXPECT_TRUE(plan->framing() == ruvia::http_response_stream_framing::http2_frames);
        EXPECT_TRUE(plan->head_disposition() ==
                    ruvia::http_response_stream_head_disposition::body_open);
        EXPECT_TRUE(plan->trailers_available());
    }
    ruvia::http_response duplicate;
    const auto rejected = server.submit_streaming_response_head(1, std::move(duplicate),
        ruvia::http_response_trailer_intent::none);
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(rejected));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(rejected) &&
                std::get<ruvia::http2_response_head_submit_error>(rejected) ==
                    ruvia::http2_response_head_submit_error::invalid_state);
    (void)ruvia::test::drain_output(server);

    const std::array<ruvia::http_header_view, 1> fields_value{{{"x-final", "done"}}};
    const auto trailers = ruvia::validate_http_response_trailers(fields_value);
    EXPECT_TRUE(server.submit_data(1, "body", ruvia::http2_end_stream::keep_open, 65535).status_ ==
                ruvia::http2_data_submit_status::complete);
    EXPECT_TRUE(server.finish_response(1, trailers) ==
                ruvia::http2_finish_status::accepted);
    std::string frame_bytes;
    EXPECT_TRUE(server.take_output_batch(1, frame_bytes).status_ == ruvia::http2_output_batch_status::taken);
    const auto trailer_frame = ruvia::parse_http2_frame_header(
        std::span<const char>(frame_bytes.data(), frame_bytes.size()));
    EXPECT_TRUE(trailer_frame && trailer_frame->type_ ==
                                     static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
    frame_bytes.clear();
    EXPECT_TRUE(server.take_output_batch(1, frame_bytes).status_ == ruvia::http2_output_batch_status::taken);
    const auto terminal = ruvia::parse_http2_frame_header(
        std::span<const char>(frame_bytes.data(), frame_bytes.size()));
    EXPECT_TRUE(terminal && terminal->type_ ==
                                static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    EXPECT_TRUE(terminal && (terminal->flags_ & 0x1U) != 0);
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)server.finish_response(1, trailers); }));
    EXPECT_TRUE(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_retried_data_and_trailers_keep_hpack_state_across_window_reopen) {
    for (const auto value_size : {ruvia::http2_default_max_frame_size - 12,
             ruvia::http2_default_max_frame_size - 11}) {
        {
            auto server = ruvia::http2_connection::server({});
            (void)ruvia::test::drain_output(server);
            const auto wire = server_request_wire({}, "GET");
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            const auto end = server.next_event();
            auto* request_head = request ? request->request_head() : nullptr;
            EXPECT_TRUE(request_head != nullptr);
            EXPECT_TRUE(end && end->message_end() != nullptr);
            (void)ruvia::test::drain_output(server);

            constexpr std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
            std::string window_settings;
            append_frame(window_settings, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view(zero_window.data(), zero_window.size()));
            EXPECT_TRUE(server.feed(window_settings) == ruvia::http2_feed_result::accepted);
            EXPECT_TRUE(!server.next_event());
            (void)ruvia::test::drain_output(server);
            ruvia::http_response response;
            response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
            const auto committed = server.submit_streaming_response_head(1, std::move(response),
                ruvia::http_response_trailer_intent::present);
            EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
            (void)ruvia::test::drain_output(server);
            const auto blocked = server.submit_data(1, "z", ruvia::http2_end_stream::keep_open, 1);
            EXPECT_TRUE(blocked.status_ == ruvia::http2_data_submit_status::backpressured);
            EXPECT_EQ(blocked.consumed_bytes_, std::size_t{0});
            std::string value(value_size, 't');
            const auto expected = value;
            const std::array fields{ruvia::http_header_view{"x-tail", value}};
            const auto trailers = ruvia::validate_http_response_trailers(fields);

            constexpr std::array<char, 6> zero_table{0, 1, 0, 0, 0, 0};
            std::string table_settings;
            append_frame(table_settings, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view(zero_table.data(), zero_table.size()));
            EXPECT_TRUE(server.feed(table_settings) == ruvia::http2_feed_result::accepted);
            EXPECT_TRUE(!server.next_event());
            (void)ruvia::test::drain_output(server);
            constexpr std::array<char, 4> increment{0, 0, 0, 1};
            std::string window_update;
            append_frame(window_update, ruvia::http2_frame_type::window_update, 0, 1,
                std::string_view(increment.data(), increment.size()));
            EXPECT_TRUE(server.feed(window_update) == ruvia::http2_feed_result::accepted);
            EXPECT_TRUE(!server.next_event());
            EXPECT_TRUE(!server.wants_write());
            const auto submitted = server.submit_data(1, "z", ruvia::http2_end_stream::keep_open, 1);
            EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::complete);
            EXPECT_EQ(submitted.consumed_bytes_, std::size_t{1});
            EXPECT_TRUE(server.finish_response(1, trailers) == ruvia::http2_finish_status::accepted);
            value.assign(value_size, 'x');
            const auto output = ruvia::test::drain_output(server);
            std::string body;
            std::string encoded;
            unsigned header_runs = 0;
            bool headers_complete = false;
            for (std::size_t offset = 0; offset < output.size();) {
                const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(output).subspan(offset));
                EXPECT_TRUE(frame.has_value());
                if (!frame) {
                    break;
                }
                const auto payload_offset = offset + ruvia::http2_frame_header_bytes;
                EXPECT_TRUE(frame->length_ <= output.size() - payload_offset);
                if (frame->length_ > output.size() - payload_offset) {
                    break;
                }
                const auto payload = output.substr(payload_offset, frame->length_);
                EXPECT_EQ(frame->stream_id_, std::uint32_t{1});
                if (frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    EXPECT_EQ(header_runs, 0u);
                    EXPECT_TRUE((frame->flags_ & 1) == 0);
                    body.append(payload);
                } else {
                    EXPECT_TRUE(!headers_complete);
                    if (frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                        ++header_runs;
                        EXPECT_TRUE((frame->flags_ & 1) != 0);
                    } else {
                        EXPECT_TRUE(frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::continuation));
                        EXPECT_EQ(header_runs, 1u);
                    }
                    encoded.append(payload);
                    headers_complete = (frame->flags_ & 4) != 0;
                }
                offset = payload_offset + frame->length_;
            }
            EXPECT_EQ(body, "z");
            EXPECT_EQ(header_runs, 1u);
            EXPECT_TRUE(headers_complete);
            EXPECT_TRUE(encoded.starts_with('\x20'));
            ruvia::hpack_decoder decoder;
            decoder.set_max_dynamic_table_size(0);
            unsigned field_count = 0;
            const auto decoded = decoder.decode(encoded, [&](std::string_view name, std::string_view field_value) {
                ++field_count;
                EXPECT_EQ(name, "x-tail");
                EXPECT_EQ(field_value, expected);
                return true;
            });
            EXPECT_TRUE(!decoded.has_value());
            EXPECT_EQ(field_count, 1u);
            EXPECT_TRUE(!server.send_window_state(1));
            if (request_head != nullptr) {
                EXPECT_TRUE(server.release(std::move(*request_head)) ==
                            ruvia::http2_server_request_release_status::released);
            }
        }
    }
}

TEST(Ruvia, http2_public_streaming_head_response_ends_at_headers) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    const auto wire = server_request_wire({}, "HEAD");
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    EXPECT_TRUE(request_head != nullptr);
    EXPECT_TRUE(end && end->message_end() != nullptr);
    (void)ruvia::test::drain_output(server);

    ruvia::http_response response;
    const auto committed = server.submit_streaming_response_head(1, std::move(response),
        ruvia::http_response_trailer_intent::none);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed) &&
                std::get<ruvia::http_response_stream_commit_plan>(committed).head_disposition() ==
                    ruvia::http_response_stream_head_disposition::message_ended);
    const auto output = ruvia::test::drain_output(server);
    const auto frame = ruvia::parse_http2_frame_header(
        std::span<const char>(output.data(), output.size()));
    EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    EXPECT_TRUE(frame && (frame->flags_ & 0x1U) != 0);
    EXPECT_TRUE(ruvia::test::throws_on(
        [&] { (void)server.submit_data(1, "", ruvia::http2_end_stream::end_stream, 65535); }));
    EXPECT_TRUE(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_transformed_coding_ows_preserves_response_framing) {
    // A complete gzip representation of an empty payload; the coded body still
    // has twenty bytes that must be framed independently of its decoded size.
    constexpr std::string_view encoded("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00", 20);
    for (const bool replace_body : {false, true}) {
        for (const std::string_view coding : {" gzip", "gzip ", "\tgzip\t", " \tgzip\t "}) {
            auto client = prepared_client();
            auto server = ruvia::http2_connection::server({});
            const auto wire = server_request_wire({}, "GET");
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            EXPECT_TRUE(request && request->request_head());
            const auto request_end = server.next_event();
            EXPECT_TRUE(request_end && request_end->message_end());
            ruvia::http_response response;
            response.body(replace_body ? "old representation" : encoded);
            response.header("ETag", "\"v1\"");
            response.header("Content-Length", "999");
            if (replace_body) {
                std::string replacement(encoded);
                response.replace_body_with_content_encoding(std::move(replacement), coding);
            } else {
                response.apply_content_encoding(coding);
            }
            EXPECT_TRUE(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
            const auto response_wire = ruvia::test::drain_output(server);
            EXPECT_TRUE(client.feed(response_wire) == ruvia::http2_feed_result::accepted);
            const auto head = client.next_event();
            EXPECT_TRUE(head && head->response_head());
            if (head && head->response_head()) {
                const auto fields = head->response_head()->head().headers();
                const auto find_value = [&](std::string_view name) {
                    const auto found = std::find_if(fields.begin(), fields.end(),
                        [name](const auto& field) { return field.name() == name; });
                    return found == fields.end() ? std::string_view{} : found->value();
                };
                EXPECT_EQ(find_value("content-encoding"), "gzip");
                EXPECT_EQ(find_value("content-length"), "20");
                EXPECT_EQ(find_value("etag"), "W/\"v1\"");
            }
            auto body = client.next_event();
            auto* chunk = body ? body->message_body_chunk() : nullptr;
            EXPECT_TRUE(chunk != nullptr);
            if (chunk != nullptr) {
                EXPECT_EQ(chunk->bytes(), encoded);
                (void)chunk->take_credit();
            }
            const auto end = client.next_event();
            EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
            EXPECT_TRUE(!client.next_event());
            EXPECT_TRUE(!client.connection_error());
        }
    }
}

TEST(Ruvia, http2_public_empty_buffered_head_matches_get_content_length) {
    for (const std::string_view method : {std::string_view("GET"), std::string_view("HEAD")}) {
        auto client = prepared_client();
        auto server = ruvia::http2_connection::server({});
        const auto wire = server_request_wire({}, method);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto request = server.next_event();
        EXPECT_TRUE(request && request->request_head());
        const auto request_end = server.next_event();
        EXPECT_TRUE(request_end && request_end->message_end());
        ruvia::http_response response;
        response.body("");
        const auto plan = ruvia::plan_buffered_http_response_write(
            request->request_head()->build_request().known_method(), response);
        const auto submitted = server.submit_response_head(1, response, plan);
        EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
        (void)server.release(std::move(*request->request_head()));
        const auto response_wire = ruvia::test::drain_output(server);
        EXPECT_TRUE(client.feed(response_wire) == ruvia::http2_feed_result::accepted);
        const auto head = client.next_event();
        EXPECT_TRUE(head && head->response_head());
        if (head && head->response_head()) {
            const auto fields = head->response_head()->head().headers();
            const auto found = std::find_if(fields.begin(), fields.end(),
                [](const auto& field) { return field.name() == "content-length"; });
            EXPECT_TRUE(found != fields.end());
            if (found != fields.end()) {
                EXPECT_EQ(found->value(), "0");
            }
        }
        const auto end = client.next_event();
        EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
        EXPECT_TRUE(!client.next_event());
        EXPECT_TRUE(!client.connection_error());
    }
}

TEST(Ruvia, http2_public_server_submits_buffered_response_head_and_returns_write_plan) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    const auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    EXPECT_TRUE(request_head != nullptr);
    EXPECT_TRUE(end && end->message_end() != nullptr);
    (void)ruvia::test::drain_output(server);

    ruvia::http_response response;
    response.body("payload");
    const auto wrong_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::head, response);
    const auto mismatch = server.submit_response_head(1, response, wrong_plan);
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(mismatch));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(mismatch) &&
                std::get<ruvia::http2_response_head_submit_error>(mismatch) ==
                    ruvia::http2_response_head_submit_error::response_plan_mismatch);
    const auto write_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::post, response);
    const auto submitted = server.submit_response_head(1, response, write_plan);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
    if (const auto* plan = std::get_if<ruvia::http_buffered_response_write_plan>(&submitted)) {
        EXPECT_TRUE(plan->body_plan().response_status() == ruvia::http_status::ok);
        EXPECT_TRUE(plan->send_body());
        EXPECT_TRUE(plan->content_length() == 7);
    }
    const auto head_output = ruvia::test::drain_output(server);
    const auto head = ruvia::parse_http2_frame_header(
        std::span<const char>(head_output.data(), head_output.size()));
    EXPECT_TRUE(head && head->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    EXPECT_TRUE(head && (head->flags_ & 0x1U) == 0);
    EXPECT_TRUE(server.submit_data(1, "payload", ruvia::http2_end_stream::end_stream, 65535).status_ ==
                ruvia::http2_data_submit_status::complete);

    EXPECT_TRUE(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_response_head_submit_exposes_shared_failure_values) {
    auto closed = ruvia::http2_connection::server({});
    ruvia::http_response closed_response;
    const auto closed_buffered = closed.submit_response_head(1, closed_response,
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, closed_response));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_buffered));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_buffered) &&
                std::get<ruvia::http2_response_head_submit_error>(closed_buffered) ==
                    ruvia::http2_response_head_submit_error::closed);
    const auto closed_streaming = closed.submit_streaming_response_head(1,
        ruvia::http_response(),
        ruvia::http_response_trailer_intent::none);
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_streaming));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_streaming) &&
                std::get<ruvia::http2_response_head_submit_error>(closed_streaming) ==
                    ruvia::http2_response_head_submit_error::closed);

    auto client = prepared_client();
    ruvia::http_response client_response;
    const auto invalid_state = client.submit_streaming_response_head(1, std::move(client_response),
        ruvia::http_response_trailer_intent::none);
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_state));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_state) &&
                std::get<ruvia::http2_response_head_submit_error>(invalid_state) ==
                    ruvia::http2_response_head_submit_error::invalid_state);

    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    const auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    const auto end = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    EXPECT_TRUE(request_head != nullptr);
    EXPECT_TRUE(end && end->message_end() != nullptr);
    (void)ruvia::test::drain_output(server);

    ruvia::http_response invalid_response;
    invalid_response.header("Content-Length", "invalid");
    const auto invalid_message = server.submit_streaming_response_head(1,
        std::move(invalid_response),
        ruvia::http_response_trailer_intent::none);
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_message));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_message) &&
                std::get<ruvia::http2_response_head_submit_error>(invalid_message) ==
                    ruvia::http2_response_head_submit_error::invalid_message);
    EXPECT_TRUE(!server.wants_write());
    EXPECT_TRUE(server.release(std::move(*request_head)) ==
                ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_response_head_validates_stable_representation_metadata) {
    for (const auto invalid : {ruvia::http_header_view{"Content-Type", "text plain"},
             ruvia::http_header_view{"Content-Encoding", "gzip;q=1"},
             ruvia::http_header_view{"Trailer", "Content-Length"}}) {
        for (const bool streaming : {false, true}) {
            auto server = ruvia::http2_connection::server({});
            (void)ruvia::test::drain_output(server);
            const auto wire = server_request_wire({});
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            const auto end = server.next_event();
            auto* request_head = request ? request->request_head() : nullptr;
            EXPECT_TRUE(request_head != nullptr);
            EXPECT_TRUE(end && end->message_end() != nullptr);
            (void)ruvia::test::drain_output(server);

            ruvia::http_response response;
            try {
                response.header_stable_view(invalid.name(), invalid.value());
            } catch (const std::invalid_argument&) {
                continue;
            }
            if (streaming) {
                const auto rejected = server.submit_streaming_response_head(1, std::move(response),
                    ruvia::http_response_trailer_intent::none);
                EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(rejected));
                if (const auto* err = std::get_if<ruvia::http2_response_head_submit_error>(&rejected)) {
                    EXPECT_TRUE(*err == ruvia::http2_response_head_submit_error::invalid_message);
                }
            } else {
                const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::post, response);
                const auto rejected = server.submit_response_head(1, response, plan);
                EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(rejected));
                if (const auto* err = std::get_if<ruvia::http2_response_head_submit_error>(&rejected)) {
                    EXPECT_TRUE(*err == ruvia::http2_response_head_submit_error::invalid_message);
                }
            }
            EXPECT_TRUE(!server.wants_write());
            if (request_head != nullptr) {
                EXPECT_TRUE(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
            }
        }
    }
}

// The handler's `chunked` Transfer-Encoding is the framework-owned framing hint
// on HTTP/2 as on HTTP/1: the writer omits it, so the peer receives a valid
// head instead of a connection-specific field (RFC 9113 section 8.2.2).
TEST(Ruvia, http2_public_response_head_omits_chunked_transfer_encoding_hint) {
    for (const auto status : {ruvia::http_status::ok, ruvia::http_status::no_content, ruvia::http_status::not_modified}) {
        for (const bool streaming : {false, true}) {
            auto client = prepared_client();
            auto server = ruvia::http2_connection::server({});
            const auto wire = server_request_wire({}, "GET");
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            const auto request_end = server.next_event();
            auto* request_head = request ? request->request_head() : nullptr;
            EXPECT_TRUE(request_head != nullptr);
            EXPECT_TRUE(request_end && request_end->message_end() != nullptr);

            ruvia::http_response response;
            response.status(status);
            response.header("Transfer-Encoding", "ChUnKeD");
            if (streaming) {
                const auto committed = server.submit_streaming_response_head(1, std::move(response),
                    ruvia::http_response_trailer_intent::none);
                const auto* plan = std::get_if<ruvia::http_response_stream_commit_plan>(&committed);
                EXPECT_TRUE(plan != nullptr);
                if (plan != nullptr &&
                    plan->head_disposition() == ruvia::http_response_stream_head_disposition::body_open) {
                    EXPECT_TRUE(server.submit_data(1, {}, ruvia::http2_end_stream::end_stream, 65535).status_ ==
                                ruvia::http2_data_submit_status::complete);
                }
            } else {
                const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
                EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
                    server.submit_response_head(1, response, plan)));
            }
            if (request_head != nullptr) {
                (void)server.release(std::move(*request_head));
            }

            const auto response_wire = ruvia::test::drain_output(server);
            EXPECT_TRUE(client.feed(response_wire) == ruvia::http2_feed_result::accepted);
            const auto head = client.next_event();
            EXPECT_TRUE(head && head->response_head());
            if (head && head->response_head()) {
                const auto fields = head->response_head()->head().headers();
                EXPECT_TRUE(std::none_of(fields.begin(), fields.end(),
                    [](const auto& field) { return field.name() == "transfer-encoding"; }));
            }
            const auto end = client.next_event();
            EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
            EXPECT_TRUE(!client.connection_error());
        }
    }
}

// Any other Transfer-Encoding claims a coding the writer never applies and
// stays a rejected connection-specific field, with nothing queued.
TEST(Ruvia, http2_public_response_head_rejects_transfer_encoding_other_than_chunked) {
    for (const std::string_view coding : {"gzip", "gzip, chunked", "identity", "", "chunked, chunked"}) {
        for (const bool streaming : {false, true}) {
            auto server = ruvia::http2_connection::server({});
            (void)ruvia::test::drain_output(server);
            const auto wire = server_request_wire({});
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            const auto end = server.next_event();
            auto* request_head = request ? request->request_head() : nullptr;
            EXPECT_TRUE(request_head != nullptr);
            EXPECT_TRUE(end && end->message_end() != nullptr);
            (void)ruvia::test::drain_output(server);

            ruvia::http_response response;
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
            EXPECT_TRUE(error == ruvia::http2_response_head_submit_error::invalid_message);
            EXPECT_TRUE(!server.wants_write());
            if (request_head != nullptr) {
                EXPECT_TRUE(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
            }
        }
    }
}

TEST(Ruvia, http2_public_dropped_request_preserves_outstanding_data_credit) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    const auto wire = server_request_wire("x");
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    auto chunk = server.next_event();
    auto* body = chunk ? chunk->message_body_chunk() : nullptr;
    EXPECT_TRUE(request && request->request_head() != nullptr);
    EXPECT_TRUE(body != nullptr);
    auto credit = body->take_credit();

    request.reset();
    EXPECT_TRUE(credit.valid());
    (void)ruvia::http2_received_data_credit(std::move(credit));
    EXPECT_TRUE(!server.connection_error());
}

TEST(Ruvia, http2_public_dropped_request_event_abandons_its_stream) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    const auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    (void)ruvia::test::drain_output(server);

    {
        const auto request = server.next_event();
        EXPECT_TRUE(request && request->request_head() != nullptr);
    }

    const auto output = ruvia::test::drain_output(server);
    const auto frame =
        ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
    EXPECT_TRUE(frame.has_value());
    EXPECT_TRUE(
        frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
}

TEST(Ruvia, http2_public_dropped_request_retries_failed_abandonment) {
    auto server = ruvia::http2_connection::server({});
    std::string initial_output;
    ruvia::test::drain_output(server, initial_output);
    const auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    EXPECT_TRUE(request && request->request_head() != nullptr);
    request.reset();
    // Getters never mutate; the writer's next output drain retries the release.
    std::string output;
    ruvia::test::drain_output(server, output);
    constexpr auto settings_ack_bytes = ruvia::http2_frame_header_bytes;
    EXPECT_TRUE(output.size() == settings_ack_bytes + ruvia::http2_frame_header_bytes + 4);
    const auto reset = ruvia::parse_http2_frame_header(std::span<const char>(
        output.data() + settings_ack_bytes, output.size() - settings_ack_bytes));
    EXPECT_TRUE(
        reset && reset->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
    EXPECT_TRUE(reset && reset->stream_id_ == 1);
}

TEST(Ruvia, http2_public_request_endpoint_survives_connection_destruction_without_aba) {
    auto escaped = [&]() {
        auto server = ruvia::http2_connection::server({});
        (void)ruvia::test::drain_output(server);
        const auto wire = server_request_wire({});
        (void)server.feed(wire);
        return server.next_event();
    }();
    EXPECT_TRUE(escaped && escaped->request_head() != nullptr);

    auto other = ruvia::http2_connection::server({});
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)other.release(std::move(*escaped->request_head())); }));
    escaped.reset();
}

TEST(Ruvia, http2_public_data_credit_endpoint_survives_connection_destruction_without_aba) {
    auto escaped_credit = [&]() {
        auto client = prepared_client();
        const auto wire = client_response_wire("x");
        (void)client.feed(wire);
        (void)client.next_event();
        auto chunk = client.next_event();
        return chunk->message_body_chunk()->take_credit();
    }();
    EXPECT_TRUE(escaped_credit.valid());

    // Destroying the credit returns it only to its own connection state, even
    // if a new connection reuses the destroyed connection's storage.
    auto other = prepared_client();
    (void)ruvia::test::drain_output(other);
    (void)ruvia::http2_received_data_credit(std::move(escaped_credit));
    EXPECT_TRUE(!other.wants_write());
    EXPECT_TRUE(!other.connection_error());
}

TEST(Ruvia, http2_public_request_views_survive_connection_move_assignment) {
    // Keep caller-owned input alive so this test isolates the connection's decoded storage.
    const auto wire = server_request_wire("x");
    std::optional<ruvia::http2_event> escaped_request;
    std::optional<ruvia::http2_event> escaped_chunk;

    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    EXPECT_TRUE(request && request->request_head() != nullptr);
    if (request) {
        escaped_request.emplace(std::move(*request));
    }
    auto chunk = server.next_event();
    EXPECT_TRUE(chunk && chunk->message_body_chunk() != nullptr);
    if (chunk) {
        escaped_chunk.emplace(std::move(*chunk));
    }

    // Move assignment destroys the old implementation while the public events still hold leases.
    auto replacement = ruvia::http2_connection::server({});
    server = std::move(replacement);

    const auto* request_head = escaped_request ? escaped_request->request_head() : nullptr;
    EXPECT_TRUE(request_head != nullptr);
    if (request_head != nullptr) {
        const std::string buffered_body = "x";
        const auto materialized = request_head->build_request(buffered_body);
        EXPECT_EQ(materialized.body(), buffered_body);
        EXPECT_EQ(materialized.body().data(), buffered_body.data());
        EXPECT_TRUE(materialized.method() == "POST");
        EXPECT_TRUE(materialized.target() == "/upload");
        EXPECT_TRUE(materialized.authority() == "example.test");
        const auto content_length = materialized.header("content-length");
        EXPECT_TRUE(content_length && *content_length == "1");
    }

    const auto* body = escaped_chunk ? escaped_chunk->message_body_chunk() : nullptr;
    EXPECT_TRUE(body != nullptr);
    if (body != nullptr) {
        EXPECT_TRUE(body->bytes() == "x");
    }
}

TEST(Ruvia, http2_public_stream_receive_status_is_a_read_only_snapshot) {
    auto client = prepared_client();
    EXPECT_TRUE(client.stream_receive_status(1) == ruvia::http2_stream_receive_status::open);
    EXPECT_TRUE(client.stream_receive_status(3) == ruvia::http2_stream_receive_status::closed);

    auto wire = client_response_wire({}, false, false);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(client.stream_receive_status(1) == ruvia::http2_stream_receive_status::open);
    while (client.next_event()) {
    }

    std::string end_frame;
    append_frame(end_frame, ruvia::http2_frame_type::data, 0x1, 1, {});
    EXPECT_TRUE(client.feed(end_frame) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(client.stream_receive_status(1) == ruvia::http2_stream_receive_status::ended);
}

TEST(Ruvia, http2_public_received_peer_settings_reports_handshake_readiness) {
    auto client = ruvia::http2_connection::client({});
    EXPECT_TRUE(!client.received_peer_settings());

    std::string wire;
    append_peer_settings(wire);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(client.received_peer_settings());
}

TEST(Ruvia, http2_public_output_drain_enforces_undrained_control_frame_limits) {
    for (const auto type : {ruvia::http2_frame_type::ping, ruvia::http2_frame_type::settings}) {
        auto client = ruvia::http2_connection::client();
        std::string settings;
        append_peer_settings(settings);
        EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);
        (void)ruvia::test::drain_output(client);

        std::string frame;
        append_frame(frame, type, 0, 0,
            type == ruvia::http2_frame_type::ping ? std::string_view("12345678") : std::string_view{});
        for (std::size_t count = 0; count < 1000; ++count) {
            EXPECT_TRUE(client.feed(frame) == ruvia::http2_feed_result::accepted);
        }
        std::string pending;
        for (std::size_t count = 0; count < 1000; ++count) {
            append_frame(pending, type, 0x1, 0,
                type == ruvia::http2_frame_type::ping ? std::string_view("12345678") : std::string_view{});
        }
        std::string output;
        ruvia::test::drain_output(client, output);
        EXPECT_TRUE(std::string_view(output) == pending);
        EXPECT_TRUE(!client.wants_write());
        for (std::size_t count = 0; count < 1000; ++count) {
            EXPECT_TRUE(client.feed(frame) == ruvia::http2_feed_result::accepted);
        }
        EXPECT_TRUE(!client.connection_error());
        EXPECT_TRUE(client.feed(frame) == ruvia::http2_feed_result::protocol_failure);
        EXPECT_TRUE(client.connection_error() == ruvia::http2_error_code::enhance_your_calm);
    }
}

namespace {

ruvia::http2_connection started_server() {
    auto server = ruvia::http2_connection::server({});
    std::string handshake(ruvia::http2_client_preface);
    append_peer_settings(handshake);
    if (server.feed(handshake) != ruvia::http2_feed_result::accepted) {
        throw std::logic_error("test HTTP/2 handshake failed");
    }
    (void)ruvia::test::drain_output(server);
    return server;
}

// A complete GET head that leaves the request stream open.
void append_open_get_request(std::string& wire, std::uint32_t stream_id) {
    std::string block;
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

struct goaway_frame final {
    std::uint32_t last_stream_id_;
    std::uint32_t error_code_;
};

[[nodiscard]] std::uint32_t read_u32(std::string_view bytes) {
    std::uint32_t value = 0;
    for (const auto byte : bytes.substr(0, 4)) {
        value = (value << 8) | static_cast<unsigned char>(byte);
    }
    return value;
}

// Every GOAWAY in whole frames.
[[nodiscard]] std::vector<goaway_frame> goaway_frames(std::string_view frames) {
    std::vector<goaway_frame> found;
    while (frames.size() >= ruvia::http2_frame_header_bytes) {
        const auto header = ruvia::parse_http2_frame_header(std::span<const char>(frames.data(), frames.size()));
        if (!header || frames.size() < ruvia::http2_frame_header_bytes + header->length_) {
            break;
        }
        const auto payload = frames.substr(ruvia::http2_frame_header_bytes, header->length_);
        if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) && payload.size() >= 8) {
            found.push_back({read_u32(payload) & 0x7FFF'FFFFU, read_u32(payload.substr(4))});
        }
        frames.remove_prefix(ruvia::http2_frame_header_bytes + header->length_);
    }
    return found;
}

}  // namespace

// RFC 9113 section 6.8: an owner-initiated immediate shutdown sends one GOAWAY
// with its error code and the last peer stream processed, then ignores input.
TEST(Ruvia, http2_public_terminate_sends_goaway_with_error_code) {
    {
        auto server = started_server();
        std::string wire;
        append_open_get_request(wire, 1);
        append_open_get_request(wire, 3);
        ASSERT_EQ(server.feed(wire), ruvia::http2_feed_result::accepted);
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                EXPECT_EQ(server.release(std::move(*request)), ruvia::http2_server_request_release_status::released);
            }
        }
        EXPECT_FALSE(server.connection_error().has_value());

        server.terminate(ruvia::http2_error_code::enhance_your_calm);
        const auto goaways = goaway_frames(ruvia::test::drain_output(server));
        ASSERT_EQ(goaways.size(), std::size_t{1});
        EXPECT_EQ(goaways.front().last_stream_id_, 3U);
        EXPECT_EQ(goaways.front().error_code_, static_cast<std::uint32_t>(ruvia::http2_error_code::enhance_your_calm));
        EXPECT_EQ(server.connection_error(), ruvia::http2_error_code::enhance_your_calm);

        // The first termination is kept; later input is refused.
        server.terminate(ruvia::http2_error_code::protocol_error);
        EXPECT_TRUE(goaway_frames(ruvia::test::drain_output(server)).empty());
        EXPECT_EQ(server.connection_error(), ruvia::http2_error_code::enhance_your_calm);
        wire.clear();
        append_open_get_request(wire, 5);
        EXPECT_EQ(server.feed(wire), ruvia::http2_feed_result::protocol_failure);
        EXPECT_EQ(server.submit_ping(), ruvia::http2_submit_status::closed);
    }
    {
        // NO_ERROR also ends the connection at once, unlike a graceful drain.
        auto client = ruvia::http2_connection::client({});
        (void)ruvia::test::drain_output(client);
        client.begin_drain();
        (void)ruvia::test::drain_output(client);
        client.terminate(ruvia::http2_error_code::no_error);
        const auto goaways = goaway_frames(ruvia::test::drain_output(client));
        ASSERT_EQ(goaways.size(), std::size_t{1});
        EXPECT_EQ(goaways.front().last_stream_id_, 0U);
        EXPECT_EQ(goaways.front().error_code_, 0U);
        EXPECT_EQ(client.connection_error(), ruvia::http2_error_code::no_error);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
        EXPECT_FALSE(std::holds_alternative<std::uint32_t>(submitted));
    }
}

TEST(Ruvia, http2_public_peer_induced_stream_errors_end_in_enhance_your_calm) {
    // Each round opens a fresh stream and provokes a stream error with a trailer
    // section that lacks END_STREAM (malformed, RFC 9113 §8.1). Drained output
    // leaves only the lifetime budget (1024 resets); undrained resets trip the
    // control-frame budget first.
    for (const bool drain_each_round : {true, false}) {
        auto server = started_server();
        const std::size_t admitted_resets = drain_each_round ? 1024 : 1000;
        std::string trailer;
        ruvia::hpack_encoder::encode_header(trailer, "x-trailer", "1");
        std::uint32_t stream_id = 1;
        for (std::size_t round = 0; round <= admitted_resets; ++round, stream_id += 2) {
            std::string wire;
            append_open_get_request(wire, stream_id);
            append_frame(wire, ruvia::http2_frame_type::headers, 0x4, stream_id, trailer);
            const auto result = server.feed(wire);
            while (server.next_event()) {
            }
            if (round < admitted_resets) {
                EXPECT_TRUE(result == ruvia::http2_feed_result::accepted);
                EXPECT_TRUE(!server.connection_error());
            } else {
                EXPECT_TRUE(result == ruvia::http2_feed_result::protocol_failure);
                EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::enhance_your_calm);
            }
            if (drain_each_round) {
                (void)ruvia::test::drain_output(server);
            }
        }
    }
}

TEST(Ruvia, http2_public_owner_resets_and_drained_pings_do_not_consume_peer_error_budgets) {
    auto server = started_server();
    std::uint32_t stream_id = 1;
    for (std::size_t round = 0; round != 1100; ++round, stream_id += 2) {
        std::string wire;
        append_open_get_request(wire, stream_id);
        append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, "keepaliv");
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        EXPECT_TRUE(request != nullptr);
        if (request == nullptr) {
            return;
        }
        EXPECT_TRUE(server.submit_reset(stream_id, ruvia::http2_error_code::cancel) ==
                    ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
        while (server.next_event()) {
        }
        (void)ruvia::test::drain_output(server);
    }
    EXPECT_TRUE(!server.connection_error());
}

TEST(Ruvia, http2_public_submit_ping_keeps_one_liveness_probe_outstanding) {
    auto server = started_server();

    EXPECT_TRUE(server.submit_ping() == ruvia::http2_submit_status::accepted);
    const auto probe = ruvia::test::drain_output(server);
    EXPECT_EQ(probe.size(), ruvia::http2_frame_header_bytes + 8);
    const auto payload = ping_payload(probe);
    EXPECT_EQ(payload.size(), std::size_t{8});

    // At most one probe is outstanding until its ACK arrives.
    EXPECT_TRUE(server.submit_ping() == ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(!server.wants_write());

    std::string ack;
    append_frame(ack, ruvia::http2_frame_type::ping, 0x1, 0, payload);
    EXPECT_TRUE(server.feed(ack) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.next_event());
    EXPECT_TRUE(!server.draining());
    EXPECT_TRUE(server.submit_ping() == ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(ruvia::test::drain_output(server) == probe);

    // The graceful-drain PING carries a different payload, so neither ACK can
    // complete the other's round trip.
    server.begin_drain();
    const auto drain_frames = ruvia::test::drain_output(server);
    const auto drain_payload = ping_payload(drain_frames);
    EXPECT_EQ(drain_payload.size(), std::size_t{8});
    EXPECT_TRUE(drain_payload != payload);

    std::string invalid;
    append_frame(invalid, ruvia::http2_frame_type::ping, 0, 1, "12345678");
    EXPECT_TRUE(server.feed(invalid) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(server.submit_ping() == ruvia::http2_submit_status::closed);
}

TEST(Ruvia, http2_public_bounds_malformed_request_stream_resets_per_connection) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);

    std::string handshake(ruvia::http2_client_preface);
    append_peer_settings(handshake);
    EXPECT_TRUE(server.feed(handshake) == ruvia::http2_feed_result::accepted);
    (void)ruvia::test::drain_output(server);

    std::string malformed;
    ruvia::hpack_encoder::encode_header(malformed, ":method", "GET");
    ruvia::hpack_encoder::encode_header(malformed, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(malformed, ":authority", "example.test");
    std::uint32_t next_stream_id = 1;
    for (std::uint32_t index = 0; index != 32; ++index) {
        std::string frame;
        append_frame(frame, ruvia::http2_frame_type::headers, 0x5, next_stream_id, malformed);
        next_stream_id += 2;
        const auto result = server.feed(frame);
        while (server.next_event()) {
        }
        (void)ruvia::test::drain_output(server);
        if (index + 1 == 32) {
            EXPECT_TRUE(result == ruvia::http2_feed_result::protocol_failure);
            EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::enhance_your_calm);
        } else {
            EXPECT_TRUE(result == ruvia::http2_feed_result::accepted);
            EXPECT_TRUE(!server.connection_error());
        }

        if (index == 15) {
            std::string valid(malformed);
            ruvia::hpack_encoder::encode_header(valid, ":path", "/");
            std::string valid_frame;
            append_frame(valid_frame, ruvia::http2_frame_type::headers, 0x5, next_stream_id, valid);
            const auto valid_stream_id = next_stream_id;
            next_stream_id += 2;
            EXPECT_TRUE(server.feed(valid_frame) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            EXPECT_TRUE(request && request->request_head());
            const auto end = server.next_event();
            EXPECT_TRUE(end && end->message_end());
            if (request && request->request_head()) {
                ruvia::http_response response;
                response.status(ruvia::http_status::no_content);
                const auto submitted = server.submit_response_head(valid_stream_id, response,
                    ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
                EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
                (void)server.release(std::move(*request->request_head()));
            }
            (void)ruvia::test::drain_output(server);
        }
    }
}

TEST(Ruvia, http2_public_malformed_response_streams_do_not_consume_server_abuse_budget) {
    auto client = ruvia::http2_connection::client({});
    std::string settings;
    append_peer_settings(settings);
    EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);

    std::array<std::uint32_t, 32> stream_ids{};
    for (auto& stream_id : stream_ids) {
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (const auto* id = std::get_if<std::uint32_t>(&submitted)) {
            stream_id = *id;
        }
    }
    (void)ruvia::test::drain_output(client);

    std::string malformed;
    ruvia::hpack_encoder::encode_header(malformed, "content-type", "text/plain");
    for (const auto stream_id : stream_ids) {
        std::string frame;
        append_frame(frame, ruvia::http2_frame_type::headers, 0x5, stream_id, malformed);
        EXPECT_TRUE(client.feed(frame) == ruvia::http2_feed_result::accepted);
        while (client.next_event()) {
        }
        (void)ruvia::test::drain_output(client);
        EXPECT_TRUE(!client.connection_error());
    }
}

TEST(Ruvia, http2_public_data_submission_stops_after_connection_failure) {
    for (const bool blocked_window : {false, true}) {
        for (const auto end_stream : {ruvia::http2_end_stream::keep_open, ruvia::http2_end_stream::end_stream}) {
            auto client = ruvia::http2_connection::client({});
            std::string settings;
            constexpr std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
            append_frame(settings, ruvia::http2_frame_type::settings, 0, 0,
                blocked_window ? std::string_view(zero_window.data(), zero_window.size()) : std::string_view{});
            EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);
            const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
                .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::streaming()});
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
            if (!std::holds_alternative<std::uint32_t>(submitted)) {
                continue;
            }
            const auto stream_id = std::get<std::uint32_t>(submitted);
            (void)ruvia::test::drain_output(client);

            std::string invalid_ping;
            append_frame(invalid_ping, ruvia::http2_frame_type::ping, 0, 0, {});
            EXPECT_TRUE(client.feed(invalid_ping) == ruvia::http2_feed_result::protocol_failure);
            EXPECT_TRUE(client.connection_error() == ruvia::http2_error_code::frame_size_error);
            (void)ruvia::test::drain_output(client);
            const auto before = client.send_window_state(stream_id);
            const auto payload = end_stream == ruvia::http2_end_stream::end_stream ? std::string_view{} : std::string_view("content");
            EXPECT_TRUE(client.submit_data(stream_id, payload, end_stream, 65535).status_ == ruvia::http2_data_submit_status::closed);
            EXPECT_TRUE(client.finish_request(stream_id) == ruvia::http2_finish_status::closed);
            EXPECT_TRUE(!client.wants_write());
            EXPECT_EQ(client.pending_data_output_bytes(stream_id), std::size_t{0});
            const auto after = client.send_window_state(stream_id);
            if (before && after) {
                EXPECT_EQ(after->connection_window_, before->connection_window_);
                EXPECT_EQ(after->stream_window_, before->stream_window_);
            }
        }
    }
}

TEST(Ruvia, http2_public_data_submission_continues_existing_stream_during_graceful_drain) {
    for (const bool peer_goaway : {false, true}) {
        auto client = ruvia::http2_connection::client({});
        std::string settings;
        append_peer_settings(settings);
        EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::streaming()});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            continue;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        if (peer_goaway) {
            constexpr std::array<char, 8> payload{0, 0, 0, 1, 0, 0, 0, 0};
            std::string goaway;
            append_frame(goaway, ruvia::http2_frame_type::goaway, 0, 0,
                std::string_view(payload.data(), payload.size()));
            EXPECT_TRUE(client.feed(goaway) == ruvia::http2_feed_result::accepted);
        } else {
            client.begin_drain();
        }
        EXPECT_TRUE(!client.connection_error());
        (void)ruvia::test::drain_output(client);
        EXPECT_TRUE(client.submit_data(stream_id, "content", ruvia::http2_end_stream::end_stream, 65535).status_ ==
                    ruvia::http2_data_submit_status::complete);
        EXPECT_EQ(client.pending_data_output_bytes(stream_id), std::size_t{7});
    }
}

TEST(Ruvia, http2_public_response_submission_stops_after_connection_failure) {
    for (const bool body_open : {false, true}) {
        {
            auto server = ruvia::http2_connection::server({});
            const auto wire = server_request_wire({});
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            auto* head = request ? request->request_head() : nullptr;
            EXPECT_TRUE(head != nullptr);
            if (head == nullptr) {
                continue;
            }
            while (server.next_event()) {
            }
            if (body_open) {
                ruvia::http_response response;
                EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
                    server.submit_streaming_response_head(1, std::move(response))));
            }
            ruvia::http_response buffered;
            buffered.body("response");
            const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::post, buffered);
            ruvia::http_response streamed;
            const ruvia::http_interim_response_head interim(ruvia::http_status::continue_value);
            const std::array fields{ruvia::http_header_view{"x-final", "done"}};
            const auto trailers = ruvia::validate_http_response_trailers(fields);
            const auto built_request = head->build_request();
            const auto validation = ruvia::validate_http2_websocket_handshake(server, 1, built_request);
            (void)ruvia::test::drain_output(server);
            std::string invalid_ping;
            append_frame(invalid_ping, ruvia::http2_frame_type::ping, 0, 0, {});
            EXPECT_TRUE(server.feed(invalid_ping) == ruvia::http2_feed_result::protocol_failure);
            (void)ruvia::test::drain_output(server);
            const auto buffered_head = server.submit_response_head(1, buffered, plan);
            EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(buffered_head) &&
                        std::get<ruvia::http2_response_head_submit_error>(buffered_head) ==
                            ruvia::http2_response_head_submit_error::closed);
            const auto streamed_head = server.submit_streaming_response_head(1, std::move(streamed),
                ruvia::http_response_trailer_intent::none);
            EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(streamed_head) &&
                        std::get<ruvia::http2_response_head_submit_error>(streamed_head) ==
                            ruvia::http2_response_head_submit_error::closed);
            EXPECT_TRUE(submit_buffered_response(server, 1, buffered) == ruvia::http2_submit_status::closed);
            EXPECT_TRUE(server.submit_connect_response_head(1, buffered) == ruvia::http2_submit_status::closed);
            EXPECT_TRUE(server.submit_interim_response_head(1, interim) == ruvia::http2_submit_status::closed);
            EXPECT_TRUE(server.finish_response(1, trailers) == ruvia::http2_finish_status::closed);
            const auto websocket = server.submit_websocket_handshake(1, built_request, validation, {});
            EXPECT_TRUE(std::holds_alternative<ruvia::http2_websocket_handshake_submit_error>(websocket) &&
                        std::get<ruvia::http2_websocket_handshake_submit_error>(websocket) ==
                            ruvia::http2_websocket_handshake_submit_error::closed);
            EXPECT_EQ(head->request_view().path_, std::string_view("/upload"));
            EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
            EXPECT_TRUE(!server.wants_write());
        }
    }
}

TEST(Ruvia, http2_public_failed_connection_releases_request_and_data_credit_without_output) {
    for (const bool release_first : {false, true}) {
        {
            auto server = ruvia::http2_connection::server({});
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "POST");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
            ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
            ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
            std::string wire(ruvia::http2_client_preface);
            append_peer_settings(wire);
            // Acknowledged SETTINGS put the advertised 1 MiB stream window in force.
            append_frame(wire, ruvia::http2_frame_type::settings, 0x1, 0, {});
            append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
            const std::string payload(16384, 'x');
            for (std::size_t index = 0; index < 32; ++index) {
                append_frame(wire, ruvia::http2_frame_type::data, 0, 1, payload);
            }
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request = server.next_event();
            auto* head = request ? request->request_head() : nullptr;
            EXPECT_TRUE(head != nullptr);
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
            EXPECT_TRUE(credit && credit->valid());
            if (!credit) {
                continue;
            }
            (void)ruvia::test::drain_output(server);
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {});
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
            (void)ruvia::test::drain_output(server);
            if (release_first) {
                EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
            } else {
                EXPECT_TRUE(server.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
            }
            credit.reset();
            if (!release_first) {
                EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
            }
            EXPECT_TRUE(server.stream_aborted(1));
            EXPECT_TRUE(!server.wants_write());
        }
    }
}

TEST(Ruvia, http2_public_failed_connection_retires_peer_finished_tunnel) {
    {
        auto client = ruvia::http2_connection::client({});
        const auto submitted = client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        std::string wire;
        append_peer_settings(wire);
        std::string block;
        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, stream_id, block);
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, stream_id, {});
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        EXPECT_TRUE(client.next_event().has_value());
        const auto ended = client.next_event();
        EXPECT_TRUE(ended && ended->tunnel_end());
        EXPECT_TRUE(client.send_window_state(stream_id).has_value());
        (void)ruvia::test::drain_output(client);
        wire.clear();
        append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {});
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        const auto terminal_output = ruvia::test::drain_output(client);
        const auto frame = ruvia::parse_http2_frame_header(std::span(terminal_output.data(), terminal_output.size()));
        EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
        if (frame) {
            EXPECT_EQ(terminal_output.size(), ruvia::http2_frame_header_bytes + frame->length_);
        }
        EXPECT_TRUE(!client.send_window_state(stream_id));
        EXPECT_TRUE(client.stream_aborted(stream_id));
        EXPECT_TRUE(!client.wants_write());
    }
}

TEST(Ruvia, http2_public_client_accepts_long_informational_sequence_before_final_response) {
    auto client = prepared_client();
    std::string wire;
    append_peer_settings(wire);
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":status", "103");
    ruvia::hpack_encoder::encode_header(block, "link", "</asset>; rel=preload");
    constexpr std::size_t informational_count = 300;
    for (std::size_t index = 0; index < informational_count; ++index) {
        append_header_section(wire, 1, block, false, false);
    }
    append_response(wire, 1, "200", "final body");
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    for (std::size_t index = 0; index < informational_count; ++index) {
        const auto event = client.next_event();
        const auto* informational = event ? event->informational_head() : nullptr;
        EXPECT_TRUE(informational != nullptr);
        if (informational == nullptr) {
            return;
        }
        EXPECT_EQ(informational->stream_id(), std::uint32_t{1});
        EXPECT_EQ(informational->head().status().value(), std::uint16_t{103});
        const auto fields = informational->head().headers();
        EXPECT_EQ(fields.size(), std::size_t{1});
        if (fields.size() == 1) {
            EXPECT_TRUE(fields.front().name() == "link");
            EXPECT_TRUE(fields.front().value() == "</asset>; rel=preload");
        }
    }
    const auto head = client.next_event();
    EXPECT_TRUE(head && head->response_head());
    if (head && head->response_head()) {
        EXPECT_EQ(head->response_head()->stream_id(), std::uint32_t{1});
        EXPECT_TRUE(head->response_head()->head().status() == ruvia::http_status::ok);
    }
    const auto body = client.next_event();
    EXPECT_TRUE(body && body->message_body_chunk());
    if (body && body->message_body_chunk()) {
        EXPECT_EQ(body->message_body_chunk()->stream_id(), std::uint32_t{1});
        EXPECT_TRUE(body->message_body_chunk()->bytes() == "final body");
    }
    const auto end = client.next_event();
    EXPECT_TRUE(end && end->message_end());
    if (end && end->message_end()) {
        EXPECT_EQ(end->message_end()->stream_id(), std::uint32_t{1});
        EXPECT_TRUE(end->message_end()->trailers().empty());
        EXPECT_TRUE(end->message_end()->content_semantics() ==
                    ruvia::http2_message_content_semantics::content);
    }
    EXPECT_TRUE(!client.next_event());
    EXPECT_TRUE(!client.connection_error());
}

TEST(Ruvia, http2_public_informational_metadata_does_not_frame_or_leak_into_final_responses) {
    for (const std::string_view length : {"0", "123"}) {
        auto client = prepared_client();
        const auto following = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .authority_ = "example.test"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(following) && std::get<std::uint32_t>(following) == 3);
        std::string wire;
        append_peer_settings(wire);
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":status", "103");
        ruvia::hpack_encoder::encode_header(block, "content-length", length);
        ruvia::hpack_encoder::encode_header(block, "trailer", "x-checksum");
        ruvia::hpack_encoder::encode_header(block, "date", "Sun, 06 Nov 1994 08:49:37 GMT");
        ruvia::hpack_encoder::encode_header(block, "date", "Mon, 07 Nov 1994 08:49:37 GMT");
        append_header_section(wire, 1, block, false, true);
        append_response(wire, 1, "200", "abc");
        append_response(wire, 3, "200", "next");
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        const auto event = client.next_event();
        const auto* info = event ? event->informational_head() : nullptr;
        EXPECT_TRUE(info != nullptr);
        if (info == nullptr) {
            continue;
        }
        EXPECT_EQ(info->stream_id(), std::uint32_t{1});
        EXPECT_EQ(info->head().status().value(), std::uint16_t{103});
        const auto fields = info->head().headers();
        EXPECT_EQ(fields.size(), std::size_t{4});
        if (fields.size() != 4) {
            continue;
        }
        EXPECT_EQ(fields[0].name(), "content-length");
        EXPECT_EQ(fields[0].value(), length);
        EXPECT_EQ(fields[1].name(), "trailer");
        EXPECT_EQ(fields[1].value(), "x-checksum");
        const auto consume_response = [&](std::uint32_t stream_id, std::string_view expected) {
            const auto head = client.next_event();
            EXPECT_TRUE(head && head->response_head());
            if (head && head->response_head()) {
                EXPECT_EQ(head->response_head()->stream_id(), stream_id);
                EXPECT_TRUE(head->response_head()->head().status() == ruvia::http_status::ok);
                EXPECT_TRUE(head->response_head()->head().headers().empty());
            }
            auto body = client.next_event();
            auto* chunk = body ? body->message_body_chunk() : nullptr;
            EXPECT_TRUE(chunk != nullptr);
            if (chunk != nullptr) {
                EXPECT_EQ(chunk->stream_id(), stream_id);
                EXPECT_EQ(chunk->bytes(), expected);
                (void)chunk->take_credit();
            }
            const auto end = client.next_event();
            EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == stream_id);
            if (end && end->message_end()) {
                EXPECT_TRUE(end->message_end()->trailers().empty());
            }
        };
        consume_response(1, "abc");
        consume_response(3, "next");
        EXPECT_EQ(fields[2].name(), "date");
        EXPECT_EQ(fields[2].value(), "Sun, 06 Nov 1994 08:49:37 GMT");
        EXPECT_EQ(fields[3].name(), "date");
        EXPECT_EQ(fields[3].value(), "Mon, 07 Nov 1994 08:49:37 GMT");
        EXPECT_TRUE(!client.next_event());
        EXPECT_TRUE(!client.connection_error());
    }
}

TEST(Ruvia, http2_public_client_rejects_goaway_excluding_informational_response) {
    for (const std::size_t informational_count : {std::size_t{1}, std::size_t{256}}) {
        auto client = prepared_client();
        std::string wire;
        append_peer_settings(wire);
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":status", "103");
        for (std::size_t index = 0; index < informational_count; ++index) {
            append_header_section(wire, 1, block, false, false);
        }
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        for (std::size_t index = 0; index < informational_count; ++index) {
            const auto event = client.next_event();
            const auto* informational = event ? event->informational_head() : nullptr;
            EXPECT_TRUE(informational != nullptr);
            if (informational == nullptr) {
                return;
            }
            EXPECT_EQ(informational->stream_id(), std::uint32_t{1});
            EXPECT_EQ(informational->head().status().value(), std::uint16_t{103});
        }
        EXPECT_TRUE(!client.next_event());
        EXPECT_TRUE(!client.connection_error());
        (void)ruvia::test::drain_output(client);
        wire.clear();
        constexpr std::array<char, 8> payload{};
        append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0,
            std::string_view(payload.data(), payload.size()));
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        EXPECT_TRUE(client.connection_error() == ruvia::http2_error_code::protocol_error);
        // A request_unprocessed event would incorrectly authorize replay.
        EXPECT_TRUE(!client.next_event());
        const auto output = ruvia::test::drain_output(client);
        const auto frame = ruvia::parse_http2_frame_header(std::span(output.data(), output.size()));
        EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
    }
}

TEST(Ruvia, http2_public_client_rejects_informational_response_with_end_stream) {
    for (const auto status : {std::string_view("100"), std::string_view("103")}) {
        auto client = prepared_client();
        std::string wire;
        append_peer_settings(wire);
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":status", status);
        append_header_section(wire, 1, block, true, false);
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        const auto event = client.next_event();
        const auto* closed = event ? event->stream_closed() : nullptr;
        EXPECT_TRUE(closed != nullptr);
        if (closed != nullptr) {
            EXPECT_EQ(closed->stream_id(), std::uint32_t{1});
            EXPECT_TRUE(closed->source() == ruvia::http2_stream_close_source::local);
            EXPECT_TRUE(closed->error() == ruvia::http2_error_code::protocol_error);
        }
        EXPECT_TRUE(!client.next_event());
        EXPECT_TRUE(!client.connection_error());
    }
}

TEST(Ruvia, http2_public_response_head_and_trailers_are_owned_by_events) {
    auto client = prepared_client();
    auto wire = client_response_with_trailers_wire();
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);

    auto head_event = client.next_event();
    EXPECT_TRUE(head_event && head_event->response_head() != nullptr);
    auto owned_head = std::move(*head_event->response_head()).take_head();
    EXPECT_TRUE(owned_head.status() == ruvia::http_status::ok);
    EXPECT_TRUE(owned_head.headers().size() == 1);
    EXPECT_TRUE(owned_head.headers().front().name() == "x-test");

    auto end_event = client.next_event();
    EXPECT_TRUE(end_event && end_event->message_end() != nullptr);
    EXPECT_TRUE(end_event->message_end()->trailers().size() == 1);
    auto owned_trailers_value = std::move(*end_event->message_end()).take_trailers();
    EXPECT_TRUE(owned_trailers_value.size() == 1);
    EXPECT_TRUE(owned_trailers_value.front().name() == "x-trace");
    EXPECT_TRUE(owned_trailers_value.front().value() == "done");

    auto replacement = ruvia::http2_connection::client({});
    client = std::move(replacement);
    EXPECT_TRUE(owned_head.headers().front().value() == "owned");
    EXPECT_TRUE(owned_trailers_value.front().value() == "done");
}

// RFC 9110 section 6.5.1 binds only the sender: a received response trailer
// that is not permitted as a trailer is discarded, while a connection-specific
// field keeps the stream malformed (RFC 9113 section 8.2.2).
TEST(Ruvia, http2_public_client_discards_response_trailers_not_permitted_as_trailers) {
    for (const std::string_view name : {"content-type", "date", "connection"}) {
        const bool malformed = name == "connection";
        auto client = prepared_client();
        std::string wire;
        append_peer_settings(wire);
        append_response(wire, 1, "200", "abc", name, "text/plain");
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        unsigned ends = 0;
        unsigned resets = 0;
        std::size_t trailers = 1;
        while (auto event = client.next_event()) {
            if (auto* chunk = event->message_body_chunk()) {
                (void)chunk->take_credit();
            } else if (const auto* end = event->message_end()) {
                ++ends;
                trailers = end->trailers().size();
            } else if (const auto* closed = event->stream_closed()) {
                resets += closed->error() == ruvia::http2_error_code::protocol_error;
            }
        }
        EXPECT_EQ(ends, malformed ? 0U : 1U);
        EXPECT_EQ(resets, malformed ? 1U : 0U);
        if (!malformed) {
            EXPECT_EQ(trailers, std::size_t{0});
        }
        EXPECT_TRUE(!client.connection_error());
    }
}

// RFC 9110 section 15: a received :status 000..099 or 600..999 is processed as
// 5xx.
TEST(Ruvia, http2_public_client_processes_out_of_range_status_as_server_error) {
    for (const auto& [status, value] : {std::pair{std::string_view("600"), 600},
             std::pair{std::string_view("099"), 99}, std::pair{std::string_view("000"), 0}}) {
        auto client = prepared_client();
        std::string wire;
        append_peer_settings(wire);
        append_response(wire, 1, status);
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head_event = client.next_event();
        EXPECT_TRUE(head_event && head_event->response_head() != nullptr);
        if (head_event && head_event->response_head() != nullptr) {
            const auto head = std::move(*head_event->response_head()).take_head();
            EXPECT_TRUE(head.status() == ruvia::http_status::internal_server_error);
            EXPECT_EQ(head.received_status().value(), value);
        }
        EXPECT_TRUE(!client.connection_error());
    }
}

TEST(Ruvia, http2_public_single_and_fragmented_request_fields_are_owned_and_keep_hpack_indexes) {
    const std::string head_name = "x-request-" + std::string(48, 'h');
    const std::string trailer_name = "x-request-" + std::string(48, 't');
    const std::string head_value(256, 'h');
    const std::string trailer_value(256, 't');
    for (const bool fragmented : {false, true}) {
        auto server = ruvia::http2_connection::server({});
        std::string head;
        ruvia::hpack_encoder::encode_header(head, ":method", "GET");
        ruvia::hpack_encoder::encode_header(head, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(head, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(head, ":path", "/owned");
        const auto field_offset = head.size();
        ruvia::hpack_encoder::encode_header(head, head_name, head_value);
        head[field_offset] = static_cast<char>(0x40);  // Literal with incremental indexing.
        std::string trailer;
        ruvia::hpack_encoder::encode_header(trailer, trailer_name, trailer_value);
        trailer[0] = static_cast<char>(0x40);
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        append_header_section(wire, 1, head, false, fragmented);
        append_header_section(wire, 1, trailer, true, fragmented);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::fill(wire.begin(), wire.end(), '#');
        auto request = server.next_event();
        EXPECT_TRUE(request && request->request_head());
        if (!request || !request->request_head()) {
            continue;
        }
        const auto received = request->request_head()->build_request();
        EXPECT_TRUE(received.target() == "/owned");
        EXPECT_TRUE(received.header(head_name) == head_value);
        auto end = server.next_event();
        EXPECT_TRUE(end && end->message_end());
        if (!end || !end->message_end()) {
            continue;
        }
        EXPECT_EQ(end->message_end()->trailers().size(), std::size_t{1});
        EXPECT_TRUE(end->message_end()->trailers().front().name() == trailer_name);
        EXPECT_TRUE(end->message_end()->trailers().front().value() == trailer_value);

        head.resize(field_offset);
        head.push_back(static_cast<char>(0xbf));     // Older head field: dynamic index 63.
        trailer.assign(1, static_cast<char>(0xbe));  // Newest trailer field: dynamic index 62.
        wire.clear();
        append_header_section(wire, 3, head, false, fragmented);
        append_header_section(wire, 3, trailer, true, fragmented);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::fill(wire.begin(), wire.end(), '#');
        auto indexed_request = server.next_event();
        EXPECT_TRUE(indexed_request && indexed_request->request_head());
        if (indexed_request && indexed_request->request_head()) {
            EXPECT_TRUE(indexed_request->request_head()->build_request().header(head_name) == head_value);
        }
        auto indexed_end = server.next_event();
        EXPECT_TRUE(indexed_end && indexed_end->message_end());
        if (indexed_end && indexed_end->message_end()) {
            EXPECT_EQ(indexed_end->message_end()->trailers().size(), std::size_t{1});
            EXPECT_TRUE(indexed_end->message_end()->trailers().front().name() == trailer_name);
            EXPECT_TRUE(indexed_end->message_end()->trailers().front().value() == trailer_value);
        }
        EXPECT_TRUE(received.header(head_name) == head_value);
        EXPECT_TRUE(end->message_end()->trailers().front().value() == trailer_value);
    }
}

TEST(Ruvia, http2_public_single_and_fragmented_response_fields_are_owned_and_keep_hpack_indexes) {
    const std::string head_name = "x-response-" + std::string(48, 'h');
    const std::string trailer_name = "x-response-" + std::string(48, 't');
    const std::string head_value(256, 'h');
    const std::string trailer_value(256, 't');
    for (const bool fragmented : {false, true}) {
        auto client = prepared_client();
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/two"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        std::string head;
        ruvia::hpack_encoder::encode_status(head, ruvia::http_status::ok);
        const auto field_offset = head.size();
        ruvia::hpack_encoder::encode_header(head, head_name, head_value);
        head[field_offset] = static_cast<char>(0x40);
        std::string trailer;
        ruvia::hpack_encoder::encode_header(trailer, trailer_name, trailer_value);
        trailer[0] = static_cast<char>(0x40);
        std::string wire;
        append_peer_settings(wire);
        append_header_section(wire, 1, head, false, fragmented);
        append_header_section(wire, 1, trailer, true, fragmented);
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        std::fill(wire.begin(), wire.end(), '#');
        auto response = client.next_event();
        EXPECT_TRUE(response && response->response_head());
        if (!response || !response->response_head()) {
            continue;
        }
        auto owned_head = std::move(*response->response_head()).take_head();
        EXPECT_TRUE(owned_head.status() == ruvia::http_status::ok);
        EXPECT_EQ(owned_head.headers().size(), std::size_t{1});
        EXPECT_TRUE(owned_head.headers().front().name() == head_name);
        EXPECT_TRUE(owned_head.headers().front().value() == head_value);
        auto end = client.next_event();
        EXPECT_TRUE(end && end->message_end());
        if (!end || !end->message_end()) {
            continue;
        }
        auto owned_trailers = std::move(*end->message_end()).take_trailers();
        EXPECT_EQ(owned_trailers.size(), std::size_t{1});
        EXPECT_TRUE(owned_trailers.front().name() == trailer_name);
        EXPECT_TRUE(owned_trailers.front().value() == trailer_value);

        head.resize(field_offset);
        head.push_back(static_cast<char>(0xbf));
        trailer.assign(1, static_cast<char>(0xbe));
        wire.clear();
        append_header_section(wire, 3, head, false, fragmented);
        append_header_section(wire, 3, trailer, true, fragmented);
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        std::fill(wire.begin(), wire.end(), '#');
        auto indexed_response = client.next_event();
        EXPECT_TRUE(indexed_response && indexed_response->response_head());
        if (indexed_response && indexed_response->response_head()) {
            const auto& indexed_head = indexed_response->response_head()->head();
            EXPECT_EQ(indexed_head.headers().size(), std::size_t{1});
            EXPECT_TRUE(indexed_head.headers().front().name() == head_name);
            EXPECT_TRUE(indexed_head.headers().front().value() == head_value);
        }
        auto indexed_end = client.next_event();
        EXPECT_TRUE(indexed_end && indexed_end->message_end());
        if (indexed_end && indexed_end->message_end()) {
            EXPECT_EQ(indexed_end->message_end()->trailers().size(), std::size_t{1});
            EXPECT_TRUE(indexed_end->message_end()->trailers().front().name() == trailer_name);
            EXPECT_TRUE(indexed_end->message_end()->trailers().front().value() == trailer_value);
        }
        EXPECT_TRUE(owned_head.headers().front().value() == head_value);
        EXPECT_TRUE(owned_trailers.front().value() == trailer_value);
    }
}

TEST(Ruvia, http2_public_head_205_keeps_nonzero_content_length_as_metadata) {
    auto client = prepared_client_method("HEAD");
    std::string block;
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::reset_content);
    ruvia::hpack_encoder::encode_header(block, "content-length", "7");
    std::string wire;
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    const auto head = client.next_event();
    EXPECT_TRUE(head && head->response_head());
    if (head && head->response_head()) {
        const auto& received = head->response_head()->head();
        EXPECT_TRUE(received.status() == ruvia::http_status::reset_content);
        EXPECT_TRUE(!received.headers().empty());
        if (!received.headers().empty()) {
            EXPECT_EQ(received.headers().front().value(), "7");
        }
    }
    const auto end = client.next_event();
    EXPECT_TRUE(end && end->message_end());
    if (end && end->message_end()) {
        EXPECT_TRUE(end->message_end()->content_semantics() ==
                    ruvia::http2_message_content_semantics::metadata_only);
    }
    EXPECT_TRUE(!client.next_event());
    EXPECT_TRUE(!client.connection_error());
}

TEST(Ruvia, http2_public_205_content_is_discarded_with_flow_control_credit_returned) {
    // RFC 9110 section 15.3.6 binds only the 205 sender; DATA framing is
    // unambiguous, so the client consumes and discards content.
    for (const bool declared : {false, true}) {
        auto client = prepared_client_method("GET");
        (void)ruvia::test::drain_output(client);
        std::string block;
        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::reset_content);
        // Enough discarded bytes to cross the half-window WINDOW_UPDATE threshold.
        const std::string bulk(16384, 'e');
        constexpr int bulk_frames = 33;
        if (declared) {
            ruvia::hpack_encoder::encode_header(
                block, "content-length", std::to_string(bulk.size() * bulk_frames));
        }
        std::string wire;
        append_peer_settings(wire);
        // Acknowledged SETTINGS put the advertised 1 MiB stream window in force.
        append_frame(wire, ruvia::http2_frame_type::settings, 0x1, 0, {});
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        for (int frame = 0; frame != bulk_frames; ++frame) {
            append_frame(wire, ruvia::http2_frame_type::data, frame + 1 == bulk_frames ? 0x1 : 0x0, 1, bulk);
        }
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        const auto head = client.next_event();
        EXPECT_TRUE(head && head->response_head() &&
                    head->response_head()->head().status() == ruvia::http_status::reset_content);
        const auto end = client.next_event();
        EXPECT_TRUE(end && end->message_end());
        EXPECT_TRUE(!client.next_event());
        EXPECT_TRUE(!client.connection_error());
        // The discarded bytes' connection credit is returned without an owner.
        constexpr std::string_view connection_window_update{"\0\0\x04\x08\0\0\0\0\0", 9};
        const auto output = ruvia::test::drain_output(client);
        EXPECT_TRUE(std::string_view(output).find(connection_window_update) != std::string_view::npos);
    }

    // Content-Length still binds the DATA sum (RFC 9113 section 8.1.1).
    auto client = prepared_client_method("GET");
    (void)ruvia::test::drain_output(client);
    std::string block;
    ruvia::hpack_encoder::encode_status(block, ruvia::http_status::reset_content);
    ruvia::hpack_encoder::encode_header(block, "content-length", "2");
    std::string wire;
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "hello");
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    const auto head = client.next_event();
    EXPECT_TRUE(head && head->response_head());
    while (auto event = client.next_event()) {
        EXPECT_TRUE(!event->message_body_chunk());
        EXPECT_TRUE(!event->message_end());
    }
    EXPECT_TRUE(!client.connection_error());
}

TEST(Ruvia, http2_public_message_end_reports_metadata_only_and_empty_trailers) {
    for (const auto method : {std::string_view("HEAD"), std::string_view("GET")}) {
        auto client = prepared_client_method(method);
        std::string wire;
        append_peer_settings(wire);
        append_response(wire, 1, method == "HEAD" ? "200" : "304");
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = client.next_event();
        EXPECT_TRUE(head && head->response_head() != nullptr);
        auto end = client.next_event();
        EXPECT_TRUE(end && end->message_end() != nullptr);
        EXPECT_TRUE(end->message_end()->trailers().empty());
        EXPECT_TRUE(end->message_end()->content_semantics() ==
                    ruvia::http2_message_content_semantics::metadata_only);
    }
    auto client = prepared_client_method("GET");
    std::string wire;
    append_peer_settings(wire);
    append_response(wire, 1, "200", "body");
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(client.next_event().has_value());
    EXPECT_TRUE(client.next_event().has_value());
    const auto end = client.next_event();
    EXPECT_TRUE(end && end->message_end() != nullptr);
    EXPECT_TRUE(end->message_end()->content_semantics() ==
                ruvia::http2_message_content_semantics::content);
}

// nghttp2 offers no rollback once input processing allocates, so an allocation
// Errors nghttp2 reports as fatal from its receive call still end the connection
// with a GOAWAY carrying the mapped error code.
TEST(Ruvia, http2_public_fatal_receive_errors_end_in_goaway) {
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
        auto server = ruvia::http2_connection::server({});
        (void)ruvia::test::drain_output(server);
        EXPECT_TRUE(server.feed("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n") == ruvia::http2_feed_result::protocol_failure);
        EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
        EXPECT_TRUE(goaway_error(ruvia::test::drain_output(server)) ==
                    static_cast<std::uint32_t>(ruvia::http2_error_code::protocol_error));
    }
    {
        // CVE-2024-27316: a header block may not continue indefinitely.
        auto server = started_server();
        std::string wire;
        append_frame(wire, ruvia::http2_frame_type::headers, 0, 1, {});
        for (std::size_t index = 0; index != 1025; ++index) {
            append_frame(wire, ruvia::http2_frame_type::continuation, 0, 1, {});
        }
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::enhance_your_calm);
        EXPECT_TRUE(goaway_error(ruvia::test::drain_output(server)) ==
                    static_cast<std::uint32_t>(ruvia::http2_error_code::enhance_your_calm));
    }
}

TEST(Ruvia, http2_public_data_credit_merge_is_linear) {
    auto client = prepared_client();
    auto wire = client_response_wire("x", false, false);
    append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "x");
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    std::string drained;
    ruvia::test::drain_output(client, drained);
    EXPECT_TRUE(client.next_event().has_value());
    auto first = client.next_event();
    EXPECT_TRUE(first && first->message_body_chunk() != nullptr);
    auto first_credit = first->message_body_chunk()->take_credit();
    constexpr std::size_t frame_count = 2;
    for (std::size_t index = 1; index < frame_count; ++index) {
        auto chunk = client.next_event();
        EXPECT_TRUE(chunk && chunk->message_body_chunk() != nullptr);
        auto next_credit = chunk->message_body_chunk()->take_credit();
        first_credit.merge(std::move(next_credit));
        EXPECT_TRUE(first_credit.valid());
        EXPECT_TRUE(!next_credit.valid());
        EXPECT_TRUE(!client.wants_write());
    }
    auto end = client.next_event();
    EXPECT_TRUE(end && end->message_end() != nullptr);
    EXPECT_TRUE(!client.wants_write());
    (void)ruvia::http2_received_data_credit(std::move(first_credit));
    EXPECT_TRUE(client.submit_reset(1, ruvia::http2_error_code::cancel) ==
                ruvia::http2_submit_status::closed);
}

TEST(Ruvia, http2_public_data_credit_merge_rejects_different_connection_unchanged) {
    auto first_client = prepared_client();
    std::string first_wire;
    append_peer_settings(first_wire);
    append_response(first_wire, 1, "200", "a", {}, {});
    EXPECT_TRUE(first_client.feed(first_wire) == ruvia::http2_feed_result::accepted);
    (void)first_client.next_event();
    auto first_event = first_client.next_event();
    auto first_credit = first_event->message_body_chunk()->take_credit();

    auto second_client = prepared_client();
    std::string second_wire;
    append_peer_settings(second_wire);
    append_response(second_wire, 1, "200", "b", {}, {});
    EXPECT_TRUE(second_client.feed(second_wire) == ruvia::http2_feed_result::accepted);
    (void)second_client.next_event();
    auto second_event = second_client.next_event();
    auto second_credit = second_event->message_body_chunk()->take_credit();

    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)first_credit.merge(std::move(second_credit)); }));
    EXPECT_TRUE(first_credit.valid());
    EXPECT_TRUE(second_credit.valid());
    (void)ruvia::http2_received_data_credit(std::move(first_credit));
    (void)ruvia::http2_received_data_credit(std::move(second_credit));
    EXPECT_TRUE(!first_client.connection_error());
    EXPECT_TRUE(!second_client.connection_error());
}

TEST(Ruvia, http2_public_data_credit_merge_rejects_different_stream_unchanged) {
    auto client = prepared_client();
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/two"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));

    std::string wire;
    append_peer_settings(wire);
    append_response(wire, 1, "200", "a");
    append_response(wire, 3, "200", "b");
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    (void)client.next_event();
    auto first_event = client.next_event();
    EXPECT_TRUE(first_event && first_event->message_body_chunk() != nullptr);
    auto first_credit = first_event->message_body_chunk()->take_credit();
    (void)client.next_event();
    (void)client.next_event();
    auto second_event = client.next_event();
    EXPECT_TRUE(second_event && second_event->message_body_chunk() != nullptr);
    auto second_credit = second_event->message_body_chunk()->take_credit();

    EXPECT_TRUE(ruvia::test::throws_on([&] { first_credit.merge(std::move(second_credit)); }));
    EXPECT_TRUE(first_credit.valid());
    EXPECT_TRUE(second_credit.valid());
    (void)ruvia::http2_received_data_credit(std::move(first_credit));
    (void)ruvia::http2_received_data_credit(std::move(second_credit));
    EXPECT_TRUE(!client.connection_error());
}

TEST(Ruvia, http2_public_server_message_end_owns_request_trailers) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    auto wire = server_request_with_trailers_wire();
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    EXPECT_TRUE(request && request->request_head() != nullptr);
    EXPECT_TRUE(!request->request_head()->build_request().header("x-request-trace"));
    const auto chunk = server.next_event();
    EXPECT_TRUE(chunk && chunk->message_body_chunk() != nullptr);
    auto end = server.next_event();
    EXPECT_TRUE(end && end->message_end() != nullptr);
    EXPECT_TRUE(end->message_end()->content_semantics() ==
                ruvia::http2_message_content_semantics::content);
    EXPECT_TRUE(end->message_end()->trailers().size() == 1);
    auto trailers = std::move(*end->message_end()).take_trailers();
    EXPECT_TRUE(trailers.size() == 1);
    if (!trailers.empty()) {
        EXPECT_TRUE(trailers.front().name() == "x-request-trace");
        EXPECT_TRUE(trailers.front().value() == "done");
    }
    EXPECT_TRUE(server.release(std::move(*request->request_head())) ==
                ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_request_headers_remain_valid_while_trailers_arrive) {
    auto server = ruvia::http2_connection::server({});
    const std::string initial_value(700, 'a');
    const std::string trailer_value(3000, 'b');
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");
    ruvia::hpack_encoder::encode_header(block, "x-initial", initial_value);
    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    EXPECT_TRUE(request && request->request_head() != nullptr);
    const auto built_initial = request->request_head()->build_request();
    const auto borrowed = built_initial.header("x-initial");
    EXPECT_TRUE(borrowed && *borrowed == initial_value);

    block.clear();
    ruvia::hpack_encoder::encode_header(block, "x-trailer", trailer_value);
    std::string trailers;
    append_frame(trailers, ruvia::http2_frame_type::headers, 0x5, 1, block);
    EXPECT_TRUE(server.feed(trailers) == ruvia::http2_feed_result::accepted);
    auto end = server.next_event();
    EXPECT_TRUE(end && end->message_end() != nullptr);
    EXPECT_TRUE(borrowed && *borrowed == initial_value);
    const auto built_after = request->request_head()->build_request();
    EXPECT_TRUE(!built_after.header("x-trailer"));
    const auto fields_value = end->message_end()->trailers();
    EXPECT_TRUE(fields_value.size() == 1);
    if (!fields_value.empty()) {
        EXPECT_TRUE(fields_value.front().value() == trailer_value);
    }
    EXPECT_TRUE(server.release(std::move(*request->request_head())) ==
                ruvia::http2_server_request_release_status::released);
}

// A trailer declaration never authorizes forbidden actual trailers. Fields the
// sender was not permitted to generate (RFC 9110 section 6.5.1) are discarded
// and never merged into the head; connection-specific fields stay malformed
// (RFC 9113 section 8.2.2).
TEST(Ruvia, http2_public_trailer_declaration_does_not_authorize_forbidden_actual_fields) {
    for (const std::string_view forbidden : {"content-length", "host", "cache-control", "connection"}) {
        const bool malformed = forbidden == "connection";
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        std::string block;
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
        auto server = ruvia::http2_connection::server({});
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
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
                EXPECT_TRUE(chunk->stream_id() == 1);
                body.append(chunk->bytes());
                (void)chunk->take_credit();
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
        EXPECT_TRUE(first && next && next_ends == 1);
        EXPECT_EQ(resets, malformed ? 1U : 0U);
        EXPECT_EQ(first_ends, malformed ? 0U : 1U);
        if (!malformed) {
            EXPECT_EQ(first_trailers, std::size_t{1});
            EXPECT_TRUE(kept_checksum);
        }
        EXPECT_EQ(body, "abc");
        EXPECT_TRUE(!server.connection_error());
        if (first) {
            const auto built_first = first->build_request();
            EXPECT_EQ(built_first.header("Trailer").value_or(""), "Content-Length, Host, bad field");
            EXPECT_EQ(built_first.header("Content-Length").value_or(""), "3");
            EXPECT_EQ(built_first.header("Host").value_or(""), "example.test");
            (void)server.release(std::move(*first));
        }
        if (next) {
            EXPECT_EQ(next->request_view().path_, "/next");
            ruvia::http_response reply;
            EXPECT_TRUE(submit_buffered_response(server, 3, reply) == ruvia::http2_submit_status::accepted);
            (void)server.release(std::move(*next));
        }
    }
}

TEST(Ruvia, http2_public_server_release_before_message_end_keeps_terminal_event) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    auto request = server.next_event();
    EXPECT_TRUE(request && request->request_head() != nullptr);
    ruvia::http_response response;
    EXPECT_TRUE(submit_buffered_response(server, 1, response) ==
                ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(server.release(std::move(*request->request_head())) ==
                ruvia::http2_server_request_release_status::released);

    auto end = server.next_event();
    EXPECT_TRUE(end && end->message_end() != nullptr);
    EXPECT_TRUE(end->message_end()->stream_id() == 1);
    EXPECT_TRUE(end->message_end()->trailers().empty());
}

TEST(Ruvia, http2_public_server_request_snapshot_exposes_http_decisions) {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "POST");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
    ruvia::hpack_encoder::encode_header(block, "content-length", "1");
    ruvia::hpack_encoder::encode_header(block, "expect", "100-continue");
    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);

    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto event = server.next_event();
    auto* request = event ? event->request_head() : nullptr;
    EXPECT_TRUE(request != nullptr);
    if (request == nullptr) {
        return;
    }
    EXPECT_TRUE(request->snapshot().content_ == ruvia::http_request_content_indication::will_follow);
    EXPECT_TRUE(request->snapshot().body_open_);
    EXPECT_TRUE(!request->snapshot().connect_pending_);
    const auto plan = request->expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(plan == ruvia::http_server_expectation_plan::send_continue);
}

TEST(Ruvia, http2_public_semantically_invalid_expectations_preserve_upload_boundaries) {
    for (const std::string_view expectation :
        {"bad field", "100-continue;foo=bar", "custom=\"unterminated", "100-continue, bad field"}) {
        auto server = ruvia::http2_connection::server({});
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
        ruvia::hpack_encoder::encode_header(block, "content-length", "3");
        ruvia::hpack_encoder::encode_header(block, "expect", expectation);
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        append_header_section(wire, 1, block, false, true);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        EXPECT_TRUE(request != nullptr);
        if (!request) {
            continue;
        }
        EXPECT_EQ(request->stream_id(), std::uint32_t{1});
        EXPECT_TRUE(request->build_request().header("Expect") == expectation);
        EXPECT_TRUE(request->snapshot().body_open_);
        EXPECT_TRUE(request->snapshot().content_ == ruvia::http_request_content_indication::will_follow);
        const auto rejected = request->expectation_plan(ruvia::http_unsupported_expectation_policy::reject);
        EXPECT_TRUE(rejected == ruvia::http_server_expectation_plan::reject_unsupported);
        EXPECT_EQ(
            ruvia::http_unsupported_expectation_error().status(), ruvia::http_status::expectation_failed);
        const auto ignored = request->expectation_plan(ruvia::http_unsupported_expectation_policy::ignore);
        const bool known_continue = expectation == "100-continue, bad field";
        EXPECT_TRUE(ignored == (known_continue ? ruvia::http_server_expectation_plan::send_continue
                                               : ruvia::http_server_expectation_plan::no_action));
        EXPECT_TRUE(!server.next_event());

        wire.clear();
        append_frame(wire, ruvia::http2_frame_type::data, 0, 1, "a");
        append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "bc");
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::string body;
        unsigned ends = 0;
        while (auto content = server.next_event()) {
            if (auto* chunk = content->message_body_chunk()) {
                EXPECT_EQ(chunk->stream_id(), std::uint32_t{1});
                body.append(chunk->bytes());
                (void)chunk->take_credit();
            } else {
                const auto* end = content->message_end();
                EXPECT_TRUE(end != nullptr);
                if (end) {
                    EXPECT_EQ(end->stream_id(), std::uint32_t{1});
                    EXPECT_TRUE(end->trailers().empty());
                    ++ends;
                }
            }
        }
        EXPECT_EQ(body, "abc");
        EXPECT_EQ(ends, 1u);
        ruvia::http_response reply;
        EXPECT_TRUE(submit_buffered_response(server, 1, reply) == ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.release(std::move(*request)) ==
                    ruvia::http2_server_request_release_status::released);

        block.clear();
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/next");
        wire.clear();
        append_header_section(wire, 3, block, true, true);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto next = server.next_event();
        EXPECT_TRUE(next && next->request_head());
        if (next && next->request_head()) {
            EXPECT_EQ(next->request_head()->stream_id(), std::uint32_t{3});
            EXPECT_EQ(next->request_head()->request_view().path_, "/next");
            EXPECT_TRUE(submit_buffered_response(server, 3, reply) == ruvia::http2_submit_status::accepted);
            EXPECT_TRUE(server.release(std::move(*next->request_head())) ==
                        ruvia::http2_server_request_release_status::released);
        }
        auto end = server.next_event();
        EXPECT_TRUE(end && end->message_end());
        if (end && end->message_end()) {
            EXPECT_EQ(end->message_end()->stream_id(), std::uint32_t{3});
        }
        EXPECT_TRUE(!server.next_event());
        EXPECT_TRUE(!server.connection_error());
    }
}

// body_open_ describes the head as published: a HEADERS frame without
// END_STREAM promises content events even when the DATA that completes the
// request arrived in the same feed() and the core already consumed it.
TEST(Ruvia, http2_public_server_request_snapshot_body_open_survives_batched_content) {
    for (const bool end_stream_on_headers : {false, true}) {
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "POST");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/upload");
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers,
            static_cast<std::uint8_t>(end_stream_on_headers ? 0x5 : 0x4), 1, block);
        if (!end_stream_on_headers) {
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "x");
        }

        auto server = ruvia::http2_connection::server({});
        (void)ruvia::test::drain_output(server);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        EXPECT_TRUE(request != nullptr);
        if (request == nullptr) {
            return;
        }
        EXPECT_TRUE(request->snapshot().body_open_ == !end_stream_on_headers);
        EXPECT_EQ(request->snapshot().initial_end_stream_, end_stream_on_headers);
        EXPECT_TRUE(!request->snapshot().connect_pending_);
        if (!end_stream_on_headers) {
            const auto chunk = server.next_event();
            EXPECT_TRUE(chunk && chunk->message_body_chunk() != nullptr &&
                        chunk->message_body_chunk()->bytes() == "x");
        }
        const auto end = server.next_event();
        EXPECT_TRUE(end && end->message_end() != nullptr);
        EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_dropped_request_before_message_end_keeps_terminal_event) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    auto wire = server_request_wire({});
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    EXPECT_TRUE(request && request->request_head() != nullptr);
    request.reset();

    bool saw_end = false;
    while (const auto event = server.next_event()) {
        if (event->message_end() != nullptr) {
            saw_end = true;
            EXPECT_TRUE(event->message_end()->stream_id() == 1);
            EXPECT_TRUE(event->message_end()->trailers().empty());
        }
    }
    EXPECT_TRUE(saw_end);
}

TEST(Ruvia, http2_public_client_reset_before_terminal_events_drain_keeps_events_readable) {
    auto client = ruvia::http2_connection::client({});
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/", .content_ = ruvia::http_request_content_plan::streaming()});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    auto wire = client_response_wire({}, false, true);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    const auto reset = client.submit_reset(1, ruvia::http2_error_code::cancel);
    EXPECT_TRUE(reset == ruvia::http2_submit_status::accepted);

    bool saw_head = false;
    bool saw_end = false;
    while (const auto event = client.next_event()) {
        if (event->response_head() != nullptr) {
            saw_head = true;
            EXPECT_TRUE(event->response_head()->head().status() == ruvia::http_status::ok);
        }
        if (event->message_end() != nullptr) {
            saw_end = true;
            EXPECT_TRUE(event->message_end()->trailers().empty());
        }
    }
    EXPECT_TRUE(saw_head);
    EXPECT_TRUE(saw_end);
}

TEST(Ruvia, http2_public_streaming_known_length_keeps_upload_open_for_trailers) {
    for (const std::uint64_t length : {0U, 3U}) {
        auto client = ruvia::http2_connection::client({});
        auto server = ruvia::http2_connection::server({});
        ruvia::test::output_relay relay;
        auto exchange_value = [&](auto& from, auto& to) {
            EXPECT_TRUE(relay.feed(from, to) == ruvia::http2_feed_result::accepted);
        };
        exchange_value(client, server);
        exchange_value(server, client);
        const auto head = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming(length)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(head));
        exchange_value(client, server);
        bool saw_end = false;
        std::optional<ruvia::http2_request_head_event> lease;
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                EXPECT_EQ(request->build_request().header("content-length").value_or(""), length == 0 ? "0" : "3");
                lease.emplace(std::move(*request));
            }
            saw_end |= event->message_end() != nullptr;
        }
        EXPECT_TRUE(!saw_end);
        const std::array<ruvia::http_header_view, 1> trailers{{{"x-end", "retained"}}};
        if (length != 0) {
            // Content short of the declared length is a value; nothing changes.
            EXPECT_TRUE(client.finish_request(1, trailers) == ruvia::http2_finish_status::invalid_message);
            EXPECT_TRUE(client.submit_data(1, "abc", ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
        }
        // So is a trailer field that may not appear in request trailers.
        const std::array<ruvia::http_header_view, 1> forbidden{{{"content-length", "3"}}};
        EXPECT_TRUE(client.finish_request(1, forbidden) == ruvia::http2_finish_status::invalid_message);
        EXPECT_TRUE(client.finish_request(1, trailers) == ruvia::http2_finish_status::accepted);
        exchange_value(client, server);
        while (auto event = server.next_event()) {
            if (const auto* end = event->message_end()) {
                saw_end = true;
                EXPECT_EQ(end->trailers().size(), std::size_t{1});
                if (!end->trailers().empty()) {
                    EXPECT_EQ(end->trailers()[0].value(), "retained");
                }
            }
        }
        EXPECT_TRUE(saw_end);
    }
}

TEST(Ruvia, http2_public_push_request_lease_preserves_fields_and_releases_repeated_streams) {
    {
        auto client = ruvia::http2_connection::client({.enable_push_ = true});
        auto server = ruvia::http2_connection::server({});
        ruvia::test::output_relay relay;
        auto exchange_value = [&](auto& from, auto& to) {
            EXPECT_TRUE(relay.feed(from, to) == ruvia::http2_feed_result::accepted);
        };
        exchange_value(client, server);
        exchange_value(server, client);
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        exchange_value(client, server);
        std::optional<ruvia::http2_request_head_event> parent;
        while (auto event = server.next_event()) {
            if (auto* head = event->request_head()) {
                parent.emplace(std::move(*head));
            }
        }
        EXPECT_TRUE(parent.has_value());
        const std::array<ruvia::http_header_view, 3> headers{{{"host", "EXAMPLE.test:443"}, {"cookie", "a=1"}, {"x-push", "request"}}};
        for (unsigned repeat = 0; repeat != 100; ++repeat) {
            auto pushed = server.submit_push_request(std::get<std::uint32_t>(submitted),
                {.authority_ = "example.test", .path_ = "/asset?version=1", .headers_ = headers});
            EXPECT_TRUE((pushed.index() == 0));
            if ((pushed.index() != 0)) {
                break;
            }
            const auto request = std::get<0>(pushed).build_request();
            EXPECT_TRUE(request.scheme() == "https" && request.authority() == "example.test");
            EXPECT_TRUE(request.path() == "/asset" && request.query_string() == "version=1");
            EXPECT_TRUE(request.header("cookie") == "a=1");
            EXPECT_TRUE(request.header("host") == "EXAMPLE.test:443");
            EXPECT_TRUE(request.header("x-push") == "request");
            ruvia::http_response response;
            EXPECT_TRUE(submit_buffered_response(server, std::get<0>(pushed).stream_id(), response) == ruvia::http2_submit_status::accepted);
            exchange_value(server, client);
            unsigned promised = 0;
            unsigned ended = 0;
            while (auto event = client.next_event()) {
                if (const auto* promise = event->push_promise()) {
                    ++promised;
                    EXPECT_TRUE(promise->request_.path_ == "/asset?version=1");
                }
                if (event->message_end()) {
                    ++ended;
                }
            }
            EXPECT_TRUE(promised == 1 && ended == 1);
            EXPECT_TRUE(server.release(std::move(std::get<0>(pushed))) == ruvia::http2_server_request_release_status::released);
            exchange_value(client, server);
            while (server.next_event()) {
            }
        }
        EXPECT_TRUE(server.release(std::move(*parent)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_request_reset_keeps_events_consumable) {
    for (const bool split_input : {false, true}) {
        auto server = ruvia::http2_connection::server({});
        (void)ruvia::test::drain_output(server);
        auto wire = server_request_wire({});
        if (split_input) {
            const auto prefix_size = wire.size() - 1;
            EXPECT_TRUE(server.feed(std::string_view(wire).substr(0, prefix_size)) ==
                        ruvia::http2_feed_result::accepted);
            wire.erase(0, prefix_size);
        }
        const char cancel[] = {0, 0, 0, 8};
        append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1,
            std::string_view(cancel, sizeof(cancel)));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        EXPECT_TRUE(!server.connection_error().has_value());
        bool saw_close = false;
        std::size_t request_heads = 0;
        while (auto event = server.next_event()) {
            saw_close = saw_close || event->stream_closed() != nullptr;
            if (auto* request = event->request_head()) {
                ++request_heads;
                EXPECT_EQ(request->request_view().path_, std::string_view("/upload"));
                EXPECT_TRUE(server.release(std::move(*request)) ==
                            ruvia::http2_server_request_release_status::released);
            }
        }
        EXPECT_TRUE(saw_close);
        EXPECT_EQ(request_heads, std::size_t{1});
    }
}

TEST(Ruvia, http2_public_closed_request_lease_does_not_overflow_settings) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    const auto request_wire = server_request_wire({});
    EXPECT_TRUE(server.feed(request_wire) == ruvia::http2_feed_result::accepted);
    auto request = server.next_event();
    auto* request_head = request ? request->request_head() : nullptr;
    EXPECT_TRUE(request_head != nullptr);
    while (server.next_event()) {
    }
    std::string wire;
    const char increment[] = {0x7f, static_cast<char>(0xff), 0, 0};
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1,
        std::string_view(increment, sizeof(increment)));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    ruvia::http_response response;
    EXPECT_TRUE(submit_buffered_response(server, 1, response) ==
                ruvia::http2_submit_status::accepted);
    while (server.next_event()) {
    }
    (void)ruvia::test::drain_output(server);
    const char settings[] = {0, 4, 0, 1, 0, 0};
    wire.clear();
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
        std::string_view(settings, sizeof(settings)));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error().has_value());
    if (request_head != nullptr) {
        EXPECT_TRUE(server.release(std::move(*request_head)) ==
                    ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_server_refuses_stream_beyond_advertised_concurrency) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");

    // The server advertises SETTINGS_MAX_CONCURRENT_STREAMS = 128. One HEADERS
    // beyond that limit is a stream error (RFC 9113 5.1.2), not a connection error.
    constexpr std::uint32_t advertised_limit = 128;
    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    for (std::uint32_t index = 0; index <= advertised_limit; ++index) {
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, index * 2 + 1, block);
    }
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error().has_value());

    std::string refused;
    constexpr std::array<char, 4> refused_stream_payload{0, 0, 0, 7};
    append_frame(refused, ruvia::http2_frame_type::rst_stream, 0, advertised_limit * 2 + 1,
        std::string_view(refused_stream_payload.data(), refused_stream_payload.size()));
    EXPECT_TRUE(ruvia::test::drain_output(server).ends_with(std::string_view(refused)));

    std::size_t request_heads = 0;
    while (auto event = server.next_event()) {
        if (event->request_head() != nullptr) {
            ++request_heads;
        }
    }
    EXPECT_EQ(request_heads, std::size_t{advertised_limit});
}

TEST(Ruvia, http2_public_reports_available_client_request_streams_without_reserving) {
    auto server = ruvia::http2_connection::server({});
    EXPECT_EQ(server.available_request_streams(4), std::size_t{0});

    auto client = ruvia::http2_connection::client({});
    EXPECT_EQ(client.available_request_streams(1000), std::size_t{128});
    EXPECT_EQ(client.available_request_streams(0), std::size_t{0});
    std::string settings;
    constexpr std::array<char, 6> peer_limit{0, 3, 0, 0, 0, 2};
    append_frame(settings, ruvia::http2_frame_type::settings, 0, 0,
        std::string_view(peer_limit.data(), peer_limit.size()));
    EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);
    while (client.next_event()) {
    }
    EXPECT_EQ(client.available_request_streams(4), std::size_t{2});
    EXPECT_EQ(client.available_request_streams(1), std::size_t{1});

    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    const auto* stream_id = std::get_if<std::uint32_t>(&submitted);
    EXPECT_TRUE(stream_id != nullptr);
    EXPECT_EQ(client.available_request_streams(4), std::size_t{1});
    if (stream_id != nullptr) {
        EXPECT_TRUE(client.submit_reset(*stream_id, ruvia::http2_error_code::cancel) ==
                    ruvia::http2_submit_status::accepted);
    }
    EXPECT_EQ(client.available_request_streams(4), std::size_t{2});

    client.begin_drain();
    EXPECT_EQ(client.available_request_streams(4), std::size_t{0});
}

namespace {

bool http2_client_submits_field(std::string_view name, std::string_view value) {
    auto client = ruvia::http2_connection::client({});
    const std::array headers{ruvia::http_header_view{name, value}};
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST",
        .authority_ = "example.test",
        .target_ = "/",
        .headers_ = headers,
        .content_ = ruvia::http_request_content_plan::streaming()});
    return std::holds_alternative<std::uint32_t>(submitted);
}

bool http2_server_accepts_field(std::string_view name, std::string_view value) {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");
    ruvia::hpack_encoder::encode_header(block, name, value);
    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);

    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
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
                .method_ = "POST", .authority_ = "example.test", .headers_ = fields, .content_ = ruvia::http_request_content_plan::streaming(), .field_source_ = source});
        case 2:
            return client.submit_request_head(ruvia::http2_connect_request_head_view{
                .authority_ = "example.test:443", .headers_ = fields, .field_source_ = source});
        default:
            return client.submit_request_head(ruvia::http2_extended_connect_request_head_view{
                .protocol_ = "example", .authority_ = "example.test:443", .target_ = "/tunnel", .headers_ = fields, .field_source_ = source});
    }
}

}  // namespace

TEST(Ruvia, http2_public_client_generates_framing_and_expectation_fields_itself) {
    for (const auto& [name, value] : {std::pair<std::string_view, std::string_view>{"expect", "100-continue"},
             {"Expect", "foo=bar"}, {"content-length", "0"}, {"Content-Length", "3"}}) {
        EXPECT_TRUE(!http2_client_submits_field(name, value));
    }
}

TEST(Ruvia, http2_public_te_allows_only_trailers_with_list_ows) {
    for (const std::string_view value : {"trailers", "TRAILERS", "trailers , trailers"}) {
        EXPECT_TRUE(http2_client_submits_field("te", value));
        EXPECT_TRUE(http2_server_accepts_field("te", value));
    }
    for (const std::string_view value : {"gzip", "trailers, gzip", "trailers;q=1"}) {
        EXPECT_TRUE(!http2_client_submits_field("te", value));
        EXPECT_TRUE(!http2_server_accepts_field("te", value));
    }
}

TEST(Ruvia, http2_public_received_te_accepts_empty_lists_without_dropping_fields) {
    for (const std::string_view value : {"", ",,", ", trailers,,"}) {
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/");
        ruvia::hpack_encoder::encode_header(block, "te", value);
        ruvia::hpack_encoder::encode_header(block, "x-after", "kept");
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);

        auto server = ruvia::http2_connection::server({});
        (void)ruvia::test::drain_output(server);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = server.next_event();
        auto* request = event ? event->request_head() : nullptr;
        EXPECT_TRUE(request != nullptr);
        if (request == nullptr) {
            continue;
        }
        const auto built_req = request->build_request();
        EXPECT_TRUE(built_req.header("te") == std::optional<std::string_view>{value});
        EXPECT_TRUE(built_req.header("x-after") == std::string_view("kept"));
        const auto end = server.next_event();
        EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
        EXPECT_TRUE(!server.connection_error());
        ruvia::http_response response;
        EXPECT_TRUE(submit_buffered_response(server, 1, response) == ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.release(std::move(*request)) ==
                    ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_normalized_request_capacity_accepts_many_fields_within_byte_budget) {
    for (const std::size_t fields : {std::size_t{63}, std::size_t{80}}) {
        for (const bool explicit_host : {false, true}) {
            for (const unsigned cookies : {0U, 1U, 2U}) {
                std::string wire(ruvia::http2_client_preface);
                append_peer_settings(wire);
                std::string block;
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
                auto server = ruvia::http2_connection::server({});
                EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
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
                            const auto built_req = head->build_request();
                            EXPECT_EQ(built_req.headers().size(), normalized);
                            EXPECT_EQ(built_req.header("Host").value_or(""),
                                explicit_host ? "EXAMPLE.test:443" : "example.test");
                            EXPECT_EQ(built_req.query_string(), "x=kept");
                            if (cookies != 0) {
                                EXPECT_EQ(built_req.header("Cookie").value_or(""),
                                    cookies == 1 ? "a=1" : "a=1; b=2");
                            }
                        } else {
                            ++following_heads;
                            EXPECT_TRUE(id == 3 && head->request_view().path_ == "/next");
                        }
                        ruvia::http_response reply;
                        EXPECT_TRUE(submit_buffered_response(server, id, reply) == ruvia::http2_submit_status::accepted);
                        EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
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
                EXPECT_TRUE(first_heads == 1U);
                EXPECT_TRUE(first_ends == 1U);
                EXPECT_TRUE(resets == 0U);
                EXPECT_TRUE(following_heads == 1 && following_ends == 1 && !server.connection_error());
            }
        }
    }
}

TEST(Ruvia, http2_public_forwards_received_application_values_through_hpack) {
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
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "/");
        ruvia::hpack_encoder::encode_header(block, field.name(), field.value());
        std::string wire(ruvia::http2_client_preface);
        append_peer_settings(wire);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        auto incoming = ruvia::http2_connection::server({});
        EXPECT_TRUE(incoming.feed(wire) == ruvia::http2_feed_result::accepted);
        auto event = incoming.next_event();
        auto* received_head = event ? event->request_head() : nullptr;
        EXPECT_TRUE(received_head != nullptr);
        if (!received_head) {
            continue;
        }
        const auto received_request = received_head->build_request();
        const auto received = received_request.header(field.name());
        EXPECT_TRUE(received == std::optional<std::string_view>{field.value()});
        if (!received) {
            continue;
        }
        const std::array fields{ruvia::http_header_view{field.name(), *received}};
        for (unsigned entry = 0; entry != 4; ++entry) {
            ruvia::test::output_relay relay;
            auto client = ruvia::http2_connection::client({});
            auto upstream = ruvia::http2_connection::server({});
            relay.transfer(client, upstream);
            relay.transfer(upstream, client);
            relay.transfer(client, upstream);
            const auto generated = submit_field_request(client, entry, fields, ruvia::http_request_field_source::generated);
            EXPECT_TRUE(std::get_if<ruvia::http2_request_head_submit_error>(&generated) != nullptr);
            EXPECT_TRUE(!client.wants_write());
            const auto forwarded = submit_field_request(client, entry, fields, ruvia::http_request_field_source::forwarded);
            const auto* id = std::get_if<std::uint32_t>(&forwarded);
            EXPECT_TRUE(id != nullptr);
            if (!id) {
                continue;
            }
            EXPECT_EQ(*id, std::uint32_t{1});
            relay.transfer(client, upstream);
            auto forwarded_event = upstream.next_event();
            auto* forwarded_head = forwarded_event ? forwarded_event->request_head() : nullptr;
            EXPECT_TRUE(forwarded_head != nullptr);
            if (forwarded_head) {
                EXPECT_EQ(forwarded_head->stream_id(), *id);
                const auto request = forwarded_head->build_request();
                EXPECT_TRUE(request.header(field.name()) == received);
            }
            EXPECT_TRUE(!upstream.connection_error());
        }
    }
}

TEST(Ruvia, http2_public_forwarding_never_relaxes_owned_hop_by_hop_or_singleton_fields) {
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
        ruvia::test::output_relay relay;
        auto client = ruvia::http2_connection::client();
        auto server = ruvia::http2_connection::server();
        relay.transfer(client, server);
        relay.transfer(server, client);
        relay.transfer(client, server);
        const auto reject = [&](std::span<const ruvia::http_header_view> fields,
                                ruvia::http_request_field_source source) {
            const auto result = submit_field_request(client, entry, fields, source);
            const auto* error = std::get_if<ruvia::http2_request_head_submit_error>(&result);
            EXPECT_TRUE(error != nullptr);
            if (error) {
                EXPECT_TRUE(*error == ruvia::http2_request_head_submit_error::invalid_message);
            }
            EXPECT_TRUE(!client.wants_write());
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
        EXPECT_TRUE(id && *id == 1);
        relay.transfer(client, server);
        auto event = server.next_event();
        EXPECT_TRUE(event && event->request_head());
    }
}

TEST(Ruvia, http2_public_forwarded_options_content_keeps_missing_media_type) {
    for (const auto content : {ruvia::http_request_content_plan::known_length(1),
             ruvia::http_request_content_plan::streaming(1), ruvia::http_request_content_plan::streaming()}) {
        const auto incoming_wire = server_request_wire("x", "OPTIONS");
        auto incoming = ruvia::http2_connection::server({});
        EXPECT_TRUE(incoming.feed(incoming_wire) == ruvia::http2_feed_result::accepted);
        auto received = incoming.next_event();
        EXPECT_TRUE(received && received->request_head());
        std::string body;
        while (auto next = incoming.next_event()) {
            if (const auto* chunk = next->message_body_chunk()) {
                body.append(chunk->bytes());
            }
        }
        EXPECT_EQ(body, "x");
        ruvia::test::output_relay relay;
        auto client = ruvia::http2_connection::client({});
        auto upstream = ruvia::http2_connection::server({});
        relay.transfer(client, upstream);
        relay.transfer(upstream, client);
        relay.transfer(client, upstream);
        ruvia::http2_regular_request_head_view request{
            .method_ = "OPTIONS", .authority_ = "example.test", .content_ = content};
        const auto generated = client.submit_request_head(request);
        EXPECT_TRUE(std::get_if<ruvia::http2_request_head_submit_error>(&generated) != nullptr);
        request.method_ = "TRACE";
        request.field_source_ = ruvia::http_request_field_source::forwarded;
        const auto trace = client.submit_request_head(request);
        EXPECT_TRUE(std::get_if<ruvia::http2_request_head_submit_error>(&trace) != nullptr);
        EXPECT_TRUE(!client.wants_write());
        request.method_ = "OPTIONS";
        const auto forwarded = client.submit_request_head(request);
        const auto* id = std::get_if<std::uint32_t>(&forwarded);
        EXPECT_TRUE(id && *id == 1);
        if (!id) {
            continue;
        }
        EXPECT_TRUE(client.submit_data(*id, body, ruvia::http2_end_stream::end_stream, 65535).status_ ==
                    ruvia::http2_data_submit_status::complete);
        relay.transfer(client, upstream);
        auto event = upstream.next_event();
        auto* head = event ? event->request_head() : nullptr;
        EXPECT_TRUE(head != nullptr);
        if (head) {
            const auto forwarded_request = head->build_request();
            EXPECT_TRUE(!forwarded_request.header("content-type"));
        }
        bool ended = false;
        std::string forwarded_body;
        while (auto next = upstream.next_event()) {
            if (const auto* chunk = next->message_body_chunk()) {
                forwarded_body.append(chunk->bytes());
            }
            ended |= next->message_end() != nullptr;
        }
        EXPECT_TRUE(ended);
        EXPECT_EQ(forwarded_body, body);
        EXPECT_TRUE(!upstream.connection_error());
    }
}

TEST(Ruvia, http2_public_trace_zero_length_accepts_header_or_empty_data_endings) {
    for (const bool head_end : {false, true}) {
        auto wire = server_request_wire({}, "TRACE", {}, head_end);
        if (!head_end) {
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, {});
        }
        auto server = ruvia::http2_connection::server({});
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = server.next_event();
        auto* request = head ? head->request_head() : nullptr;
        EXPECT_TRUE(request != nullptr);
        if (!request) {
            continue;
        }
        const auto built_req = request->build_request();
        EXPECT_EQ(built_req.method(), "TRACE");
        EXPECT_EQ(built_req.header("Content-Length").value_or(""), "0");
        const auto end = server.next_event();
        EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
        EXPECT_TRUE(!server.next_event());
        ruvia::http_response reply;
        EXPECT_TRUE(submit_buffered_response(server, 1, reply) == ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_known_empty_requests_never_wait_for_continue_or_send_content) {
    for (const std::string_view method : {"GET", "OPTIONS", "TRACE"}) {
        for (const bool streaming : {false, true}) {
            auto client = ruvia::http2_connection::client({});
            std::string wire(ruvia::test::drain_output(client));
            ruvia::http2_regular_request_head_view head{
                .method_ = method, .authority_ = "example.test", .content_ = streaming ? ruvia::http_request_content_plan::streaming(0) : ruvia::http_request_content_plan::known_length(0), .expectation_ = ruvia::http_client_request_expectation::continue_value};
            const auto rejected = client.submit_request_head(head);
            EXPECT_TRUE(std::holds_alternative<ruvia::http2_request_head_submit_error>(rejected) &&
                        std::get<ruvia::http2_request_head_submit_error>(rejected) == ruvia::http2_request_head_submit_error::invalid_message);
            EXPECT_TRUE(!client.wants_write());
            head.expectation_ = ruvia::http_client_request_expectation::none;
            const auto submitted = client.submit_request_head(head);
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted) && std::get<std::uint32_t>(submitted) == 1);
            if (!std::holds_alternative<std::uint32_t>(submitted)) {
                continue;
            }
            // A known empty head ended the stream: DATA is an ordering error.
            // A streaming zero stays open, and content beyond it is a value.
            if (streaming) {
                const auto overrun = client.submit_data(1, "x", ruvia::http2_end_stream::keep_open, 65535);
                EXPECT_TRUE(overrun.status_ == ruvia::http2_data_submit_status::invalid_message && overrun.consumed_bytes_ == 0);
            } else {
                EXPECT_TRUE(ruvia::test::throws_on(
                    [&] { (void)client.submit_data(1, "x", ruvia::http2_end_stream::keep_open, 65535); }));
            }
            if (streaming) {
                EXPECT_TRUE(client.submit_data(1, {}, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
            }
            wire.append(ruvia::test::drain_output(client));
            auto server = ruvia::http2_connection::server({});
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto request_event = server.next_event();
            auto* request = request_event ? request_event->request_head() : nullptr;
            EXPECT_TRUE(request != nullptr);
            if (!request) {
                continue;
            }
            const auto built_req = request->build_request();
            EXPECT_EQ(built_req.method(), method);
            EXPECT_TRUE(!built_req.header("Content-Type"));
            EXPECT_TRUE(!built_req.header("Expect"));
            EXPECT_TRUE(built_req.header("Content-Length") ==
                        (method == "TRACE" ? std::nullopt : std::optional<std::string_view>{"0"}));
            const auto end = server.next_event();
            EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
            EXPECT_TRUE(!server.next_event());
            ruvia::http_response reply;
            EXPECT_TRUE(submit_buffered_response(server, 1, reply) == ruvia::http2_submit_status::accepted);
            EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
        }
    }
}

TEST(Ruvia, http2_public_options_without_content_type_preserves_body_and_following_stream) {
    for (const std::string_view body : {"", "abc"}) {
        for (const bool declared_length : {false, true}) {
            std::string wire(ruvia::http2_client_preface);
            append_peer_settings(wire);
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "OPTIONS");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
            ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
            ruvia::hpack_encoder::encode_header(block, ":path", "*");
            if (declared_length) {
                char length_buffer[20];
                const auto length_end = std::to_chars(length_buffer, length_buffer + sizeof(length_buffer), body.size()).ptr;
                const std::string_view length(length_buffer, length_end);
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
            auto server = ruvia::http2_connection::server({});
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto first = server.next_event();
            auto* options = first ? first->request_head() : nullptr;
            EXPECT_TRUE(options != nullptr);
            if (options == nullptr) {
                continue;
            }
            const auto built_options = options->build_request();
            EXPECT_EQ(built_options.method(), "OPTIONS");
            EXPECT_EQ(built_options.path(), "*");
            EXPECT_TRUE(!built_options.header("Content-Type"));
            if (!body.empty()) {
                auto data = server.next_event();
                auto* chunk = data ? data->message_body_chunk() : nullptr;
                EXPECT_TRUE(chunk && chunk->stream_id() == 1 && chunk->bytes() == body);
                if (chunk) {
                    (void)chunk->take_credit();
                }
            }
            const auto end = server.next_event();
            EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
            auto next = server.next_event();
            auto* following = next ? next->request_head() : nullptr;
            EXPECT_TRUE(following && following->stream_id() == 3 && following->request_view().path_ == "/next");
            const auto next_end = server.next_event();
            EXPECT_TRUE(next_end && next_end->message_end() && next_end->message_end()->stream_id() == 3);
            EXPECT_TRUE(!server.connection_error());
            for (auto* request : {options, following}) {
                if (request) {
                    ruvia::http_response reply;
                    EXPECT_TRUE(submit_buffered_response(server, request->stream_id(), reply) == ruvia::http2_submit_status::accepted);
                    EXPECT_TRUE(server.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
                }
            }
        }
    }
}

TEST(Ruvia, http2_public_opaque_metadata_preserves_request_and_response_bodies) {
    for (const bool server_role : {true, false}) {
        for (const std::string_view field_name : {"content-type", "content-encoding",
                 "origin", "access-control-request-method", "access-control-request-headers", "trailer"}) {
            for (const std::string_view value : {"", "not a media type", "text/plain; charset=\"unfinished"}) {
                std::string block;
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
                std::string wire;
                if (server_role) {
                    wire.append(ruvia::http2_client_preface);
                }
                append_peer_settings(wire);
                append_header_section(wire, 1, block, false, true);
                append_frame(wire, ruvia::http2_frame_type::data, 0x1, 1, "abc");
                auto connection = server_role ? ruvia::http2_connection::server({})
                                              : ruvia::http2_connection::client({});
                if (!server_role) {
                    const auto submitted = connection.submit_request_head(ruvia::http2_regular_request_head_view{
                        .authority_ = "example.test"});
                    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
                }
                EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
                auto head = connection.next_event();
                auto* request = head ? head->request_head() : nullptr;
                const auto* response = head ? head->response_head() : nullptr;
                EXPECT_TRUE(server_role ? request != nullptr : response != nullptr);
                if (server_role ? request == nullptr : response == nullptr) {
                    continue;
                }
                if (request != nullptr) {
                    const auto built_req = request->build_request();
                    EXPECT_TRUE(built_req.header(field_name) == std::optional<std::string_view>{value});
                    EXPECT_TRUE(built_req.header("x-after") == std::string_view("kept"));
                } else {
                    const auto fields = response->head().headers();
                    const auto metadata = std::find_if(fields.begin(), fields.end(), [field_name](const auto& field) {
                        return field.name() == field_name;
                    });
                    EXPECT_TRUE(metadata != fields.end() && metadata->value() == value);
                    const auto after = std::find_if(fields.begin(), fields.end(), [](const auto& field) {
                        return field.name() == "x-after";
                    });
                    EXPECT_TRUE(after != fields.end() && after->value() == "kept");
                }
                auto body = connection.next_event();
                auto* chunk = body ? body->message_body_chunk() : nullptr;
                EXPECT_TRUE(chunk != nullptr);
                if (chunk != nullptr) {
                    EXPECT_EQ(chunk->bytes(), "abc");
                    (void)chunk->take_credit();
                }
                const auto end = connection.next_event();
                EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == 1);
                EXPECT_TRUE(!connection.connection_error());
                if (request != nullptr) {
                    ruvia::http_response reply;
                    EXPECT_TRUE(submit_buffered_response(connection, 1, reply) == ruvia::http2_submit_status::accepted);
                    EXPECT_TRUE(connection.release(std::move(*request)) == ruvia::http2_server_request_release_status::released);
                }
            }
        }
    }
}

TEST(Ruvia, http2_public_sender_rejects_invalid_request_metadata) {
    for (const auto& [name, value] :
        {std::pair<std::string_view, std::string_view>{"origin", "https://app.example/path"},
            {"access-control-request-method", "POST GET"}, {"access-control-request-headers", "x bad"},
            {"trailer", "Content-Length"}, {"trailer", "bad field"}, {"trailer", "\"x-name\""}}) {
        EXPECT_TRUE(!http2_client_submits_field(name, value));
    }
    EXPECT_TRUE(http2_client_submits_field("origin", "https://app.example"));
    EXPECT_TRUE(http2_server_accepts_field("origin", "https://app.example"));
}

TEST(Ruvia, http2_public_completed_response_preserves_request_send_half_and_retires_credits) {
    for (const bool reset : {false, true}) {
        auto client = ruvia::http2_connection::client({});
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming(3)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        const auto id = std::get<std::uint32_t>(request);
        auto wire = client_response_wire("ok");
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
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
        EXPECT_EQ(received, "ok");
        EXPECT_TRUE(ended);
        if (!reset) {
            credit.reset();
        }
        EXPECT_TRUE(client.submit_data(id, "abc", ruvia::http2_end_stream::keep_open, 65535).status_ ==
                    ruvia::http2_data_submit_status::complete);
        if (reset) {
            EXPECT_TRUE(client.submit_reset(id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
        } else {
            EXPECT_TRUE(client.finish_request(id) == ruvia::http2_finish_status::accepted);
        }
        credit.reset();
        while (client.next_event()) {
        }
        EXPECT_TRUE(!client.connection_error());
    }
}

TEST(Ruvia, http2_public_completed_response_keeps_unconsumed_request_suffix_retryable) {
    auto client = ruvia::http2_connection::client({});
    const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::known_length(65536)});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
    if (!std::holds_alternative<std::uint32_t>(request)) {
        return;
    }
    const auto id = std::get<std::uint32_t>(request);
    const std::string payload(65536, 'b');
    const auto submitted = client.submit_data(id, payload, ruvia::http2_end_stream::end_stream, payload.size());
    EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::backpressured);
    EXPECT_EQ(submitted.consumed_bytes_, std::size_t{65535});
    std::string output;
    ruvia::test::drain_output(client, output);
    auto wire = client_response_wire({});
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    bool ended = false;
    while (auto event = client.next_event()) {
        ended |= event->message_end() != nullptr;
    }
    EXPECT_TRUE(ended);
    EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
    (void)ruvia::test::drain_output(client);
    wire.clear();
    const std::string_view increment("\0\0\0\1", 4);
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, increment);
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, id, increment);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.wants_write());
    const auto retried = client.submit_data(id, std::string_view(payload).substr(submitted.consumed_bytes_),
        ruvia::http2_end_stream::end_stream, 1);
    EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete);
    EXPECT_EQ(retried.consumed_bytes_, std::size_t{1});
    EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{1});
    const auto final_output = ruvia::test::drain_output(client);
    const auto frame = ruvia::parse_http2_frame_header(final_output);
    EXPECT_TRUE(frame && frame->length_ == 1 && frame->stream_id_ == id && (frame->flags_ & 1U) != 0);
    EXPECT_EQ(final_output.size(), ruvia::http2_frame_header_bytes + 1);
}

TEST(Ruvia, http2_public_data_consumption_is_bounded_and_does_not_retain_input_suffixes) {
    for (const std::uint32_t window : {0U, 2U, 65535U}) {
        for (const std::size_t allowance : {std::size_t{0}, std::size_t{1}, std::size_t{6}}) {
            auto client = ruvia::http2_connection::client({});
            std::string wire;
            const std::array<char, 6> setting{0, 4, static_cast<char>(window >> 24),
                static_cast<char>(window >> 16), static_cast<char>(window >> 8), static_cast<char>(window)};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view(setting.data(), setting.size()));
            EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
            const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
                .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(6)});
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
            if (!std::holds_alternative<std::uint32_t>(request)) {
                continue;
            }
            const auto id = std::get<std::uint32_t>(request);
            (void)ruvia::test::drain_output(client);
            const auto expected_count = std::min({std::size_t{6}, static_cast<std::size_t>(window), allowance});
            ruvia::http2_data_submit_result submitted{};
            {
                std::string input = "abcdef";
                submitted = client.submit_data(id, input, ruvia::http2_end_stream::end_stream, allowance);
                input.assign(input.size(), 'x');
            }
            EXPECT_EQ(submitted.consumed_bytes_, expected_count);
            EXPECT_TRUE(submitted.status_ == (expected_count == 6 ? ruvia::http2_data_submit_status::complete
                                                                  : ruvia::http2_data_submit_status::backpressured));
            EXPECT_EQ(client.pending_data_output_bytes(id), expected_count);
            auto output = ruvia::test::drain_output(client);
            std::string expected(std::string_view("abcdef").substr(0, expected_count));
            if (expected_count != 6) {
                wire.clear();
                append_frame(wire, ruvia::http2_frame_type::window_update, 0, id, std::string_view("\0\0\0\6", 4));
                EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
                EXPECT_TRUE(!client.wants_write());
                EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
                const std::string replacement(6 - expected_count, 'z');
                const auto retried = client.submit_data(id, replacement, ruvia::http2_end_stream::end_stream, replacement.size());
                EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete);
                EXPECT_EQ(retried.consumed_bytes_, replacement.size());
                expected.append(replacement);
                output.append(ruvia::test::drain_output(client));
            }
            std::string received;
            unsigned terminal_frames = 0;
            std::string_view remaining(output);
            while (!remaining.empty()) {
                const auto frame = ruvia::parse_http2_frame_header(remaining);
                EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
                if (!frame || remaining.size() < ruvia::http2_frame_header_bytes + frame->length_) {
                    break;
                }
                received.append(remaining.substr(ruvia::http2_frame_header_bytes, frame->length_));
                terminal_frames += (frame->flags_ & 1U) != 0;
                if ((frame->flags_ & 1U) != 0) {
                    EXPECT_EQ(received.size(), std::size_t{6});
                }
                remaining.remove_prefix(ruvia::http2_frame_header_bytes + frame->length_);
            }
            EXPECT_TRUE(remaining.empty());
            EXPECT_EQ(received, expected);
            EXPECT_EQ(terminal_frames, 1U);
            EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        }
    }
}

// RFC 9113 §6.9: even an empty DATA frame waits for positive connection and
// stream windows. An empty terminal submission or finish without trailers is
// backpressured without changing state until both windows open.
TEST(Ruvia, http2_public_empty_end_stream_waits_for_both_send_windows) {
    for (const bool finish : {false, true}) {
        auto client = ruvia::http2_connection::client();
        const auto filled_request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(65535)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(filled_request));
        if (!std::holds_alternative<std::uint32_t>(filled_request)) {
            return;
        }
        const std::string filling(65535, 'f');
        const auto filled = client.submit_data(std::get<std::uint32_t>(filled_request), filling,
            ruvia::http2_end_stream::end_stream, filling.size());
        EXPECT_TRUE(filled.status_ == ruvia::http2_data_submit_status::complete);
        EXPECT_EQ(filled.consumed_bytes_, filling.size());
        std::string wire;
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\0", 6));
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::streaming(0)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::test::drain_output(client);
        const auto end_stream = [&](bool sent) {
            if (finish) {
                EXPECT_TRUE(client.finish_request(id) ==
                            (sent ? ruvia::http2_finish_status::accepted : ruvia::http2_finish_status::backpressured));
                return;
            }
            const auto result = client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0);
            EXPECT_EQ(result.consumed_bytes_, std::size_t{0});
            EXPECT_TRUE(result.status_ ==
                        (sent ? ruvia::http2_data_submit_status::complete : ruvia::http2_data_submit_status::backpressured));
        };
        const auto credit = [&](std::uint32_t stream_id) {
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::window_update, 0, stream_id, std::string_view("\0\0\0\1", 4));
            EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        };
        for (const std::uint32_t opened : {0U, id}) {
            const auto window = client.send_window_state(id);
            EXPECT_TRUE(window && (window->connection_window_ == 0 || window->stream_window_ == 0));
            end_stream(false);
            EXPECT_TRUE(!client.wants_write());
            EXPECT_TRUE(client.send_window_state(id).has_value());
            credit(opened);
        }
        end_stream(true);
        const auto output = ruvia::test::drain_output(client);
        const auto frame = ruvia::parse_http2_frame_header(output);
        EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
                    frame->length_ == 0 && frame->stream_id_ == id && (frame->flags_ & 1U) != 0);
        EXPECT_EQ(output.size(), ruvia::http2_frame_header_bytes);
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            (void)client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0);
        }));
        EXPECT_TRUE(!client.wants_write());
    }
}

TEST(Ruvia, http2_public_data_validates_terminal_length_before_partial_consumption) {
    for (const std::size_t allowance : {std::size_t{0}, std::size_t{2}, std::size_t{8}}) {
        auto client = ruvia::http2_connection::client();
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(4)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            continue;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::test::drain_output(client);
        const auto before = client.send_window_state(id);
        for (const std::string_view invalid : {"abc", "abcde"}) {
            const auto rejected = client.submit_data(id, invalid, ruvia::http2_end_stream::end_stream, allowance);
            EXPECT_TRUE(rejected.status_ == ruvia::http2_data_submit_status::invalid_message && rejected.consumed_bytes_ == 0);
            EXPECT_TRUE(!client.wants_write());
            EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
            const auto after = client.send_window_state(id);
            EXPECT_TRUE(before && after && before->connection_window_ == after->connection_window_ &&
                        before->stream_window_ == after->stream_window_);
        }
        const auto first = client.submit_data(id, "abcd", ruvia::http2_end_stream::end_stream, 2);
        EXPECT_TRUE(first.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(first.consumed_bytes_, std::size_t{2});
        EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{2});
        auto output = ruvia::test::drain_output(client);
        EXPECT_TRUE(client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 0).status_ ==
                    ruvia::http2_data_submit_status::invalid_message);
        EXPECT_TRUE(client.submit_data(id, "cde", ruvia::http2_end_stream::end_stream, 0).status_ ==
                    ruvia::http2_data_submit_status::invalid_message);
        EXPECT_TRUE(!client.wants_write());
        const auto second = client.submit_data(id, "cd", ruvia::http2_end_stream::end_stream, 1);
        EXPECT_TRUE(second.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(second.consumed_bytes_, std::size_t{1});
        const auto last = client.submit_data(id, "d", ruvia::http2_end_stream::end_stream, 1);
        EXPECT_TRUE(last.status_ == ruvia::http2_data_submit_status::complete);
        EXPECT_EQ(last.consumed_bytes_, std::size_t{1});
        output.append(ruvia::test::drain_output(client));
        std::string_view remaining(output);
        for (const auto payload : {std::string_view("ab"), std::string_view("c"), std::string_view("d")}) {
            const auto frame = ruvia::parse_http2_frame_header(remaining);
            EXPECT_TRUE(frame && frame->length_ == payload.size() && ((frame->flags_ & 1U) != 0) == (payload == "d"));
            if (!frame || remaining.size() < ruvia::http2_frame_header_bytes + frame->length_) {
                break;
            }
            EXPECT_TRUE(remaining.substr(ruvia::http2_frame_header_bytes, frame->length_) == payload);
            remaining.remove_prefix(ruvia::http2_frame_header_bytes + frame->length_);
        }
        EXPECT_TRUE(remaining.empty());
    }
}

TEST(Ruvia, http2_public_response_data_counts_only_consumed_bytes_toward_content_length) {
    for (const bool buffered : {false, true}) {
        auto server = ruvia::http2_connection::server();
        const auto wire = server_request_wire({}, "GET");
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto request = server.next_event();
        auto* head = request ? request->request_head() : nullptr;
        EXPECT_TRUE(head != nullptr);
        if (head == nullptr) {
            continue;
        }
        EXPECT_TRUE(server.next_event()->message_end() != nullptr);
        ruvia::http_response response;
        if (buffered) {
            response.body("abcd");
            EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
                server.submit_response_head(1, response,
                    ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response))));
        } else {
            response.header("content-length", "4");
            EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
                server.submit_streaming_response_head(1, std::move(response))));
        }
        (void)ruvia::test::drain_output(server);
        for (const std::string_view invalid : {"abc", "abcde"}) {
            EXPECT_TRUE(server.submit_data(1, invalid, ruvia::http2_end_stream::end_stream, 2).status_ ==
                        ruvia::http2_data_submit_status::invalid_message);
            EXPECT_TRUE(!server.wants_write());
            EXPECT_TRUE(server.send_window_state(1)->available_ == 65535);
        }
        const auto first = server.submit_data(1, "abcd", ruvia::http2_end_stream::end_stream, 2);
        EXPECT_TRUE(first.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(first.consumed_bytes_, std::size_t{2});
        EXPECT_EQ(server.pending_data_output_bytes(1), std::size_t{2});
        const auto prefix = ruvia::test::drain_output(server);
        const auto prefix_frame = ruvia::parse_http2_frame_header(prefix);
        EXPECT_TRUE(prefix_frame && prefix_frame->length_ == 2 && (prefix_frame->flags_ & 1U) == 0);
        EXPECT_TRUE(std::string_view(prefix).substr(ruvia::http2_frame_header_bytes) == "ab");
        const auto trailers = ruvia::validate_http_response_trailers(std::span<const ruvia::http_header_view>{});
        EXPECT_TRUE(server.finish_response(1, trailers) == ruvia::http2_finish_status::invalid_message);
        EXPECT_TRUE(!server.wants_write());
        const auto final = server.submit_data(1, "cd", ruvia::http2_end_stream::end_stream, 2);
        EXPECT_TRUE(final.status_ == ruvia::http2_data_submit_status::complete);
        EXPECT_EQ(final.consumed_bytes_, std::size_t{2});
        EXPECT_EQ(server.pending_data_output_bytes(1), std::size_t{2});
        const auto suffix = ruvia::test::drain_output(server);
        const auto suffix_frame = ruvia::parse_http2_frame_header(suffix);
        EXPECT_TRUE(suffix_frame && suffix_frame->length_ == 2 && (suffix_frame->flags_ & 1U) != 0);
        EXPECT_TRUE(std::string_view(suffix).substr(ruvia::http2_frame_header_bytes) == "cd");
        EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_expectation_gate_does_not_consume_or_retain_data) {
    for (const bool timeout_release : {false, true}) {
        auto client = ruvia::http2_connection::client();
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(4), .expectation_ = ruvia::http_client_request_expectation::continue_value});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            continue;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::test::drain_output(client);
        {
            const std::string input = "body";
            const auto pending = client.submit_data(id, input, ruvia::http2_end_stream::end_stream, input.size());
            EXPECT_TRUE(pending.status_ == ruvia::http2_data_submit_status::expectation_pending);
            EXPECT_EQ(pending.consumed_bytes_, std::size_t{0});
        }
        EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        EXPECT_TRUE(!client.wants_write());
        if (timeout_release) {
            (void)client.release_request_content(id);
        } else {
            std::string wire;
            append_peer_settings(wire);
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":status", "100");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x4, id, block);
            EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
            while (client.next_event()) {
            }
            (void)ruvia::test::drain_output(client);
        }
        EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        const auto retried = client.submit_data(id, "next", ruvia::http2_end_stream::end_stream, 4);
        EXPECT_TRUE(retried.status_ == ruvia::http2_data_submit_status::complete);
        EXPECT_EQ(retried.consumed_bytes_, std::size_t{4});
        const auto output = ruvia::test::drain_output(client);
        const auto frame = ruvia::parse_http2_frame_header(output);
        EXPECT_TRUE(frame && frame->length_ == 4 && (frame->flags_ & 1U) != 0);
        EXPECT_TRUE(std::string_view(output).substr(ruvia::http2_frame_header_bytes) == "next");
    }
}

TEST(Ruvia, http2_public_reset_and_goaway_discard_no_caller_owned_suffix) {
    for (const bool goaway : {false, true}) {
        auto client = ruvia::http2_connection::client();
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(4)});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            continue;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::test::drain_output(client);
        const auto submitted = client.submit_data(id, "body", ruvia::http2_end_stream::end_stream, 2);
        EXPECT_TRUE(submitted.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(submitted.consumed_bytes_, std::size_t{2});
        (void)ruvia::test::drain_output(client);
        std::string wire;
        append_peer_settings(wire);
        if (goaway) {
            append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, std::string_view("\0\0\0\0\0\0\0\0", 8));
        } else {
            append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, id, std::string_view("\0\0\0\10", 4));
        }
        EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        bool closed = false;
        while (auto event = client.next_event()) {
            closed |= goaway ? event->request_unprocessed() != nullptr : event->stream_closed() != nullptr;
        }
        EXPECT_TRUE(closed);
        EXPECT_TRUE(client.stream_aborted(id));
        (void)ruvia::test::drain_output(client);
        const auto retry = client.submit_data(id, "dy", ruvia::http2_end_stream::end_stream, 2);
        EXPECT_TRUE(retry.status_ == ruvia::http2_data_submit_status::closed);
        EXPECT_EQ(retry.consumed_bytes_, std::size_t{0});
        EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        EXPECT_TRUE(!client.wants_write());
    }
}

// Window-clamped DATA is serialized completely within each submission, so
// streams keep their submission order and both send windows stay exact.
TEST(Ruvia, http2_public_window_clamped_data_preserves_stream_order_and_windows) {
    {
        auto client = ruvia::http2_connection::client({});
        std::string wire;
        const auto set_window = [&](std::uint32_t size) {
            wire.clear();
            const std::array<char, 6> setting{0, 4, static_cast<char>(size >> 24),
                static_cast<char>(size >> 16), static_cast<char>(size >> 8), static_cast<char>(size)};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view(setting.data(), setting.size()));
            EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
        };
        set_window(0);
        const std::array bodies{std::string(4097, 'a'), std::string(1024, 'b'), std::string(8192, 'd')};
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
                .method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::known_length(bodies[i].size())});
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
            const auto id = static_cast<std::uint32_t>(2 * i + 1);
            const auto blocked = client.submit_data(id, bodies[i], ruvia::http2_end_stream::end_stream, bodies[i].size());
            EXPECT_TRUE(blocked.status_ == ruvia::http2_data_submit_status::backpressured);
            EXPECT_EQ(blocked.consumed_bytes_, std::size_t{0});
        }
        {
            std::string discarded;
            ruvia::test::drain_output(client, discarded);
        }
        std::vector<std::uint32_t> frame_order;
        std::array<std::string, 3> received;
        std::array<unsigned, 3> terminal_frames{};
        const auto collect = [&] {
            const auto output = ruvia::test::drain_output(client);
            std::string_view remaining(output);
            while (!remaining.empty()) {
                const auto frame = ruvia::parse_http2_frame_header(std::span(remaining.data(), remaining.size()));
                EXPECT_TRUE(frame && remaining.size() >= ruvia::http2_frame_header_bytes + frame->length_);
                if (!frame || remaining.size() < ruvia::http2_frame_header_bytes + frame->length_) {
                    break;
                }
                if (frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    const auto index = (frame->stream_id_ - 1) / 2;
                    EXPECT_TRUE(index < received.size());
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
        EXPECT_TRUE(frame_order.empty());
        std::array<std::size_t, 3> consumed{};
        const auto first = client.submit_data(1, bodies[0], ruvia::http2_end_stream::end_stream, 4096);
        consumed[0] = first.consumed_bytes_;
        EXPECT_TRUE(first.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(consumed[0], std::size_t{4096});
        for (std::size_t i = 1; i < bodies.size(); ++i) {
            const auto result = client.submit_data(static_cast<std::uint32_t>(2 * i + 1),
                bodies[i], ruvia::http2_end_stream::end_stream, 4096);
            consumed[i] = result.consumed_bytes_;
            EXPECT_TRUE(result.status_ == (i == 1 ? ruvia::http2_data_submit_status::complete
                                                  : ruvia::http2_data_submit_status::backpressured));
        }
        collect();
        EXPECT_EQ(consumed[1], std::size_t{1024});
        EXPECT_EQ(consumed[2], std::size_t{4096});
        set_window(8192);
        collect();
        EXPECT_TRUE((frame_order == std::vector<std::uint32_t>{1, 3, 5}));
        for (const std::size_t i : {std::size_t{0}, std::size_t{2}}) {
            const auto suffix = std::string_view(bodies[i]).substr(consumed[i]);
            const auto result = client.submit_data(static_cast<std::uint32_t>(2 * i + 1),
                suffix, ruvia::http2_end_stream::end_stream, suffix.size());
            EXPECT_TRUE(result.status_ == ruvia::http2_data_submit_status::complete);
            EXPECT_EQ(result.consumed_bytes_, suffix.size());
            consumed[i] += result.consumed_bytes_;
        }
        collect();
        EXPECT_TRUE((frame_order == std::vector<std::uint32_t>{1, 3, 5, 1, 5}));
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const auto id = static_cast<std::uint32_t>(2 * i + 1);
            EXPECT_TRUE(received[i] == bodies[i]);
            EXPECT_EQ(terminal_frames[i], 1U);
            EXPECT_EQ(consumed[i], bodies[i].size());
            const auto window = client.send_window_state(id);
            EXPECT_TRUE(window && window->connection_window_ == 52222 &&
                        window->stream_window_ == static_cast<std::int32_t>(8192 - bodies[i].size()));
        }
    }
}

TEST(Ruvia, http2_output_observer_can_submit_new_data_without_losing_it) {
    auto client = ruvia::http2_connection::client({});
    const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http_request_content_plan::streaming()});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
    if (!std::holds_alternative<std::uint32_t>(request)) {
        return;
    }
    const auto id = std::get<std::uint32_t>(request);
    (void)ruvia::test::drain_output(client);
    EXPECT_TRUE(client.submit_data(id, "first", ruvia::http2_end_stream::keep_open, 65535).status_ ==
                ruvia::http2_data_submit_status::complete);
    struct observation final {
        ruvia::http2_connection* client_;
        std::optional<ruvia::http2_data_submit_result> submitted_;
        bool failed_{};
    } observed{&client};
    std::string first;
    const auto batch = client.take_output_batch(16384, first, [](void* context, std::uint32_t stream, std::size_t) noexcept {
            auto& state = *static_cast<observation*>(context);
            try {
                state.submitted_ = state.client_->submit_data(
                    stream, "second", ruvia::http2_end_stream::end_stream, 65535);
            } catch (...) {
                state.failed_ = true;
            } }, &observed);
    EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
    EXPECT_TRUE(!observed.failed_);
    EXPECT_TRUE(observed.submitted_ && observed.submitted_->status_ == ruvia::http2_data_submit_status::complete);
    EXPECT_TRUE(observed.submitted_ && observed.submitted_->consumed_bytes_ == 6);
    EXPECT_TRUE(client.wants_write());
    std::string second;
    ruvia::test::drain_output(client, second);
    const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(second.data(), second.size()));
    EXPECT_TRUE(frame && frame->stream_id_ == id && frame->length_ == 6);
    if (frame) {
        EXPECT_TRUE(std::string_view(second).substr(ruvia::http2_frame_header_bytes) == "second");
    }
}

TEST(Ruvia, http2_output_observer_can_recursively_drain_and_append_frames) {
    auto client = ruvia::http2_connection::client({});
    const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http_request_content_plan::streaming()});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
    if (!std::holds_alternative<std::uint32_t>(request)) {
        return;
    }
    const auto id = std::get<std::uint32_t>(request);
    (void)ruvia::test::drain_output(client);
    for (const auto payload : {"a", "b", "c", "d"}) {
        EXPECT_TRUE(client.submit_data(id, payload, ruvia::http2_end_stream::keep_open, 65535).status_ ==
                    ruvia::http2_data_submit_status::complete);
    }
    struct observation final {
        ruvia::http2_connection* client_;
        std::string* nested_;
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
    std::string nested;
    observation observed{&client, &nested};
    std::string first;
    const auto batch = client.take_output_batch(20, first, &observation::on_data, &observed);
    EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
    EXPECT_TRUE(!observed.failed_);
    EXPECT_EQ(observed.calls_, std::size_t{4});
    EXPECT_EQ(first.size(), std::size_t{20});
    EXPECT_EQ(nested.size(), std::size_t{20});
    std::string remaining;
    ruvia::test::drain_output(client, remaining);
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
    EXPECT_EQ(offset, remaining.size());
    EXPECT_TRUE(payload == std::string(40, 'x') + "z");
}

TEST(Ruvia, http2_public_full_output_batch_transfers_storage_and_accounts_data) {
    {
        auto client = ruvia::http2_connection::client({});
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http_request_content_plan::streaming()});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        const auto id = std::get<std::uint32_t>(request);
        (void)ruvia::test::drain_output(client);
        const std::string body(8192, 'b');
        EXPECT_TRUE(client.submit_data(id, body, ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
        std::string output;
        data_output_observations observed;
        ruvia::http2_output_batch_result batch;
        bool threw = false;
        try {
            batch = client.take_output_batch(16384, output, &data_output_observations::observe, &observed);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        EXPECT_TRUE(!threw);
        if (threw) {
            return;
        }
        EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
        EXPECT_EQ(batch.bytes_, body.size() + ruvia::http2_frame_header_bytes);
        const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
        EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
                    frame->stream_id_ == id && frame->length_ == body.size() && frame->flags_ == 0);
        if (frame) {
            EXPECT_TRUE(std::string_view(output).substr(ruvia::http2_frame_header_bytes) == body);
        }
        EXPECT_TRUE(!client.wants_write());
        EXPECT_EQ(client.pending_data_output_bytes(id), std::size_t{0});
        EXPECT_EQ(observed.count_, std::size_t{1});
        EXPECT_TRUE(observed.entries_[0].stream_id_ == id && observed.entries_[0].payload_bytes_ == body.size());

        output.clear();
        EXPECT_TRUE(client.submit_data(id, {}, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
        batch = client.take_output_batch(0, output, &data_output_observations::observe, &observed);
        EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
        EXPECT_EQ(batch.bytes_, ruvia::http2_frame_header_bytes);
        const auto empty_end = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
        EXPECT_TRUE(empty_end && empty_end->stream_id_ == id && empty_end->length_ == 0 && empty_end->flags_ == 1);
        EXPECT_EQ(observed.count_, std::size_t{2});
        EXPECT_TRUE(observed.entries_[1].stream_id_ == id && observed.entries_[1].payload_bytes_ == 0);

        const auto next = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/next", .content_ = ruvia::http_request_content_plan::streaming()});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(next));
        if (!std::holds_alternative<std::uint32_t>(next)) {
            return;
        }
        const auto next_id = std::get<std::uint32_t>(next);
        (void)ruvia::test::drain_output(client);
        EXPECT_TRUE(client.submit_data(next_id, "tail", ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
        batch = client.take_output_batch(16384, output, &data_output_observations::observe, &observed);
        EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
        EXPECT_EQ(batch.bytes_, ruvia::http2_frame_header_bytes + 4);
        EXPECT_EQ(output.size(), 2 * ruvia::http2_frame_header_bytes + 4);
        if (output.size() >= ruvia::http2_frame_header_bytes) {
            const auto tail = ruvia::parse_http2_frame_header(
                std::span<const char>(output.data(), output.size()).subspan(ruvia::http2_frame_header_bytes));
            EXPECT_TRUE(tail && tail->stream_id_ == next_id && tail->length_ == 4 && tail->flags_ == 1);
            if (tail) {
                EXPECT_TRUE(std::string_view(output).substr(2 * ruvia::http2_frame_header_bytes) == "tail");
            }
        }
        EXPECT_EQ(observed.count_, std::size_t{3});
        EXPECT_TRUE(observed.entries_[2].stream_id_ == next_id && observed.entries_[2].payload_bytes_ == 4);
        EXPECT_TRUE(!observed.overflow_);
        EXPECT_TRUE(client.take_output_batch(16384, output, &data_output_observations::observe, &observed).status_ == ruvia::http2_output_batch_status::empty);
        EXPECT_EQ(observed.count_, std::size_t{3});
    }
}

TEST(Ruvia, http2_public_output_tail_preserves_storage_transfer_and_data_observations) {
    {
        auto client = ruvia::http2_connection::client({});
        (void)ruvia::test::drain_output(client);
        const std::string padding(4096, 'p');
        const std::array fields{ruvia::http_header_view{"x-padding", padding}};
        std::array<std::uint32_t, 2> ids{};
        for (std::size_t index = 0; index < 64; ++index) {
            const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
                .method_ = "POST", .authority_ = "example.test", .headers_ = fields, .content_ = ruvia::http_request_content_plan::streaming()});
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
            if (!std::holds_alternative<std::uint32_t>(request)) {
                return;
            }
            if (index < ids.size()) {
                ids[index] = std::get<std::uint32_t>(request);
            }
        }
        const std::array<std::string_view, 2> bodies{"left", "right"};
        for (std::size_t index = 0; index < ids.size(); ++index) {
            EXPECT_TRUE(client.submit_data(ids[index], bodies[index], ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
        }
        std::string prefix;
        data_output_observations observed;
        const auto first = client.take_output_batch(160 * 1024, prefix, &data_output_observations::observe, &observed);
        EXPECT_TRUE(first.status_ == ruvia::http2_output_batch_status::taken);
        EXPECT_TRUE(client.wants_write());
        EXPECT_EQ(observed.count_, std::size_t{0});

        std::string tail;
        bool threw = false;
        try {
            const auto last = client.take_output_batch((std::numeric_limits<std::size_t>::max)(), tail, &data_output_observations::observe, &observed);
            EXPECT_TRUE(last.status_ == ruvia::http2_output_batch_status::taken);
            EXPECT_EQ(last.bytes_, tail.size());
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        EXPECT_TRUE(!threw);
        EXPECT_TRUE(!client.wants_write());
        EXPECT_EQ(observed.count_, ids.size());
        for (std::size_t index = 0; index < ids.size(); ++index) {
            EXPECT_EQ(client.pending_data_output_bytes(ids[index]), std::size_t{0});
            EXPECT_TRUE(observed.entries_[index].stream_id_ == ids[index] &&
                        observed.entries_[index].payload_bytes_ == bodies[index].size());
        }
        std::string_view pending(tail);
        std::size_t data_index = 0;
        while (!pending.empty()) {
            const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(pending.data(), pending.size()));
            EXPECT_TRUE(frame && frame->length_ <= pending.size() - ruvia::http2_frame_header_bytes);
            if (!frame || frame->length_ > pending.size() - ruvia::http2_frame_header_bytes) {
                break;
            }
            const auto payload = pending.substr(ruvia::http2_frame_header_bytes, frame->length_);
            if (frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                EXPECT_TRUE(data_index < ids.size());
                if (data_index < ids.size()) {
                    EXPECT_EQ(frame->stream_id_, ids[data_index]);
                    EXPECT_EQ(payload, bodies[data_index]);
                }
                ++data_index;
            }
            pending.remove_prefix(ruvia::http2_frame_header_bytes + frame->length_);
        }
        EXPECT_EQ(data_index, ids.size());
    }
}

TEST(Ruvia, http2_public_output_batch_copy_failure_preserves_frame_boundaries) {
    auto client = ruvia::http2_connection::client({});
    std::array<std::uint32_t, 3> ids{};
    const std::array<std::string, 3> bodies{std::string(32, 'a'), std::string(16, 'b'), std::string(8, 'c')};
    for (auto& id : ids) {
        const auto request = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/batch", .content_ = ruvia::http_request_content_plan::streaming()});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(request));
        if (!std::holds_alternative<std::uint32_t>(request)) {
            return;
        }
        id = std::get<std::uint32_t>(request);
    }
    (void)ruvia::test::drain_output(client);
    for (std::size_t index = 0; index < ids.size(); ++index) {
        EXPECT_TRUE(client.submit_data(ids[index], bodies[index], ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
    }
    data_output_observations observed;
    std::string copied = "prefix";
    const auto first_bytes = ruvia::http2_frame_header_bytes + bodies[0].size();
    auto batch = client.take_output_batch(first_bytes, copied, &data_output_observations::observe, &observed);
    EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
    EXPECT_EQ(batch.bytes_, first_bytes);
    EXPECT_TRUE(std::string_view(copied).starts_with("prefix"));
    if (copied.size() >= 6) {
        const auto first = ruvia::parse_http2_frame_header(std::span<const char>(copied.data(), copied.size()).subspan(6));
        EXPECT_TRUE(first && first->stream_id_ == ids[0] && first->length_ == bodies[0].size());
        if (first) {
            EXPECT_TRUE(std::string_view(copied).substr(6 + ruvia::http2_frame_header_bytes) == bodies[0]);
        }
    }
    EXPECT_EQ(client.pending_data_output_bytes(ids[0]), std::size_t{0});
    EXPECT_EQ(client.pending_data_output_bytes(ids[1]), bodies[1].size());
    EXPECT_EQ(observed.count_, std::size_t{1});
    EXPECT_TRUE(observed.entries_[0].stream_id_ == ids[0] && observed.entries_[0].payload_bytes_ == bodies[0].size());

    // A unit budget still moves exactly one whole frame per batch.
    std::string suffix;
    batch = client.take_output_batch(1, suffix, &data_output_observations::observe, &observed);
    EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
    EXPECT_EQ(batch.bytes_, ruvia::http2_frame_header_bytes + bodies[1].size());
    EXPECT_EQ(client.pending_data_output_bytes(ids[1]), std::size_t{0});
    EXPECT_EQ(observed.count_, std::size_t{2});
    suffix.clear();
    batch = client.take_output_batch(1, suffix, &data_output_observations::observe, &observed);
    EXPECT_TRUE(batch.status_ == ruvia::http2_output_batch_status::taken);
    EXPECT_EQ(batch.bytes_, ruvia::http2_frame_header_bytes + bodies[2].size());
    const auto last = ruvia::parse_http2_frame_header(std::span<const char>(suffix.data(), suffix.size()));
    EXPECT_TRUE(last && last->stream_id_ == ids[2] && last->length_ == bodies[2].size());
    if (last) {
        EXPECT_TRUE(std::string_view(suffix).substr(ruvia::http2_frame_header_bytes) == bodies[2]);
    }
    EXPECT_TRUE(!client.wants_write());
    EXPECT_EQ(observed.count_, std::size_t{3});
    EXPECT_TRUE(observed.entries_[1].stream_id_ == ids[1] && observed.entries_[1].payload_bytes_ == bodies[1].size());
    EXPECT_TRUE(observed.entries_[2].stream_id_ == ids[2] && observed.entries_[2].payload_bytes_ == bodies[2].size());
    EXPECT_TRUE(!observed.overflow_);
}

TEST(Ruvia, http2_public_websocket_handshake_validates_required_subprotocols_and_configures_deflate) {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);

    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);

    std::string block1;
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

    std::string block3;
    ruvia::hpack_encoder::encode_header(block3, ":method", "CONNECT");
    ruvia::hpack_encoder::encode_header(block3, ":protocol", "websocket");
    ruvia::hpack_encoder::encode_header(block3, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block3, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block3, ":path", "/ws");
    ruvia::hpack_encoder::encode_header(block3, "sec-websocket-version", "13");
    ruvia::hpack_encoder::encode_header(block3, "sec-websocket-extensions", "permessage-deflate");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 3, block3);

    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto event1 = server.next_event();
    auto* head1 = event1 ? event1->request_head() : nullptr;
    EXPECT_TRUE(head1 != nullptr);
    if (head1 == nullptr) {
        return;
    }

    const auto built_req1 = head1->build_request();
    constexpr std::array<std::string_view, 1> unmatched{"chat.v2"};
    const auto rejected = ruvia::validate_http2_websocket_handshake(server, 1, built_req1,
        {.supported_subprotocols_ = unmatched, .require_subprotocol_ = true});
    EXPECT_TRUE(rejected.accepted() == nullptr);
    EXPECT_TRUE(rejected.failure() != nullptr);
    if (const auto* failure = rejected.failure()) {
        EXPECT_EQ(failure->protocol_error().status(), ruvia::http_status::bad_request);
    }

    const auto optional_subprotocol = ruvia::validate_http2_websocket_handshake(server, 1, built_req1,
        {.supported_subprotocols_ = unmatched, .require_subprotocol_ = false});
    EXPECT_TRUE(optional_subprotocol.accepted() != nullptr);

    constexpr std::array<std::string_view, 2> supported{"chat.v2", "chat.v1"};
    const auto accepted1 = ruvia::validate_http2_websocket_handshake(server, 1, built_req1,
        {.supported_subprotocols_ = supported, .require_subprotocol_ = true});
    EXPECT_TRUE(accepted1.accepted() != nullptr);

    const auto submitted1 = server.submit_websocket_handshake(1, built_req1, accepted1,
        {
            .supported_subprotocols_ = supported,
            .deflate_ = {.enabled_ = true, .context_takeover_ = true},
        });
    EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted1));
    if (const auto* negotiation = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted1)) {
        EXPECT_EQ(negotiation->subprotocol(), std::string_view("chat.v1"));
        EXPECT_TRUE(negotiation->compression().enabled_);
        EXPECT_TRUE(!negotiation->compression().server_no_context_takeover_);
        EXPECT_TRUE(negotiation->compression().client_no_context_takeover_);
    }
    EXPECT_TRUE(server.release(std::move(*head1)) == ruvia::http2_server_request_release_status::released);

    auto event3 = server.next_event();
    auto* head3 = event3 ? event3->request_head() : nullptr;
    EXPECT_TRUE(head3 != nullptr);
    if (head3 == nullptr) {
        return;
    }

    const auto built_req3 = head3->build_request();
    const auto rejected3 = ruvia::validate_http2_websocket_handshake(server, 3, built_req3,
        {.supported_subprotocols_ = supported, .require_subprotocol_ = true});
    EXPECT_TRUE(rejected3.accepted() == nullptr);
    EXPECT_TRUE(rejected3.failure() != nullptr);

    const auto accepted3 = ruvia::validate_http2_websocket_handshake(server, 3, built_req3);
    EXPECT_TRUE(accepted3.accepted() != nullptr);
    const auto submitted3 = server.submit_websocket_handshake(3, built_req3, accepted3,
        {
            .deflate_ = {.enabled_ = false},
        });
    EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted3));
    if (const auto* negotiation = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted3)) {
        EXPECT_TRUE(negotiation->subprotocol().empty());
        EXPECT_TRUE(!negotiation->compression().enabled_);
    }
    EXPECT_TRUE(server.release(std::move(*head3)) == ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_websocket_handshake_ignores_rfc6455_key_exchange_fields) {
    // RFC 8441 section 5: :protocol supersedes Sec-WebSocket-Key/-Accept, which
    // a recipient does not process; it neither validates nor rejects them.
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);

    std::string wire(ruvia::http2_client_preface);
    append_peer_settings(wire);
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> extra{{
        {"sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ=="},
        {"sec-websocket-accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="},
        {"sec-websocket-protocol", "chat, chat"},
    }};
    std::uint32_t stream_id = 1;
    for (const auto& [name, value] : extra) {
        std::string block;
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
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

    constexpr std::array<std::string_view, 1> supported{"chat"};
    for (std::size_t i = 0; i < extra.size(); ++i) {
        auto event = server.next_event();
        auto* head = event ? event->request_head() : nullptr;
        EXPECT_TRUE(head != nullptr);
        if (head == nullptr) {
            return;
        }
        const auto id = head->stream_id();
        const auto request = head->build_request();
        const auto validation = ruvia::validate_http2_websocket_handshake(server, id, request,
            {.supported_subprotocols_ = supported, .require_subprotocol_ = i == 2});
        EXPECT_TRUE(validation.accepted() != nullptr);
        const auto submitted =
            server.submit_websocket_handshake(id, request, validation, {.supported_subprotocols_ = supported});
        EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(submitted));
        if (const auto* negotiation = std::get_if<ruvia::websocket_extended_connect_negotiation>(&submitted)) {
            EXPECT_EQ(negotiation->subprotocol(), std::string_view(i == 2 ? "chat" : ""));
        }
        EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    }
}

TEST(Ruvia, http2_public_outbound_request_headers_and_trailers_lowercase_standard_tokens) {
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    ruvia::test::output_relay relay;
    auto exchange_value = [&](auto& from, auto& to) {
        EXPECT_TRUE(relay.feed(from, to) == ruvia::http2_feed_result::accepted);
    };
    exchange_value(client, server);
    exchange_value(server, client);

    const std::string long_mixed_name = "X-Mixed-" + std::string(80, 'N');
    const std::string long_lower_name = "x-mixed-" + std::string(80, 'n');
    const std::string last_upper_name = std::string(62, 'a') + "Z";
    const std::string first_upper_name = "Z" + std::string(63, 'b');
    const std::string middle_upper_name = std::string(32, 'c') + "Z" + std::string(32, 'c');
    const std::string last_lower_name = std::string(62, 'a') + "z";
    const std::string first_lower_name = "z" + std::string(63, 'b');
    const std::string middle_lower_name = std::string(32, 'c') + "z" + std::string(32, 'c');
    const std::array headers{ruvia::http_header_view{long_mixed_name, "head-value"},
        ruvia::http_header_view{"X-Custom-Head", "short"},
        ruvia::http_header_view{last_upper_name, "last-upper"},
        ruvia::http_header_view{first_upper_name, "first-upper"},
        ruvia::http_header_view{middle_upper_name, "middle-upper"}};
    const std::array normalized{ruvia::http_header_view{long_lower_name, "head-value"},
        ruvia::http_header_view{"x-custom-head", "short"},
        ruvia::http_header_view{last_lower_name, "last-upper"},
        ruvia::http_header_view{first_lower_name, "first-upper"},
        ruvia::http_header_view{middle_lower_name, "middle-upper"}};
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .target_ = "/mixed", .headers_ = headers, .content_ = ruvia::http_request_content_plan::streaming()});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    const std::array<ruvia::http_header_view, 2> trailers{
        {{"X-Mixed-Trailer", "tail-value"}, {"X-Second-Trailer", "tail-two"}}};
    EXPECT_TRUE(client.finish_request(1, trailers) == ruvia::http2_finish_status::accepted);
    exchange_value(client, server);

    std::optional<ruvia::http2_request_head_event> parent;
    while (auto event = server.next_event()) {
        if (auto* head = event->request_head()) {
            const auto built = head->build_request();
            for (const auto& field : normalized) {
                EXPECT_EQ(built.header(field.name()).value_or(""), field.value());
            }
            parent.emplace(std::move(*head));
        } else if (const auto* end = event->message_end()) {
            EXPECT_EQ(end->trailers().size(), std::size_t{2});
            if (end->trailers().size() == 2) {
                EXPECT_EQ(end->trailers()[0].name(), "x-mixed-trailer");
                EXPECT_EQ(end->trailers()[0].value(), "tail-value");
                EXPECT_EQ(end->trailers()[1].name(), "x-second-trailer");
                EXPECT_EQ(end->trailers()[1].value(), "tail-two");
            }
        }
    }
    EXPECT_TRUE(parent.has_value());
    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/pushed", .headers_ = headers});
    EXPECT_TRUE(pushed.index() == 0);
    if (pushed.index() == 0) {
        const auto built_push = std::get<0>(pushed).build_request();
        for (const auto& field : normalized) {
            EXPECT_EQ(built_push.header(field.name()).value_or(""), field.value());
        }
        (void)server.release(std::move(std::get<0>(pushed)));
    }
    exchange_value(server, client);
    auto promise = client.next_event();
    EXPECT_TRUE(promise && promise->push_promise());
    if (promise && promise->push_promise()) {
        const auto& fields = promise->push_promise()->request_.headers_;
        EXPECT_EQ(fields.size(), normalized.size());
        if (fields.size() == normalized.size()) {
            for (std::size_t i = 0; i < fields.size(); ++i) {
                EXPECT_EQ(fields[i].name(), normalized[i].name());
                EXPECT_EQ(fields[i].value(), normalized[i].value());
            }
        }
    }
    // Consume the abandoned push's terminal event before the next input batch.
    while (client.next_event()) {
    }
    if (parent) {
        ruvia::http_response response;
        response.status(ruvia::http_status::no_content);
        for (const auto& field : headers) {
            response.header(field.name(), field.value());
        }
        const auto result = server.submit_response_head(parent->stream_id(), response,
            ruvia::plan_buffered_http_response_write(ruvia::http_known_method::post, response));
        EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(result));
        (void)server.release(std::move(*parent));
        exchange_value(server, client);
        bool received_response = false;
        while (auto event = client.next_event()) {
            if (const auto* head = event->response_head()) {
                EXPECT_TRUE(head->head().status() == ruvia::http_status::no_content);
                for (const auto& field : normalized) {
                    EXPECT_EQ(head->head().header(field.name()).value_or(""), field.value());
                }
                received_response = true;
            }
        }
        EXPECT_TRUE(received_response);
    }
}

TEST(Ruvia, http2_public_rejected_connect_with_invalid_terminal_length_resets_without_data_frame) {
    auto client = ruvia::http2_connection::client({});
    const auto submitted = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    if (!std::holds_alternative<std::uint32_t>(submitted)) {
        return;
    }
    const auto stream_id = std::get<std::uint32_t>(submitted);
    (void)ruvia::test::drain_output(client);

    std::string wire;
    append_peer_settings(wire);
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":status", "403");
    ruvia::hpack_encoder::encode_header(block, "content-length", "5");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, stream_id, block);

    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
    const auto closed = client.next_event();
    EXPECT_TRUE(closed && closed->stream_closed());
    if (closed && closed->stream_closed()) {
        EXPECT_EQ(closed->stream_closed()->stream_id(), stream_id);
        EXPECT_TRUE(closed->stream_closed()->error() == ruvia::http2_error_code::protocol_error);
    }

    const auto out = ruvia::test::drain_output(client);
    // Peer SETTINGS generates a 9-byte SETTINGS ACK, followed only by the 13-byte RST_STREAM
    // (no spurious 9-byte DATA(END_STREAM) frame may be emitted before RST_STREAM).
    EXPECT_EQ(out.size(), ruvia::http2_frame_header_bytes + ruvia::http2_frame_header_bytes + 4);
    if (out.size() >= ruvia::http2_frame_header_bytes * 2 + 4) {
        const auto rst = ruvia::parse_http2_frame_header(
            std::span(out.data() + ruvia::http2_frame_header_bytes, ruvia::http2_frame_header_bytes));
        EXPECT_TRUE(rst.has_value());
        if (rst) {
            EXPECT_EQ(rst->type_, static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
            EXPECT_EQ(rst->stream_id_, stream_id);
            EXPECT_EQ(rst->length_, std::uint32_t{4});
        }
    }
}

// H2-02: Client receiving RST_STREAM(REFUSED_STREAM) after a 1xx informational
// response treats it as a stream reset rather than an unprocessed request.
TEST(Ruvia, http2_public_client_refused_stream_after_informational_is_not_unprocessed) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto sub = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(sub));
    const auto sid = std::get<std::uint32_t>(sub);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> sreq;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            sreq.emplace(std::move(*rh));
        }
    }
    EXPECT_TRUE(server.submit_interim_response_head(
                    sid, ruvia::http_interim_response_head(ruvia::http_status::early_hints)) ==
                ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    bool saw_1xx = false;
    while (auto ev = client.next_event()) {
        if (ev->informational_head()) {
            saw_1xx = true;
        }
    }
    EXPECT_TRUE(saw_1xx);

    std::string rst;
    constexpr std::array<char, 4> refused_payload{0, 0, 0, 7};
    append_frame(rst, ruvia::http2_frame_type::rst_stream, 0, sid,
        std::string_view(refused_payload.data(), refused_payload.size()));
    EXPECT_TRUE(client.feed(rst) == ruvia::http2_feed_result::accepted);
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
    EXPECT_TRUE(!saw_unprocessed);
    EXPECT_TRUE(saw_closed);
    if (sreq) {
        (void)server.release(std::move(*sreq));
    }
}

// CONNECT 成功时保留但忽略长度；拒绝响应仍按普通 HTTP 正文分帧处理。
TEST(Ruvia, http2_public_client_connect_response_ignores_success_length_and_validates_rejection) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto sub1 = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    const auto sid1 = std::get<std::uint32_t>(sub1);
    const auto sub2 = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    const auto sid2 = std::get<std::uint32_t>(sub2);
    (void)ruvia::test::drain_output(client);

    // Valid Content-Length on 200 CONNECT is retained in response_head.headers()
    std::string rblock1;
    ruvia::hpack_encoder::encode_header(rblock1, ":status", "200");
    ruvia::hpack_encoder::encode_header(rblock1, "content-length", "5");
    std::string rwire1;
    append_frame(rwire1, ruvia::http2_frame_type::headers, 0x4, sid1, rblock1);
    EXPECT_TRUE(client.feed(rwire1) == ruvia::http2_feed_result::accepted);
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
    EXPECT_TRUE(has_cl);
    EXPECT_TRUE(!client.stream_aborted(sid1));

    // 非 2xx CONNECT 的非法 Content-Length 仍触发 PROTOCOL_ERROR。
    std::string rblock2;
    ruvia::hpack_encoder::encode_header(rblock2, ":status", "400");
    ruvia::hpack_encoder::encode_header(rblock2, "content-length", "not-a-number");
    std::string rwire2;
    append_frame(rwire2, ruvia::http2_frame_type::headers, 0x4, sid2, rblock2);
    EXPECT_TRUE(client.feed(rwire2) == ruvia::http2_feed_result::accepted);
    while (client.next_event()) {
    }
    EXPECT_TRUE(client.stream_aborted(sid2));
}

TEST(Ruvia, http2_public_asterisk_form_request_never_generates_authority) {
    auto client = ruvia::http2_connection::client({});
    (void)ruvia::test::drain_output(client);
    const auto rejected = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "OPTIONS", .authority_ = "example.test", .target_ = "*"});
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_request_head_submit_error>(rejected) &&
                std::get<ruvia::http2_request_head_submit_error>(rejected) ==
                    ruvia::http2_request_head_submit_error::invalid_message);
    EXPECT_TRUE(!client.wants_write());
    const auto accepted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.method_ = "OPTIONS", .target_ = "*"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(accepted));
}

TEST(Ruvia, http2_public_built_request_exposes_extended_protocol) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({});
    auto server = ruvia::http2_connection::server({});
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
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
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
        EXPECT_TRUE(request_head_event.has_value());
        if (!request_head_event) {
            return;
        }
        const auto request = request_head_event->build_request();
        EXPECT_EQ(request.protocol(), protocol);
        // RFC 8441/9298: :protocol selects the tunnel protocol, so a WebSocket
        // CONNECT must not validate as CONNECT-UDP.
        EXPECT_EQ(ruvia::validate_http_connect_udp_request(request).has_value(),
            protocol == "websocket");
        (void)server.release(std::move(*request_head_event));
    }
}

TEST(Ruvia, http2_public_late_responses_after_many_client_cancellations_keep_connection) {
    auto client = ruvia::http2_connection::client({});
    std::string settings;
    append_peer_settings(settings);
    EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);
    (void)ruvia::test::drain_output(client);

    std::uint32_t first_canceled_stream = 0;
    for (std::size_t index = 0; index != 2100; ++index) {
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/canceled"});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        if (index == 0) {
            first_canceled_stream = stream_id;
        }
        EXPECT_TRUE(client.submit_reset(stream_id, ruvia::http2_error_code::cancel) ==
                    ruvia::http2_submit_status::accepted);
        (void)ruvia::test::drain_output(client);
    }

    const auto active = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/active"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(active));
    if (!std::holds_alternative<std::uint32_t>(active)) {
        return;
    }
    const auto active_stream = std::get<std::uint32_t>(active);
    (void)ruvia::test::drain_output(client);

    std::string response_block;
    ruvia::hpack_encoder::encode_header(response_block, ":status", "200");
    std::string late_head;
    append_frame(
        late_head, ruvia::http2_frame_type::headers, 0x4, first_canceled_stream, response_block);
    const auto late_head_result = client.feed(late_head);
    EXPECT_TRUE(late_head_result == ruvia::http2_feed_result::accepted);
    if (late_head_result != ruvia::http2_feed_result::accepted) {
        return;
    }
    EXPECT_TRUE(!client.next_event());

    std::string late_data;
    append_frame(late_data, ruvia::http2_frame_type::data, 0x1, first_canceled_stream, "late");
    EXPECT_TRUE(client.feed(late_data) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.next_event());
    EXPECT_TRUE(!client.connection_error());

    std::string active_response;
    append_frame(active_response, ruvia::http2_frame_type::headers, 0x5, active_stream, response_block);
    EXPECT_TRUE(client.feed(active_response) == ruvia::http2_feed_result::accepted);
    const auto head = client.next_event();
    EXPECT_TRUE(head && head->response_head() && head->response_head()->stream_id() == active_stream);
    const auto end = client.next_event();
    EXPECT_TRUE(end && end->message_end() && end->message_end()->stream_id() == active_stream);
    EXPECT_TRUE(!client.connection_error());
}

// A promised stream refused while decoding its PUSH_PROMISE is already reset, so
// submit_reset(PROTOCOL_ERROR) does not emit a duplicate RST_STREAM.
TEST(Ruvia, http2_public_rejected_push_promise_does_not_emit_duplicate_late_rst_stream) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto push_server = ruvia::http2_connection::server({});
    relay.transfer(client, push_server);
    relay.transfer(push_server, client);
    relay.transfer(client, push_server);
    const auto sub = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(sub));
    (void)ruvia::test::drain_output(client);

    // An unsafe method (POST) cannot be pushed (RFC 9113 §8.4).
    std::string post_push;
    ruvia::hpack_encoder::encode_header(post_push, ":method", "POST");
    ruvia::hpack_encoder::encode_header(post_push, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(post_push, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(post_push, ":path", "/upload");
    std::string post_payload("\x00\x00\x00\x02", 4);
    post_payload.append(post_push);
    std::string post_wire;
    append_frame(post_wire, ruvia::http2_frame_type::push_promise, 0x4, 1, post_payload);
    EXPECT_TRUE(client.feed(post_wire) == ruvia::http2_feed_result::accepted);
    EXPECT_EQ(ruvia::test::drain_output(client).size(), ruvia::http2_frame_header_bytes + 4);
    EXPECT_TRUE(client.submit_reset(2, ruvia::http2_error_code::protocol_error) ==
                ruvia::http2_submit_status::closed);
    EXPECT_TRUE(ruvia::test::drain_output(client).empty());
}

// RFC 9110 §9.3.6：成功 CONNECT 的长度字段仅保留为元数据，不能阻止进入隧道。
TEST(Ruvia, http2_public_successful_connect_ignores_content_length_semantics) {
    for (const bool extended : {false, true}) {
        for (const std::string_view length : {"", "0", "42", "not-a-number", "18446744073709551616", "1, 2"}) {
            auto client = ruvia::http2_connection::client({});
            (void)ruvia::test::drain_output(client);
            constexpr std::array<char, 6> enable_connect{0, 8, 0, 0, 0, 1};
            std::string settings;
            append_frame(settings, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view(enable_connect.data(), enable_connect.size()));
            EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);

            const auto submitted = extended
                                       ? client.submit_request_head(ruvia::http2_extended_connect_request_head_view{
                                             .protocol_ = "example", .authority_ = "example.test", .target_ = "/tunnel"})
                                       : client.submit_request_head(ruvia::http2_connect_request_head_view{
                                             .authority_ = "example.test:443"});
            EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
            if (!std::holds_alternative<std::uint32_t>(submitted)) {
                continue;
            }
            const auto stream_id = std::get<std::uint32_t>(submitted);
            (void)ruvia::test::drain_output(client);

            std::string block;
            ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
            ruvia::hpack_encoder::encode_header(block, "content-length", length);
            std::string wire;
            append_frame(wire, ruvia::http2_frame_type::headers, 0x4, stream_id, block);
            append_frame(wire, ruvia::http2_frame_type::data, 0x1, stream_id, "tunnel bytes");
            EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);
            bool received_head = false;
            bool received_data = false;
            bool received_end = false;
            while (auto event = client.next_event()) {
                EXPECT_TRUE(event->stream_closed() == nullptr);
                if (const auto* head = event->response_head()) {
                    received_head = true;
                    const auto headers = head->head().headers();
                    EXPECT_EQ(headers.size(), std::size_t{1});
                    if (!headers.empty()) {
                        EXPECT_EQ(headers.front().name(), std::string_view("content-length"));
                        EXPECT_EQ(headers.front().value(), length);
                    }
                } else if (const auto* data = event->tunnel_data()) {
                    received_data = true;
                    EXPECT_EQ(data->bytes(), std::string_view("tunnel bytes"));
                } else if (event->tunnel_end() != nullptr) {
                    received_end = true;
                }
            }
            EXPECT_TRUE(received_head);
            EXPECT_TRUE(received_data);
            EXPECT_TRUE(received_end);
            EXPECT_TRUE(client.submit_reset(stream_id, ruvia::http2_error_code::cancel) ==
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

// Server-side CONNECT data before the accept/reject decision, graceful drain
// round trip, SETTINGS_ENABLE_CONNECT_PROTOCOL from a client, and extension frames
// on an established tunnel (RFC 9113 sections 5.5, 6.8, 8.5; RFC 8441 section 3;
// RFC 9298 section 5 as updated by RFC 9931).

constexpr std::uint8_t end_stream_flag = 0x1;
constexpr std::uint8_t end_headers_flag = 0x4;

void connect_drain_append_frame(std::string& wire, std::uint8_t type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()),
            static_cast<ruvia::http2_frame_type>(type), flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

void connect_drain_append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload = {}) {
    connect_drain_append_frame(wire, static_cast<std::uint8_t>(type), flags, stream_id, payload);
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
    return parse_frames(ruvia::test::drain_output(connection));
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

std::string connect_block(std::string_view protocol) {
    std::string block;
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

std::string get_block() {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/");
    return block;
}

// The peer's preface and SETTINGS, plus its ACK of ours so that the advertised
// 1 MiB stream window is in force (RFC 9113 §6.5.3).
std::string opening_wire() {
    std::string wire(ruvia::http2_client_preface);
    connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0x1, 0, {});
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

ruvia::http2_connection connect_drain_started_server() {
    auto server = ruvia::http2_connection::server({});
    (void)ruvia::test::drain_output(server);
    return server;
}

}  // namespace

// RFC 9113 section 8.5 makes every DATA after a CONNECT request's HEADERS tunnel
// data, and RFC 9298 section 5 lets an HTTP/2 client send it before the 2xx.
TEST(Ruvia, http2_connect_data_before_accept_is_held_and_delivered_as_tunnel_data) {
    for (const std::string_view protocol : {std::string_view{}, std::string_view{"connect-udp"}}) {
        auto server = connect_drain_started_server();
        auto wire = opening_wire();
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block(protocol));
        const std::string_view capsule("\x00\x05\x00ping", 7);
        connect_drain_append_frame(wire, ruvia::http2_frame_type::data, 0, 1, capsule);
        // Enough held bytes to cross the half-window WINDOW_UPDATE threshold.
        const std::string bulk(16384, 'e');
        std::string expected(capsule);
        for (int frame = 0; frame != 33; ++frame) {
            connect_drain_append_frame(wire, ruvia::http2_frame_type::data, 0, 1, bulk);
            expected.append(bulk);
        }
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);

        std::optional<ruvia::http2_request_head_event> head;
        std::optional<ruvia::http2_received_data_credit> credit;
        std::string received;
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                EXPECT_TRUE(request->snapshot().connect_pending_);
                EXPECT_FALSE(request->snapshot().initial_end_stream_);
                head.emplace(std::move(*request));
            } else if (auto* data = event->tunnel_data()) {
                EXPECT_EQ(data->stream_id(), 1U);
                received.append(data->bytes());
                if (credit) {
                    credit->merge(data->take_credit());
                } else {
                    credit.emplace(data->take_credit());
                }
            } else {
                EXPECT_TRUE(event->stream_closed() == nullptr);
            }
        }
        EXPECT_TRUE(head.has_value());
        EXPECT_TRUE(received == expected);
        EXPECT_TRUE(credit.has_value() && credit->valid());
        if (!head || !credit) {
            return;
        }
        EXPECT_TRUE(!server.stream_aborted(1));
        EXPECT_TRUE(!server.connection_error().has_value());
        const auto before_accept = take_frames(server);
        EXPECT_TRUE(!has_frame(before_accept, ruvia::http2_frame_type::rst_stream, 1));
        // The held bytes keep their receive-window credit until the owner consumes them.
        EXPECT_TRUE(!has_frame(before_accept, ruvia::http2_frame_type::window_update, 1));
        EXPECT_TRUE(!has_frame(before_accept, ruvia::http2_frame_type::window_update, 0));

        ruvia::http_response accepted;
        accepted.status(ruvia::http_status::ok);
        EXPECT_TRUE(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
        credit.reset();
        const auto after_accept = take_frames(server);
        EXPECT_TRUE(has_frame(after_accept, ruvia::http2_frame_type::headers, 1));
        EXPECT_TRUE(has_frame(after_accept, ruvia::http2_frame_type::window_update, 1));
        EXPECT_TRUE(has_frame(after_accept, ruvia::http2_frame_type::window_update, 0));
        EXPECT_TRUE(!has_frame(after_accept, ruvia::http2_frame_type::rst_stream, 1));

        std::string more;
        connect_drain_append_frame(more, ruvia::http2_frame_type::data, end_stream_flag, 1, "tail");
        EXPECT_TRUE(server.feed(more) == ruvia::http2_feed_result::accepted);
        auto tail = server.next_event();
        EXPECT_TRUE(tail && tail->tunnel_data() != nullptr && tail->tunnel_data()->bytes() == "tail");
        const auto end = server.next_event();
        EXPECT_TRUE(end && end->tunnel_end() != nullptr);
        EXPECT_TRUE(server.release(std::move(*head)) == ruvia::http2_server_request_release_status::released);
    }
}

// Early bytes that ended the request half are followed by tunnel_end on accept.
TEST(Ruvia, http2_connect_data_with_end_stream_before_accept_ends_tunnel_after_accept) {
    auto server = connect_drain_started_server();
    auto wire = opening_wire();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block({}));
    connect_drain_append_frame(wire, ruvia::http2_frame_type::data, end_stream_flag, 1, "hello");
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    std::optional<ruvia::http2_request_head_event> head;
    std::string received;
    while (auto event = server.next_event()) {
        if (auto* request = event->request_head()) {
            head.emplace(std::move(*request));
        } else if (auto* data = event->tunnel_data()) {
            received.append(data->bytes());
        } else {
            EXPECT_TRUE(event->tunnel_end() == nullptr && event->stream_closed() == nullptr);
        }
    }
    EXPECT_TRUE(received == "hello");
    EXPECT_TRUE(head.has_value());
    ruvia::http_response accepted;
    accepted.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
    const auto end = server.next_event();
    EXPECT_TRUE(end && end->tunnel_end() != nullptr);
    EXPECT_TRUE(server.stream_receive_status(1) == ruvia::http2_stream_receive_status::ended);
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
}

// After a non-2xx CONNECT response the optimistic bytes are discarded with their
// flow-control credit instead of resetting the stream.
TEST(Ruvia, http2_connect_data_after_rejection_is_discarded_with_credit) {
    auto server = connect_drain_started_server();
    auto wire = opening_wire();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block({}));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head_event = server.next_event();
    EXPECT_TRUE(head_event && head_event->request_head() != nullptr);
    if (!head_event || head_event->request_head() == nullptr) {
        return;
    }
    auto head = std::move(*head_event->request_head());
    ruvia::http_response rejected;
    rejected.status(ruvia::http_status::forbidden);
    const auto submitted = server.submit_response_head(
        1, rejected, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::connect, rejected));
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
    (void)take_frames(server);

    std::string late;
    const std::string bulk(16384, 'l');
    for (int frame = 0; frame != 33; ++frame) {
        connect_drain_append_frame(late, ruvia::http2_frame_type::data, 0, 1, bulk);
    }
    EXPECT_TRUE(server.feed(late) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.next_event().has_value());
    const auto frames = take_frames(server);
    EXPECT_TRUE(!has_frame(frames, ruvia::http2_frame_type::rst_stream, 1));
    EXPECT_TRUE(has_frame(frames, ruvia::http2_frame_type::window_update, 0));
    EXPECT_TRUE(has_frame(frames, ruvia::http2_frame_type::window_update, 1));
    EXPECT_TRUE(!server.connection_error().has_value());

    std::string finish;
    connect_drain_append_frame(finish, ruvia::http2_frame_type::data, end_stream_flag, 1, {});
    EXPECT_TRUE(server.feed(finish) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    EXPECT_TRUE(!server.connection_error().has_value());
    (void)server.release(std::move(head));
}

// RFC 6455 section 4.1 binds the WebSocket client to wait for the 2xx; it gives the
// recipient no reason to reset, so RFC 8441 DATA before the response is held under
// flow control as tunnel data like any other CONNECT.
TEST(Ruvia, http2_websocket_connect_data_before_response_is_held_as_tunnel_data) {
    auto server = connect_drain_started_server();
    auto wire = opening_wire();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block("websocket"));
    connect_drain_append_frame(wire, ruvia::http2_frame_type::data, 0, 1, "frame");
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    bool early_data = false;
    std::optional<ruvia::http2_request_head_event> head;
    while (auto event = server.next_event()) {
        EXPECT_TRUE(event->stream_closed() == nullptr);
        if (auto* request = event->request_head()) {
            head.emplace(std::move(*request));
        } else if (auto* data = event->tunnel_data()) {
            early_data = data->stream_id() == 1 && data->bytes() == "frame";
        }
    }
    EXPECT_TRUE(early_data);
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    EXPECT_TRUE(!server.connection_error().has_value());
    if (head) {
        (void)server.release(std::move(*head));
    }
}

// RFC 7838 section 4 and RFC 8336 section 2.1: a server MUST ignore ALTSVC, and
// ORIGIN on a nonzero stream; an established tunnel keeps them out of its bytes.
TEST(Ruvia, http2_connect_tunnel_ignores_alternative_service_and_origin_frames) {
    auto server = connect_drain_started_server();
    auto wire = opening_wire();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block({}));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    EXPECT_TRUE(head.has_value());
    if (!head) {
        return;
    }
    ruvia::http_response accepted;
    accepted.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
    (void)take_frames(server);
    while (server.next_event()) {
    }

    std::string extension;
    connect_drain_append_frame(extension, ruvia::http2_frame_type::alternative_service, 0, 1, std::string_view("\0\0h3", 4));
    connect_drain_append_frame(extension, ruvia::http2_frame_type::origin, 0, 1, {});
    connect_drain_append_frame(extension, ruvia::http2_frame_type::data, 0, 1, "after");
    EXPECT_TRUE(server.feed(extension) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error().has_value());
    EXPECT_TRUE(!server.stream_aborted(1));
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    std::string received;
    while (auto event = server.next_event()) {
        EXPECT_TRUE(event->stream_closed() == nullptr);
        if (auto* data = event->tunnel_data()) {
            received.append(data->bytes());
        }
    }
    EXPECT_EQ(received, std::string("after"));
    (void)server.release(std::move(*head));
}

// RFC 9113 section 6.8: the first GOAWAY advertises 2^31-1 so streams the client
// already sent are still served; the final boundary follows one PING round trip.
TEST(Ruvia, http2_server_graceful_drain_admits_in_flight_streams_until_ping_ack) {
    auto server = connect_drain_started_server();
    auto wire = opening_wire();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block());
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto first = take_request_head(server);
    EXPECT_TRUE(first.has_value());
    (void)take_frames(server);

    server.begin_drain();
    EXPECT_TRUE(server.draining());
    EXPECT_TRUE(!server.drain_final());
    const auto initial = take_frames(server);
    EXPECT_TRUE(goaway_last_stream_ids(initial) == std::vector<std::uint32_t>{0x7fffffffU});
    // The PING's ACK proves the client read the GOAWAY only if it follows it.
    std::optional<std::string> ping_payload;
    bool goaway_seen = false;
    for (const auto& frame : initial) {
        goaway_seen = goaway_seen || frame.is(ruvia::http2_frame_type::goaway);
        if (frame.is(ruvia::http2_frame_type::ping)) {
            EXPECT_TRUE(goaway_seen);
            EXPECT_EQ(frame.header_.flags_ & 0x1, 0);
            ping_payload = frame.payload_;
        }
    }
    EXPECT_TRUE(ping_payload.has_value());
    if (!ping_payload) {
        return;
    }

    std::string in_flight;
    connect_drain_append_frame(in_flight, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 3, get_block());
    EXPECT_TRUE(server.feed(in_flight) == ruvia::http2_feed_result::accepted);
    auto second = take_request_head(server);
    EXPECT_TRUE(second && second->stream_id() == 3);
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 3));

    std::string ack;
    connect_drain_append_frame(ack, ruvia::http2_frame_type::ping, 0x1, 0, *ping_payload);
    EXPECT_TRUE(server.feed(ack) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(server.drain_final());
    EXPECT_TRUE(goaway_last_stream_ids(take_frames(server)) == std::vector<std::uint32_t>{3});

    // RFC 9113 §6.8: a stream above the final boundary is discarded unanswered;
    // the GOAWAY already tells the client it was not processed.
    std::string discarded;
    connect_drain_append_frame(discarded, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 5, get_block());
    EXPECT_TRUE(server.feed(discarded) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.next_event().has_value());
    EXPECT_TRUE(take_frames(server).empty());
    EXPECT_TRUE(!server.connection_error().has_value());
}

// A client that withholds the PING ACK cannot keep the drain open indefinitely.
TEST(Ruvia, http2_server_graceful_drain_bounds_streams_opened_before_ping_ack) {
    auto server = connect_drain_started_server();
    const auto opening = opening_wire();
    EXPECT_TRUE(server.feed(opening) == ruvia::http2_feed_result::accepted);
    (void)take_frames(server);
    server.begin_drain();
    (void)take_frames(server);
    const auto block = get_block();
    std::uint32_t stream_id = 1;
    // The drain started at stream 0: 128 further peer streams (1..255) are admitted.
    for (; stream_id <= 255; stream_id += 2) {
        std::string request;
        connect_drain_append_frame(request, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, stream_id, block);
        EXPECT_TRUE(server.feed(request) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        EXPECT_TRUE(head.has_value());
        if (!head) {
            return;
        }
        ruvia::http_response response;
        const auto submitted = server.submit_response_head(stream_id, response,
            ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
        EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
        (void)server.release(std::move(*head));
        const auto frames = take_frames(server);
        EXPECT_TRUE(goaway_last_stream_ids(frames).empty());
        EXPECT_TRUE(!has_frame(frames, ruvia::http2_frame_type::rst_stream, stream_id));
    }
    EXPECT_TRUE(!server.drain_final());
    std::string beyond;
    connect_drain_append_frame(beyond, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, stream_id, block);
    EXPECT_TRUE(server.feed(beyond) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.next_event().has_value());
    EXPECT_TRUE(server.drain_final());
    const auto frames = take_frames(server);
    EXPECT_TRUE(goaway_last_stream_ids(frames) == std::vector<std::uint32_t>{255});
    EXPECT_TRUE(has_frame(frames, ruvia::http2_frame_type::rst_stream, stream_id));
}

// A client drains in one step: its boundary covers server-initiated streams only.
TEST(Ruvia, http2_client_graceful_drain_sends_final_goaway_at_once) {
    auto client = ruvia::http2_connection::client({});
    (void)ruvia::test::drain_output(client);
    client.begin_drain();
    EXPECT_TRUE(client.drain_final());
    const auto frames = take_frames(client);
    EXPECT_TRUE(goaway_last_stream_ids(frames) == std::vector<std::uint32_t>{0});
    for (const auto& frame : frames) {
        EXPECT_TRUE(!frame.is(ruvia::http2_frame_type::ping));
    }
}

// RFC 9113 section 6.8 asks for "at least one round-trip time", not for the PING
// ACK: the owner's time bound makes the boundary final through finish_drain().
TEST(Ruvia, http2_server_finish_drain_ends_round_trip_without_ping_ack) {
    auto server = connect_drain_started_server();
    auto wire = opening_wire();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block());
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto first = take_request_head(server);
    EXPECT_TRUE(first.has_value());
    (void)take_frames(server);
    server.begin_drain();
    EXPECT_TRUE(goaway_last_stream_ids(take_frames(server)) == std::vector<std::uint32_t>{0x7fffffffU});
    std::string in_flight;
    connect_drain_append_frame(in_flight, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 3, get_block());
    EXPECT_TRUE(server.feed(in_flight) == ruvia::http2_feed_result::accepted);
    auto second = take_request_head(server);
    EXPECT_TRUE(second && second->stream_id() == 3);
    EXPECT_TRUE(!server.drain_final());

    server.finish_drain();
    EXPECT_TRUE(server.drain_final());
    EXPECT_TRUE(goaway_last_stream_ids(take_frames(server)) == std::vector<std::uint32_t>{3});
    server.finish_drain();
    EXPECT_TRUE(take_frames(server).empty());

    std::string discarded;
    connect_drain_append_frame(discarded, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 5, get_block());
    EXPECT_TRUE(server.feed(discarded) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.next_event().has_value());
    EXPECT_TRUE(take_frames(server).empty());
    EXPECT_TRUE(!server.connection_error().has_value());
}

// A peer whose input ended can open no stream: finish_drain() from the open state
// sends the final GOAWAY at once, with no PING round trip.
TEST(Ruvia, http2_server_finish_drain_without_round_trip_sends_final_goaway) {
    auto server = connect_drain_started_server();
    auto wire = opening_wire();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block());
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    EXPECT_TRUE(head.has_value());
    (void)take_frames(server);
    server.finish_drain();
    EXPECT_TRUE(server.draining());
    EXPECT_TRUE(server.drain_final());
    const auto frames = take_frames(server);
    EXPECT_TRUE(goaway_last_stream_ids(frames) == std::vector<std::uint32_t>{1});
    for (const auto& frame : frames) {
        EXPECT_TRUE(!frame.is(ruvia::http2_frame_type::ping));
    }
}

// RFC 9113 section 6.8: "Receivers of a GOAWAY frame MUST NOT open additional
// streams on the connection", so a server answering a peer GOAWAY needs no round
// trip -- neither when the peer goes first nor during a round trip in progress.
TEST(Ruvia, http2_server_drain_after_peer_goaway_is_final_at_once) {
    for (const bool round_trip_started : {false, true}) {
        auto server = connect_drain_started_server();
        auto wire = opening_wire();
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block());
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        EXPECT_TRUE(head.has_value());
        (void)take_frames(server);
        if (round_trip_started) {
            server.begin_drain();
            (void)take_frames(server);
        }
        std::string goaway;
        connect_drain_append_frame(goaway, ruvia::http2_frame_type::goaway, 0, 0, std::string_view("\0\0\0\0\0\0\0\0", 8));
        EXPECT_TRUE(server.feed(goaway) == ruvia::http2_feed_result::accepted);
        while (server.next_event()) {
        }
        EXPECT_TRUE(server.drain_final());
        const auto frames = take_frames(server);
        EXPECT_TRUE(goaway_last_stream_ids(frames) == std::vector<std::uint32_t>{1});
        for (const auto& frame : frames) {
            EXPECT_TRUE(!frame.is(ruvia::http2_frame_type::ping));
        }
        EXPECT_TRUE(!server.connection_error().has_value());
    }
}

// RFC 8441 section 3: "Receipt of this parameter by a server does not have any
// impact", so a client may withdraw it. A value outside 0..1 is a connection
// error for either role, and a server may not withdraw it.
TEST(Ruvia, http2_enable_connect_protocol_values_are_validated_for_both_roles) {
    auto server = connect_drain_started_server();
    auto wire = opening_wire();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x01", 6));
    connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x00", 6));
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag | end_stream_flag, 1, get_block());
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!server.connection_error().has_value());
    auto head = server.next_event();
    EXPECT_TRUE(head && head->request_head() != nullptr);
    EXPECT_TRUE(goaway_last_stream_ids(take_frames(server)).empty());
    while (server.next_event()) {
    }
    std::string invalid;
    connect_drain_append_frame(invalid, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x02", 6));
    EXPECT_TRUE(server.feed(invalid) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);

    for (const std::string_view value : {std::string_view("\x00\x00\x00\x02", 4), std::string_view("\x00\x00\x00\x00", 4)}) {
        auto client = ruvia::http2_connection::client({});
        (void)ruvia::test::drain_output(client);
        std::string from_server;
        connect_drain_append_frame(from_server, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x08\x00\x00\x00\x01", 6));
        std::string entry("\x00\x08", 2);
        entry.append(value);
        connect_drain_append_frame(from_server, ruvia::http2_frame_type::settings, 0, 0, entry);
        EXPECT_TRUE(client.feed(from_server) == ruvia::http2_feed_result::protocol_failure);
        EXPECT_TRUE(client.connection_error() == ruvia::http2_error_code::protocol_error);
    }
}

// RFC 9113 section 5.5: unknown frame types are discarded, including on a
// connected CONNECT stream; known non-stream-management frames still reset it.
TEST(Ruvia, http2_connected_tunnel_discards_unknown_extension_frames) {
    auto server = connect_drain_started_server();
    auto wire = opening_wire();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, connect_block({}));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head_event = server.next_event();
    EXPECT_TRUE(head_event && head_event->request_head() != nullptr);
    if (!head_event || head_event->request_head() == nullptr) {
        return;
    }
    auto head = std::move(*head_event->request_head());
    ruvia::http_response accepted;
    accepted.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(1, accepted) == ruvia::http2_submit_status::accepted);
    (void)take_frames(server);

    std::string extension;
    connect_drain_append_frame(extension, std::uint8_t{0x21}, 0, 1, "opaque");
    connect_drain_append_frame(extension, ruvia::http2_frame_type::data, 0, 1, "after");
    EXPECT_TRUE(server.feed(extension) == ruvia::http2_feed_result::accepted);
    auto data = server.next_event();
    EXPECT_TRUE(data && data->tunnel_data() != nullptr && data->tunnel_data()->bytes() == "after");
    EXPECT_TRUE(!server.next_event().has_value());
    EXPECT_TRUE(!has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    EXPECT_TRUE(!server.stream_aborted(1));

    std::string headers;
    connect_drain_append_frame(headers, ruvia::http2_frame_type::headers, end_headers_flag, 1, get_block());
    EXPECT_TRUE(server.feed(headers) == ruvia::http2_feed_result::accepted);
    while (server.next_event()) {
    }
    EXPECT_TRUE(has_frame(take_frames(server), ruvia::http2_frame_type::rst_stream, 1));
    (void)server.release(std::move(head));
}

// RFC 9110 section 8.6 makes Content-Length on CONNECT only a sender SHOULD NOT
// and RFC 9113 section 8.5 does not list it as malformed: a declared zero is
// accepted (as in HTTP/1), while a non-zero length claims unframed content.
TEST(Ruvia, http2_connect_accepts_zero_content_length_and_resets_nonzero) {
    for (const std::string_view protocol : {std::string_view{}, std::string_view{"connect-udp"}}) {
        for (const std::string_view length : {std::string_view{"0"}, std::string_view{"3"}}) {
            auto server = connect_drain_started_server();
            auto wire = opening_wire();
            auto block = connect_block(protocol);
            ruvia::hpack_encoder::encode_header(block, "content-length", length);
            connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, block);
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            const auto head = take_request_head(server);
            const auto frames = take_frames(server);
            const bool accepted = length == "0";
            EXPECT_EQ(head.has_value(), accepted);
            EXPECT_EQ(has_frame(frames, ruvia::http2_frame_type::rst_stream, 1), !accepted);
            EXPECT_TRUE(!server.connection_error().has_value());
        }
    }
}

// RFC 8441 extended CONNECT accepts Content-Length: 0 and early DATA(END_STREAM)
// carrying a WebSocket frame before the 200 handshake response is committed.
TEST(Ruvia, http2_websocket_connect_accepts_zero_content_length_and_early_data_with_end_stream) {
    {
        auto server = connect_drain_started_server();
        auto wire = opening_wire();
        auto block = connect_block("websocket");
        ruvia::hpack_encoder::encode_header(block, "content-length", "0");
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, block);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        EXPECT_TRUE(head.has_value());
        if (head) {
            const auto req = head->build_request();
            const auto val = ruvia::validate_http2_websocket_handshake(server, 1, req);
            EXPECT_TRUE(val.accepted() != nullptr);
            const auto sub = server.submit_websocket_handshake(1, req, val);
            EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(sub));
            (void)server.release(std::move(*head));
        }
    }
    {
        auto server = connect_drain_started_server();
        auto wire = opening_wire();
        auto block = connect_block("websocket");
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, block);
        connect_drain_append_frame(wire, ruvia::http2_frame_type::data, end_stream_flag, 1,
            std::string_view("\x88\x82\x00\x00\x00\x00\x03\xe8", 8));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::optional<ruvia::http2_request_head_event> head;
        bool saw_data = false;
        while (auto ev = server.next_event()) {
            if (auto* rh = ev->request_head()) {
                head.emplace(std::move(*rh));
            } else if (ev->tunnel_data()) {
                saw_data = true;
            }
        }
        EXPECT_TRUE(head.has_value());
        EXPECT_TRUE(saw_data);
        if (head) {
            const auto req = head->build_request();
            const auto val = ruvia::validate_http2_websocket_handshake(server, 1, req);
            EXPECT_TRUE(val.accepted() != nullptr);
            const auto sub = server.submit_websocket_handshake(1, req, val);
            EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(sub));
            auto end_ev = server.next_event();
            EXPECT_TRUE(end_ev && end_ev->tunnel_end() != nullptr);
            (void)server.release(std::move(*head));
        }
    }
    {
        // The same early data split across a data frame and an empty
        // DATA(END_STREAM) must reach the same handshake verdict.
        auto server = connect_drain_started_server();
        auto wire = opening_wire();
        auto block = connect_block("websocket");
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_headers_flag, 1, block);
        connect_drain_append_frame(wire, ruvia::http2_frame_type::data, 0, 1,
            std::string_view("\x88\x82\x00\x00\x00\x00\x03\xe8", 8));
        connect_drain_append_frame(wire, ruvia::http2_frame_type::data, end_stream_flag, 1, std::string_view{});
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        std::optional<ruvia::http2_request_head_event> head;
        bool saw_data = false;
        while (auto ev = server.next_event()) {
            if (auto* rh = ev->request_head()) {
                head.emplace(std::move(*rh));
            } else if (ev->tunnel_data()) {
                saw_data = true;
            }
        }
        EXPECT_TRUE(head.has_value());
        EXPECT_TRUE(saw_data);
        if (head) {
            const auto req = head->build_request();
            const auto val = ruvia::validate_http2_websocket_handshake(server, 1, req);
            EXPECT_TRUE(val.accepted() != nullptr);
            const auto sub = server.submit_websocket_handshake(1, req, val);
            EXPECT_TRUE(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(sub));
            auto end_ev = server.next_event();
            EXPECT_TRUE(end_ev && end_ev->tunnel_end() != nullptr);
            (void)server.release(std::move(*head));
        }
    }
}

namespace {

// RFC 7541 section 5.1 prefix integer, used to craft wire input.
void append_prefixed_integer(std::string& output, std::uint8_t flags, unsigned prefix_bits, std::uint32_t value) {
    const std::uint32_t prefix_limit = (1U << prefix_bits) - 1U;
    if (value < prefix_limit) {
        output.push_back(static_cast<char>(flags | value));
        return;
    }
    output.push_back(static_cast<char>(flags | prefix_limit));
    value -= prefix_limit;
    while (value >= 128) {
        output.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    output.push_back(static_cast<char>(value));
}

[[nodiscard]] std::string table_size_update(std::uint32_t bytes) {
    std::string output;
    append_prefixed_integer(output, 0x20, 5, bytes);
    return output;
}

[[nodiscard]] std::string indexed_field(std::uint32_t index) {
    std::string output;
    append_prefixed_integer(output, 0x80, 7, index);
    return output;
}

// Literal with incremental indexing and a new name (RFC 7541 section 6.2.1),
// without Huffman coding.
[[nodiscard]] std::string indexed_literal(std::string_view name, std::string_view value) {
    std::string output(1, static_cast<char>(0x40));
    append_prefixed_integer(output, 0x00, 7, static_cast<std::uint32_t>(name.size()));
    output.append(name);
    append_prefixed_integer(output, 0x00, 7, static_cast<std::uint32_t>(value.size()));
    output.append(value);
    return output;
}

void prepare_alias_output(std::string& output, bool grow) {
    output = "content-type|authorization|x-alias-name|abcdefghijklmnopqrstuvwxyz";
    output.append(192, 'v');
    if (grow) {
        output.resize(output.capacity(), '#');
    } else {
        output.reserve(output.size() * 4 + 128);
    }
}

void check_encoded_field(std::string_view output, std::string_view prefix,
    std::string_view expected_name, std::string_view expected_value) {
    EXPECT_EQ(output.substr(0, prefix.size()), prefix);
    ruvia::hpack_decoder decoder;
    std::size_t fields = 0;
    const auto result = decoder.decode(output.substr(prefix.size()),
        [&](std::string_view name, std::string_view value) {
            ++fields;
            EXPECT_EQ(name, expected_name);
            EXPECT_EQ(value, expected_value);
            return true;
        });
    EXPECT_TRUE(!result.has_value());
    EXPECT_EQ(fields, std::size_t{1});
}

void check_newest(ruvia::hpack_decoder& decoder,
    std::uint32_t index, std::string_view expected_name, std::string_view expected_value) {
    std::size_t fields = 0;
    const auto result = decoder.decode(indexed_field(index), [&](std::string_view name, std::string_view value) {
        EXPECT_EQ(name, expected_name);
        EXPECT_EQ(value, expected_value);
        ++fields;
        return true;
    });
    EXPECT_TRUE(!result.has_value());
    EXPECT_EQ(fields, std::size_t{1});
}

[[nodiscard]] bool decoder_is_poisoned(ruvia::hpack_decoder& decoder) {
    std::size_t fields = 0;
    const auto result = decoder.decode(std::string_view("\x82", 1), [&](auto, auto) {
        ++fields;
        return true;
    });
    return result == ruvia::hpack_decode_error::compression_error && fields == 0;
}

}  // namespace

TEST(Ruvia, hpack_public_encoder_accepts_fields_borrowed_from_output) {
    enum class alias_kind { name,
        value,
        disjoint,
        overlapping,
        identical,
        static_name,
        sensitive_name };
    for (const bool grow : {false, true}) {
        for (const auto kind : {alias_kind::name, alias_kind::value, alias_kind::disjoint,
                 alias_kind::overlapping, alias_kind::identical, alias_kind::static_name,
                 alias_kind::sensitive_name}) {
            {
                std::string output;
                prepare_alias_output(output, grow);
                const std::string prefix(output);
                const auto contents = std::string_view(output);
                const auto payload = contents.find("abcdefghijklmnopqrstuvwxyz");
                std::string_view name = "x-outside";
                std::string_view value = "outside-value";
                switch (kind) {
                    case alias_kind::name:
                        name = contents.substr(contents.find("x-alias-name"), 12);
                        break;
                    case alias_kind::value:
                        value = contents.substr(payload, 100);
                        break;
                    case alias_kind::disjoint:
                        name = contents.substr(contents.find("x-alias-name"), 12);
                        value = contents.substr(payload, 100);
                        break;
                    case alias_kind::overlapping:
                        name = contents.substr(payload, 40);
                        value = contents.substr(payload + 11, 80);
                        break;
                    case alias_kind::identical:
                        name = contents.substr(payload, 60);
                        value = name;
                        break;
                    case alias_kind::static_name:
                        name = contents.substr(0, 12);
                        value = contents.substr(payload, 100);
                        break;
                    case alias_kind::sensitive_name:
                        name = contents.substr(contents.find("authorization"), 13);
                        value = contents.substr(payload, 100);
                        break;
                }
                const std::string expected_name(name);
                const std::string expected_value(value);
                ruvia::hpack_encoder::encode_header(output, name, value);
                check_encoded_field(output, prefix, expected_name, expected_value);
            }
        }
    }
}

TEST(Ruvia, hpack_public_encoder_never_indexes_sensitive_fields) {
    // RFC 7541 section 7.1.3: credentials use the never-indexed literal form,
    // even when the static table holds an exact name/value match.
    for (const std::string_view name : {"authorization", "proxy-authorization", "cookie", "set-cookie"}) {
        std::string output;
        ruvia::hpack_encoder::encode_header(output, name, "");
        EXPECT_TRUE(!output.empty());
        EXPECT_EQ(static_cast<unsigned char>(output[0]) & 0xf0U, 0x10U);
        check_encoded_field(output, {}, name, "");
    }
}

TEST(Ruvia, hpack_public_table_size_rejects_out_of_range_integers_and_excess_updates) {
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
             static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1}) {
        EXPECT_TRUE(!ruvia::hpack_table_size::try_from_value(invalid).has_value());
        ruvia::hpack_decoder decoder;
        bool rejected_limit = false;
        try {
            decoder.set_max_dynamic_table_size(invalid);
        } catch (const std::invalid_argument&) {
            rejected_limit = true;
        }
        EXPECT_TRUE(rejected_limit);
    }
    EXPECT_TRUE(!ruvia::hpack_table_size::try_from_value((std::numeric_limits<std::uint64_t>::max)()).has_value());

    ruvia::hpack_decoder decoder;
    decoder.set_max_dynamic_table_size(64);
    EXPECT_TRUE(!decoder.decode(table_size_update(64), [](auto, auto) { return true; }).has_value());
    EXPECT_TRUE(decoder.decode(table_size_update(65), [](auto, auto) { return true; }) ==
                ruvia::hpack_decode_error::compression_error);
    EXPECT_TRUE(decoder_is_poisoned(decoder));
}

TEST(Ruvia, hpack_public_lowered_capacity_requires_a_leading_update) {
    // RFC 7541 section 4.2: the first block after the decoder's maximum shrinks
    // must begin with a dynamic table size update.
    for (const bool signaled : {false, true}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder.decode(indexed_literal("a", "b"), [](auto, auto) { return true; }).has_value());
        decoder.set_max_dynamic_table_size(0);
        std::string block = signaled ? table_size_update(0) : std::string();
        block.push_back(static_cast<char>(0x82));
        std::size_t fields = 0;
        const auto result = decoder.decode(block, [&](auto, auto) {
            ++fields;
            return true;
        });
        if (signaled) {
            EXPECT_TRUE(!result.has_value());
            EXPECT_EQ(fields, std::size_t{1});
            EXPECT_TRUE(decoder.decode(indexed_field(62), [](auto, auto) { return true; }) ==
                        ruvia::hpack_decode_error::compression_error);
        } else {
            EXPECT_TRUE(result == ruvia::hpack_decode_error::compression_error);
            EXPECT_EQ(fields, std::size_t{0});
        }
    }
}

TEST(Ruvia, hpack_public_encoder_preserves_the_output_terminator_as_input) {
    for (const bool empty_output : {false, true}) {
        std::string output(empty_output ? "" : "prefix");
        const std::string prefix(output);
        const std::string_view value(output.data() + output.size(), 1);
        ruvia::hpack_encoder::encode_header(output, "content-type", value);
        check_encoded_field(output, prefix, "content-type", std::string_view("\0", 1));
    }
}

TEST(Ruvia, hpack_public_dynamic_entries_own_fields_after_input_changes) {
    {
        ruvia::hpack_decoder decoder;
        {
            std::string block = indexed_literal(std::string(24, 'n'), std::string(24, 'v'));
            EXPECT_TRUE(!decoder.decode(block, [](auto, auto) { return true; }).has_value());
            block.assign(block.size(), '#');
        }
        {
            // Huffman-coded literal: the decoded bytes cannot borrow the input.
            std::string block;
            ruvia::hpack_encoder::encode_header(block, std::string(24, 'a'), std::string(25, 'b'));
            block[0] = static_cast<char>(0x40);
            EXPECT_TRUE(!decoder.decode(std::string_view(block), [](auto, auto) { return true; }).has_value());
            std::fill(block.begin(), block.end(), '#');
        }
        check_newest(decoder, 62, std::string(24, 'a'), std::string(25, 'b'));
        check_newest(decoder, 63, std::string(24, 'n'), std::string(24, 'v'));
        // RFC 7541 Appendix C.4.1: ":authority: www.example.com" as a Huffman literal.
        constexpr std::string_view huffman_authority(
            "\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 14);
        EXPECT_TRUE(!decoder.decode(huffman_authority, [](std::string_view name, std::string_view value) {
                                return name == ":authority" && value == "www.example.com";
                            })
                .has_value());
    }
}

TEST(Ruvia, hpack_public_empty_and_one_sided_fields_are_indexable) {
    ruvia::hpack_decoder decoder;
    const std::string long_bytes(49, 'x');
    const std::string block = indexed_literal("", "") + indexed_literal("", long_bytes) + indexed_literal(long_bytes, "");
    EXPECT_TRUE(!decoder.decode(block, [](auto, auto) { return true; }).has_value());
    check_newest(decoder, 62, long_bytes, "");
    check_newest(decoder, 63, "", long_bytes);
    check_newest(decoder, 64, "", "");
}

TEST(Ruvia, hpack_public_dynamic_entries_remain_indexable_across_evictions) {
    ruvia::hpack_decoder decoder;
    EXPECT_TRUE(!decoder.decode(table_size_update(120), [](auto, auto) { return true; }).has_value());
    const std::string name(40, 'n');
    const std::string value(40, 'v');
    const std::string insertion = indexed_literal(name, value);
    for (std::size_t i = 0; i < 200; ++i) {
        EXPECT_TRUE(!decoder.decode(insertion, [](auto, auto) { return true; }).has_value());
        check_newest(decoder, 62, name, value);
    }
}

TEST(Ruvia, hpack_public_leading_capacity_updates_apply_in_wire_order) {
    const std::array<std::array<std::uint32_t, 3>, 3> sequences{{{512, 64, 64}, {512, 64, 512}, {0, 0, 512}}};
    for (const auto& updates : sequences) {
        ruvia::hpack_decoder decoder;
        const std::string seed_value(64, 's');
        EXPECT_TRUE(!decoder.decode(indexed_literal("x-seed", seed_value), [](auto, auto) { return true; }).has_value());
        std::string block;
        for (const auto size : updates) {
            block += table_size_update(size);
        }
        block += indexed_literal("x", "fresh");
        std::size_t fields = 0;
        const auto decoded = decoder.decode(block, [&](std::string_view name, std::string_view value) {
            EXPECT_EQ(name, std::string_view("x"));
            EXPECT_EQ(value, std::string_view("fresh"));
            ++fields;
            return true;
        });
        EXPECT_TRUE(!decoded.has_value());
        EXPECT_EQ(fields, std::size_t{1});
        check_newest(decoder, 62, "x", "fresh");
        // Restoring capacity cannot restore an entry evicted at an intermediate minimum.
        EXPECT_TRUE(decoder.decode(indexed_field(63), [](auto, auto) { return true; }) ==
                    ruvia::hpack_decode_error::compression_error);
    }
}

TEST(Ruvia, hpack_public_capacity_updates_reject_excess_and_late_positions) {
    for (const bool late : {false, true}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder.decode(indexed_literal("x-seed", "kept"), [](auto, auto) { return true; }).has_value());
        const std::string invalid = late ? indexed_field(62) + table_size_update(0) : table_size_update(4097);
        EXPECT_TRUE(decoder.decode(invalid, [](auto, auto) { return true; }) ==
                    ruvia::hpack_decode_error::compression_error);
        EXPECT_TRUE(decoder_is_poisoned(decoder));
    }
}

TEST(Ruvia, hpack_public_callback_rejection_keeps_dynamic_table_consistent) {
    ruvia::hpack_decoder decoder;
    const auto rejected = decoder.decode(indexed_literal("a", "b"), [](auto, auto) { return false; });
    EXPECT_TRUE(rejected == ruvia::hpack_decode_error::callback_rejected);
    check_newest(decoder, 62, "a", "b");
}

TEST(Ruvia, hpack_public_compression_error_poisons_the_decoder) {
    for (const std::string_view failing : {std::string_view("\x80", 1), std::string_view("\x40\x05\x61", 3)}) {
        ruvia::hpack_decoder decoder;
        EXPECT_TRUE(!decoder.decode(indexed_literal("a", "b"), [](auto, auto) { return true; }).has_value());
        std::string block = indexed_literal("c", "d");
        block.append(failing);
        EXPECT_TRUE(decoder.decode(block, [](auto, auto) { return true; }) ==
                    ruvia::hpack_decode_error::compression_error);
        EXPECT_TRUE(decoder_is_poisoned(decoder));
    }
}

TEST(Ruvia, hpack_public_long_dynamic_name_and_value_are_preserved) {
    ruvia::hpack_decoder decoder;
    const std::string name(300, 'n');
    const std::string value(300, 'v');
    EXPECT_TRUE(!decoder.decode(indexed_literal(name, value), [](auto, auto) { return true; }).has_value());
    check_newest(decoder, 62, name, value);
}

TEST(Ruvia, hpack_public_eviction_preserves_relative_indices) {
    ruvia::hpack_decoder decoder;
    const std::string long_name(48, 'n');
    std::string initial = table_size_update(1150);
    for (int i = 0; i < 36; ++i) {
        const char value = static_cast<char>('A' + i);
        initial += indexed_literal(i % 2 == 0 ? std::string_view("a") : std::string_view(long_name),
            std::string_view(&value, 1));
    }
    initial.push_back(static_cast<char>(0x82));
    EXPECT_TRUE(!decoder.decode(initial, [](auto, auto) { return true; }).has_value());
    std::string indices;
    for (std::uint32_t index = 62; index < 82; ++index) {
        indices += indexed_field(index);
    }
    int expected = 35;
    const auto result = decoder.decode(indices, [&](std::string_view name, std::string_view value) {
        const auto expected_name = expected % 2 == 0 ? std::string_view("a") : std::string_view(long_name);
        const bool matches = name == expected_name && value.size() == 1 && value[0] == 'A' + expected;
        --expected;
        return matches;
    });
    EXPECT_TRUE(!result.has_value());
    EXPECT_EQ(expected, 15);
    // An indexed name survives the eviction its own insertion causes.
    EXPECT_TRUE(!decoder.decode(std::string_view("\x7e\x01z", 3), [](auto, auto) { return true; }).has_value());
    check_newest(decoder, 62, long_name, "z");
}

TEST(Ruvia, hpack_public_decoder_move_preserves_dynamic_entries) {
    ruvia::hpack_decoder source_value;
    EXPECT_TRUE(!source_value.decode(indexed_literal("a", "b"), [](auto, auto) { return true; }).has_value());
    ruvia::hpack_decoder moved(std::move(source_value));
    ruvia::hpack_decoder destination;
    destination = std::move(moved);
    check_newest(destination, 62, "a", "b");
}

#if !defined(_MSC_VER)
TEST(Ruvia, hpack_public_callback_exception_propagates_across_remaining_fields) {
    ruvia::hpack_decoder decoder;

    std::string block(1, static_cast<char>(0x82));  // indexed :method: GET
    block += indexed_literal(std::string(100, 'x'), "y");

    bool saw_original = false;
    try {
        (void)decoder.decode(block, [&](std::string_view, std::string_view) -> bool {
            throw std::runtime_error("original callback failure");
        });
    } catch (const std::runtime_error& error) {
        saw_original = std::string_view(error.what()) == "original callback failure";
    }
    EXPECT_TRUE(saw_original);
}
#endif

TEST(Ruvia, hpack_public_callback_cannot_replace_the_active_block_or_its_capacity) {
    for (const bool resize : {false, true}) {
        ruvia::hpack_decoder decoder;
        ruvia::hpack_decoder independent;
        std::size_t callbacks = 0;
        std::size_t rejected = 0;
        const auto result = decoder.decode(indexed_literal("a", "b"), [&](std::string_view name, std::string_view value) {
            ++callbacks;
            try {
                if (resize) {
                    decoder.set_max_dynamic_table_size(0);
                } else {
                    (void)decoder.decode({}, [](auto, auto) { return true; });
                }
            } catch (const std::logic_error&) {
                ++rejected;
            }
            EXPECT_EQ(name, "a");
            EXPECT_EQ(value, "b");
            std::size_t independent_fields = 0;
            EXPECT_TRUE(!independent.decode(std::string_view("\x82", 1), [&](auto inner_name, auto inner_value) {
                                        ++independent_fields;
                                        EXPECT_EQ(inner_name, ":method");
                                        EXPECT_EQ(inner_value, "GET");
                                        return true;
                                    })
                    .has_value());
            EXPECT_EQ(independent_fields, std::size_t{1});
            return true;
        });
        EXPECT_TRUE(!result.has_value());
        EXPECT_EQ(callbacks, std::size_t{1});
        EXPECT_EQ(rejected, std::size_t{1});
        check_newest(decoder, 62, "a", "b");
    }
}

TEST(Ruvia, hpack_public_callback_state_error_is_rethrown_after_the_block) {
    for (const bool resize : {false, true}) {
        for (const bool malformed : {false, true}) {
            ruvia::hpack_decoder decoder;
            const auto accept = [](auto, auto) { return true; };
            EXPECT_TRUE(!decoder.decode(indexed_literal("s", "t"), accept).has_value());
            std::string block = indexed_literal("a", "b") + indexed_literal("c", "d");
            if (malformed) {
                block.push_back(static_cast<char>(0x80));
            }
            std::size_t callbacks = 0;
            bool state_error = false;
            try {
                (void)decoder.decode(block, [&](auto, auto) {
                    ++callbacks;
                    if (resize) {
                        decoder.set_max_dynamic_table_size(0);
                    } else {
                        (void)decoder.decode({}, accept);
                    }
                    return true;
                });
            } catch (const std::logic_error&) {
                state_error = true;
            }
            EXPECT_TRUE(state_error);
            EXPECT_EQ(callbacks, std::size_t{1});
            if (malformed) {
                EXPECT_TRUE(decoder_is_poisoned(decoder));
            } else {
                check_newest(decoder, 62, "c", "d");
                check_newest(decoder, 63, "a", "b");
            }
        }
    }
}

TEST(Ruvia, hpack_encoder_uses_canonical_wire_indices_for_every_static_entry) {
    ruvia::hpack_decoder decoder;
    // RFC 7541 Appendix A defines the 61 one-based static table indices.
    for (std::uint32_t index = 1; index <= 61; ++index) {
        const char indexed_field = static_cast<char>(0x80U | index);
        std::string encoded;
        std::string name;
        std::size_t fields_value = 0;
        const auto decoded = decoder.decode(std::string_view(&indexed_field, 1),
            [&](std::string_view field_name, std::string_view value) {
                ++fields_value;
                name.assign(field_name);
                ruvia::hpack_encoder::encode_header(encoded, field_name, value);
                return true;
            });
        EXPECT_TRUE(!decoded.has_value());
        EXPECT_EQ(fields_value, std::size_t{1});
        // Credentials are never indexed (RFC 7541 section 7.1.3), so their
        // static entries are referenced by name only.
        if (name == "authorization" || name == "proxy-authorization" || name == "cookie" || name == "set-cookie") {
            continue;
        }
        EXPECT_EQ(encoded, std::string_view(&indexed_field, 1));
    }
}

namespace http2_receive_credit_test {

std::size_t advertised_receive_window(std::string_view output) {
    output.remove_prefix(ruvia::http2_client_preface.size());
    std::uint32_t connection_window = 65535;
    std::uint32_t stream_window = 65535;
    while (!output.empty()) {
        const auto header = ruvia::parse_http2_frame_header(std::span(output.data(), output.size()));
        if (!header || output.size() < ruvia::http2_frame_header_bytes + header->length_) {
            throw std::logic_error("invalid initial HTTP/2 output");
        }
        const auto payload = output.substr(ruvia::http2_frame_header_bytes, header->length_);
        if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings)) {
            for (std::size_t index = 0; index + 6 <= payload.size(); index += 6) {
                if (payload[index] == 0 && payload[index + 1] == 4) {
                    stream_window = read_u32(payload.substr(index + 2));
                }
            }
        } else if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::window_update) && header->stream_id_ == 0) {
            connection_window += read_u32(payload) & 0x7fffffffU;
        }
        output.remove_prefix(ruvia::http2_frame_header_bytes + header->length_);
    }
    return std::min(connection_window, stream_window);
}

void append_body(std::string& wire, std::size_t bytes) {
    const std::string chunk(16384, 'x');
    while (bytes != 0) {
        const auto count = std::min(bytes, chunk.size());
        connect_drain_append_frame(wire, ruvia::http2_frame_type::data, 0, 1,
            std::string_view(chunk).substr(0, count));
        bytes -= count;
    }
}

}  // namespace http2_receive_credit_test
using namespace http2_receive_credit_test;

// Acknowledging a credit for the whole advertised receive window restores
// exactly one window on the connection and on the stream.
TEST(Ruvia, http2_public_receive_credit_restores_exact_window) {
    auto client = ruvia::http2_connection::client({});
    const auto receive_window = advertised_receive_window(ruvia::test::drain_output(client));
    EXPECT_TRUE(receive_window != 0);
    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
    (void)ruvia::test::drain_output(client);

    std::string wire;
    connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    // The client's advertised stream window applies once the peer ACKs it.
    connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0x1, 0, {});
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":status", "200");
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    append_body(wire, receive_window);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::accepted);

    std::optional<ruvia::http2_received_data_credit> credit;
    while (auto event = client.next_event()) {
        if (auto* body = event->message_body_chunk()) {
            if (credit) {
                credit->merge(body->take_credit());
            } else {
                credit.emplace(body->take_credit());
            }
        }
    }
    EXPECT_TRUE(credit && credit->valid());
    if (!credit) {
        return;
    }
    // Received bytes are credited only once their credit is destroyed.
    std::string prefix;
    connect_drain_append_frame(prefix, ruvia::http2_frame_type::settings, 0x1, 0, {});
    EXPECT_TRUE(ruvia::test::drain_output(client) == std::string_view(prefix));
    credit.reset();

    const auto output = ruvia::test::drain_output(client);
    std::string_view updates(output);
    std::size_t connection_updates = 0;
    std::size_t stream_updates = 0;
    while (!updates.empty()) {
        const auto header = ruvia::parse_http2_frame_header(std::span(updates.data(), updates.size()));
        EXPECT_TRUE(header.has_value());
        if (!header) {
            return;
        }
        EXPECT_TRUE(header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::window_update));
        EXPECT_EQ(header->length_, 4u);
        EXPECT_EQ(read_u32(updates.substr(ruvia::http2_frame_header_bytes)), receive_window);
        connection_updates += header->stream_id_ == 0;
        stream_updates += header->stream_id_ == 1;
        updates.remove_prefix(ruvia::http2_frame_header_bytes + header->length_);
    }
    EXPECT_EQ(connection_updates, std::size_t{1});
    EXPECT_EQ(stream_updates, std::size_t{1});
    EXPECT_TRUE(!client.wants_write());

    wire.clear();
    append_body(wire, receive_window + 1);
    EXPECT_TRUE(client.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(client.connection_error() == ruvia::http2_error_code::flow_control_error);
}

namespace http2_request_pseudo_headers_test {

using pseudo_header = ruvia::http2_request_pseudo_header;

void encode_pseudo_header(std::string& block, pseudo_header header) {
    switch (header) {
        case pseudo_header::method:
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            break;
        case pseudo_header::scheme:
            ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
            break;
        case pseudo_header::authority:
            ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
            break;
        case pseudo_header::path:
            ruvia::hpack_encoder::encode_header(block, ":path", "/");
            break;
        case pseudo_header::protocol:
            ruvia::hpack_encoder::encode_header(block, ":protocol", "websocket");
            break;
    }
}

bool same_order(std::span<const pseudo_header> actual, std::span<const pseudo_header> expected) {
    return std::equal(actual.begin(), actual.end(), expected.begin(), expected.end());
}

}  // namespace http2_request_pseudo_headers_test
using namespace http2_request_pseudo_headers_test;

TEST(Ruvia, http2_request_pseudo_headers_preserve_every_regular_order) {
    std::array order{pseudo_header::method, pseudo_header::scheme, pseudo_header::authority, pseudo_header::path};
    do {
        auto server = ruvia::http2_connection::server();
        std::string block;
        for (const auto header : order) {
            encode_pseudo_header(block, header);
        }
        auto wire = opening_wire();
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        EXPECT_TRUE(head.has_value());
        if (head) {
            const auto view = head->request_view();
            EXPECT_TRUE(same_order(view.pseudo_header_order_, order));
            EXPECT_TRUE(view.method_ == "GET" && view.scheme_ == "https" && view.path_ == "/" && view.authority_ == "example.test");
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST(Ruvia, http2_request_pseudo_headers_do_not_synthesize_authority_from_host) {
    auto server = ruvia::http2_connection::server();
    constexpr std::array order{pseudo_header::path, pseudo_header::method, pseudo_header::scheme};
    std::string block;
    for (const auto header : order) {
        encode_pseudo_header(block, header);
    }
    ruvia::hpack_encoder::encode_header(block, "host", "example.test");
    auto wire = opening_wire();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    EXPECT_TRUE(head.has_value());
    if (head) {
        EXPECT_TRUE(head->request_view().authority_ == "example.test");
        EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, order));
    }
}

TEST(Ruvia, http2_request_pseudo_headers_huffman_dynamic_index_and_continuation) {
    // RFC 7541 C.4.1: Huffman authority inserted at dynamic index 62.
    const std::string block("\x82\x86\x84\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 17);
    for (std::size_t split = 0; split <= block.size(); ++split) {
        auto server = ruvia::http2_connection::server();
        auto first = opening_wire();
        connect_drain_append_frame(first, ruvia::http2_frame_type::headers, 0x1, 1, std::string_view(block).substr(0, split));
        (void)server.feed(first);
        EXPECT_TRUE(!server.next_event());
        std::string final;
        connect_drain_append_frame(final, ruvia::http2_frame_type::continuation, 0x4, 1, std::string_view(block).substr(split));
        EXPECT_TRUE(server.feed(final) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        constexpr std::array expected{pseudo_header::method, pseudo_header::scheme, pseudo_header::path, pseudo_header::authority};
        EXPECT_TRUE(head.has_value());
        if (head) {
            EXPECT_TRUE(head->request_view().authority_ == "www.example.com");
            EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, expected));
        }
        std::string next;
        connect_drain_append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, "\xbe\x84\x87\x82");
        EXPECT_TRUE(server.feed(next) == ruvia::http2_feed_result::accepted);
        auto indexed = take_request_head(server);
        constexpr std::array indexed_order{pseudo_header::authority, pseudo_header::path, pseudo_header::scheme, pseudo_header::method};
        EXPECT_TRUE(indexed.has_value());
        if (indexed) {
            EXPECT_TRUE(indexed->request_view().authority_ == "www.example.com");
            EXPECT_TRUE(same_order(indexed->request_view().pseudo_header_order_, indexed_order));
        }
        EXPECT_TRUE(!server.connection_error());
    }
}

TEST(Ruvia, http2_request_pseudo_headers_preserve_both_connect_forms) {
    for (const bool extended : {false, true}) {
        auto server = ruvia::http2_connection::server();
        (void)ruvia::test::drain_output(server);
        std::string block;
        if (extended) {
            encode_pseudo_header(block, pseudo_header::protocol);
            encode_pseudo_header(block, pseudo_header::path);
            encode_pseudo_header(block, pseudo_header::scheme);
        }
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test:443");
        ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
        if (extended) {
            ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
        }
        auto wire = opening_wire();
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        auto head = take_request_head(server);
        constexpr std::array extended_order{pseudo_header::protocol, pseudo_header::path, pseudo_header::scheme, pseudo_header::authority, pseudo_header::method};
        const auto expected = std::span(extended_order).subspan(extended ? 0 : 3);
        EXPECT_TRUE(head.has_value());
        if (head) {
            EXPECT_TRUE(head->snapshot().connect_pending_);
            EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, expected));
        }
    }
}

TEST(Ruvia, http2_invalid_pseudo_headers_do_not_publish_but_keep_dynamic_table_sync) {
    auto server = ruvia::http2_connection::server();
    auto wire = opening_wire();
    // A duplicate :method fails request semantics, but the later incremental
    // :path literal must still become dynamic index 62 (RFC 7541 section 2.3.2).
    const std::string malformed("\x82\x82\x44\x07/synced", 11);
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, malformed);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!take_request_head(server));
    EXPECT_TRUE(server.stream_aborted(1));
    std::string indexed("\xbe\x87\x82", 3);
    encode_pseudo_header(indexed, pseudo_header::authority);
    std::string next;
    connect_drain_append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, indexed);
    EXPECT_TRUE(server.feed(next) == ruvia::http2_feed_result::accepted);
    auto head = take_request_head(server);
    constexpr std::array expected{pseudo_header::path, pseudo_header::scheme, pseudo_header::method, pseudo_header::authority};
    EXPECT_TRUE(head.has_value());
    if (head) {
        EXPECT_EQ(head->stream_id(), std::uint32_t{3});
        EXPECT_TRUE(head->request_view().path_ == "/synced");
        EXPECT_TRUE(same_order(head->request_view().pseudo_header_order_, expected));
    }
    EXPECT_TRUE(!server.connection_error());
}

TEST(Ruvia, http2_request_pseudo_header_lease_survives_reset_move_and_later_input) {
    {
        std::optional<ruvia::http2_request_head_event> lease;
        ruvia::http2_server_request_view saved;
        constexpr std::array expected{pseudo_header::authority, pseudo_header::path, pseudo_header::method, pseudo_header::scheme};
        {
            auto server = ruvia::http2_connection::server({});
            std::string block;
            for (const auto header : expected) {
                encode_pseudo_header(block, header);
            }
            auto wire = opening_wire();
            connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            connect_drain_append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view("\0\0\0\x08", 4));
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            auto received = take_request_head(server);
            EXPECT_TRUE(received.has_value());
            if (!received) {
                return;
            }
            lease.emplace(std::move(*received));
            saved = lease->request_view();
            EXPECT_TRUE(same_order(saved.pseudo_header_order_, expected));
            auto moved = std::move(server);
            std::string next;
            connect_drain_append_frame(next, ruvia::http2_frame_type::headers, 0x5, 3, block);
            EXPECT_TRUE(moved.feed(next) == ruvia::http2_feed_result::accepted);
            auto following = take_request_head(moved);
            EXPECT_TRUE(following.has_value());
            EXPECT_TRUE(same_order(saved.pseudo_header_order_, expected));
            EXPECT_TRUE(same_order(lease->request_view().pseudo_header_order_, expected));
            moved = ruvia::http2_connection::server({});
        }
        EXPECT_TRUE(saved.authority_ == "example.test");
        EXPECT_TRUE(same_order(saved.pseudo_header_order_, expected));
        EXPECT_TRUE(same_order(lease->request_view().pseudo_header_order_, expected));
    }
}

TEST(Ruvia, http2_locally_synthesized_push_has_no_received_pseudo_header_order) {
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server();
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"})));
    relay.transfer(client, server);
    auto parent = take_request_head(server);
    EXPECT_TRUE(parent.has_value());
    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/asset"});
    EXPECT_TRUE(pushed.index() == 0);
    if (pushed.index() == 0) {
        EXPECT_TRUE(std::get<0>(pushed).request_view().pseudo_header_order_.empty());
        EXPECT_TRUE(std::get<0>(pushed).request_view().path_ == "/asset");
    }
}

TEST(Ruvia, http2_request_path_rejects_empty_and_defers_scheme_validation_to_finish) {
    // RFC 9113 §8.3.1: :path must be non-empty on wire; non-http(s) schemes
    // accept non-origin-form :path in finish_request_head.
    {
        auto server = ruvia::http2_connection::server();
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "custom");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "opaque-path");
        auto wire = opening_wire();
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        EXPECT_EQ(server.feed(wire), ruvia::http2_feed_result::accepted);
        const auto head = take_request_head(server);
        ASSERT_TRUE(head.has_value());
        EXPECT_EQ(head->request_view().path_, "opaque-path");
    }
    {
        auto server = ruvia::http2_connection::server();
        std::string block;
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "custom");
        ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
        ruvia::hpack_encoder::encode_header(block, ":path", "");
        auto wire = opening_wire();
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
        EXPECT_EQ(server.feed(wire), ruvia::http2_feed_result::accepted);
        EXPECT_FALSE(take_request_head(server).has_value());
        EXPECT_TRUE(server.stream_aborted(1));
    }
}

namespace http2_receive_metadata_test {

template <typename value_type>
concept settings_borrowable = requires(value_type&& value) {
    std::forward<value_type>(value).settings();
};
static_assert(settings_borrowable<const ruvia::http2_frame_metadata&>);
static_assert(!settings_borrowable<ruvia::http2_frame_metadata>);

enum class received_kind : std::uint8_t { settings,
    window_update,
    priority,
    request_head };

struct received_record final {
    received_kind kind_{received_kind::settings};
    std::array<ruvia::http2_setting_entry, 8> settings_{};
    std::size_t settings_count_{0};
    bool checked_bounds_{false};
    std::uint32_t stream_id_{0};
    std::uint32_t value_{0};
    std::uint16_t weight_{0};
    bool exclusive_{false};
    bool request_matches_{false};
    std::array<ruvia::http2_request_pseudo_header, 5> pseudo_order_{};
    std::size_t pseudo_count_{0};
};

// 按 next_event() 顺序记录元数据与请求头事件，其余事件不在本文件观察范围内。
// 请求头租约保留到日志析构，使对应流保持存活。
struct received_log final {
    std::array<received_record, 16> records_{};
    std::size_t count_{0};
    bool overflow_{false};
    std::vector<ruvia::http2_request_head_event> requests_;

    void drain(ruvia::http2_connection& connection) {
        while (auto event = connection.next_event()) {
            append(*event);
        }
    }

private:
    void append(ruvia::http2_event& event) {
        const auto* settings = event.received_settings();
        const auto* window = event.received_window_update();
        const auto* priority = event.received_priority();
        auto* request = event.request_head();
        if (settings == nullptr && window == nullptr && priority == nullptr && request == nullptr) {
            return;
        }
        if (count_ == records_.size()) {
            overflow_ = true;
            return;
        }
        auto& record = records_[count_++];
        if (settings != nullptr) {
            record.kind_ = received_kind::settings;
            record.settings_count_ = settings->size();
            if (record.settings_count_ > record.settings_.size()) {
                overflow_ = true;
                return;
            }
            for (std::size_t index = 0; index != record.settings_count_; ++index) {
                record.settings_[index] = settings->at(index);
            }
            try {
                (void)settings->at(settings->size());
            } catch (const std::out_of_range&) {
                record.checked_bounds_ = true;
            }
        } else if (window != nullptr) {
            record.kind_ = received_kind::window_update;
            record.stream_id_ = window->stream_id_;
            record.value_ = window->increment_;
        } else if (priority != nullptr) {
            record.kind_ = received_kind::priority;
            record.stream_id_ = priority->stream_id_;
            record.value_ = priority->stream_dependency_;
            record.weight_ = priority->weight_;
            record.exclusive_ = priority->exclusive_;
        } else {
            record.kind_ = received_kind::request_head;
            record.stream_id_ = request->stream_id();
            const auto view = request->request_view();
            record.request_matches_ = view.method_ == "GET" && view.scheme_ == "https" &&
                                      view.authority_ == "example.test" && view.path_ == "/metadata";
            record.pseudo_count_ = view.pseudo_header_order_.size();
            if (record.pseudo_count_ > record.pseudo_order_.size()) {
                overflow_ = true;
                return;
            }
            for (std::size_t index = 0; index != record.pseudo_count_; ++index) {
                record.pseudo_order_[index] = view.pseudo_header_order_[index];
            }
            requests_.push_back(std::move(*request));
        }
    }
};

void append_setting(std::string& payload, std::uint16_t id, std::uint32_t value) {
    const std::array<char, 6> entry{static_cast<char>(id >> 8), static_cast<char>(id),
        static_cast<char>(value >> 24), static_cast<char>(value >> 16),
        static_cast<char>(value >> 8), static_cast<char>(value)};
    payload.append(entry.data(), entry.size());
}

std::string window_payload(std::uint32_t increment) {
    const std::array<char, 4> bytes{static_cast<char>(increment >> 24), static_cast<char>(increment >> 16),
        static_cast<char>(increment >> 8), static_cast<char>(increment)};
    return std::string(bytes.data(), bytes.size());
}

std::string priority_payload(std::uint32_t dependency, unsigned weight, bool exclusive) {
    auto payload = window_payload(dependency | (exclusive ? 0x80000000U : 0));
    payload.push_back(static_cast<char>(weight - 1));
    return payload;
}

std::string request_block() {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
    ruvia::hpack_encoder::encode_header(block, ":authority", "example.test");
    ruvia::hpack_encoder::encode_header(block, ":path", "/metadata");
    return block;
}

std::string server_preface() {
    std::string wire(ruvia::http2_client_preface);
    connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
    return wire;
}

}  // namespace http2_receive_metadata_test
using namespace http2_receive_metadata_test;

TEST(Ruvia, http2_receive_frame_metadata_off_produces_no_metadata_events) {
    for (const auto role : {ruvia::http2_role::server, ruvia::http2_role::client}) {
        auto connection = role == ruvia::http2_role::server ? ruvia::http2_connection::server()
                                                            : ruvia::http2_connection::client();
        std::string settings;
        append_setting(settings, 3, 13);
        append_setting(settings, 0xfefe, 1);
        std::string wire = role == ruvia::http2_role::server ? std::string(ruvia::http2_client_preface) : std::string{};
        connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
        connect_drain_append_frame(wire, ruvia::http2_frame_type::priority, 0, 7, priority_payload(0, 16, false));
        connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(5));
        if (role == ruvia::http2_role::server) {
            const auto block = request_block();
            auto headers = priority_payload(0, 32, true);
            headers.append(block.data(), block.size());
            connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x24, 1, headers);
            connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, window_payload(5));
        }
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        received_log log;
        log.drain(connection);
        EXPECT_TRUE(!log.overflow_);
        if (role == ruvia::http2_role::server) {
            EXPECT_EQ(log.count_, 1u);
            EXPECT_TRUE(log.records_[0].kind_ == received_kind::request_head);
            EXPECT_TRUE(log.records_[0].request_matches_);
        } else {
            EXPECT_EQ(log.count_, 0u);
        }
        EXPECT_TRUE(!connection.connection_error());
    }
}

// nghttp2 folds a repeated known identifier into its first position with the
// last value, keeps unknown identifiers in wire order, and owns the decoded
// entries, so the event outlives the input.
TEST(Ruvia, http2_receive_settings_fold_known_duplicates_and_keep_unknown_entries) {
    for (const auto role : {ruvia::http2_role::server, ruvia::http2_role::client}) {
        const ruvia::http2_connection_options options{.receive_frame_metadata_ = true};
        auto connection = role == ruvia::http2_role::server ? ruvia::http2_connection::server(options)
                                                            : ruvia::http2_connection::client(options);
        std::string payload;
        append_setting(payload, 0xfefe, 0xabcdef01);
        append_setting(payload, 3, 13);
        append_setting(payload, 3, 7);
        append_setting(payload, 0xfefe, 0);
        std::string wire = role == ruvia::http2_role::server ? std::string(ruvia::http2_client_preface) : std::string{};
        connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, payload);
        connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 1, 0);
        connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        wire.assign(wire.size(), '#');
        received_log log;
        log.drain(connection);
        EXPECT_EQ(log.count_, 2u);
        EXPECT_TRUE(!log.overflow_);
        const auto& first = log.records_[0];
        EXPECT_TRUE(first.kind_ == received_kind::settings);
        EXPECT_EQ(first.settings_count_, 3u);
        EXPECT_TRUE(first.checked_bounds_);
        EXPECT_EQ(static_cast<std::uint16_t>(first.settings_[0].id_), 0xfefe);
        EXPECT_EQ(first.settings_[0].value_, 0xabcdef01u);
        EXPECT_TRUE(first.settings_[1].id_ == ruvia::http2_setting_id::max_concurrent_streams);
        EXPECT_EQ(first.settings_[1].value_, 7u);
        EXPECT_EQ(static_cast<std::uint16_t>(first.settings_[2].id_), 0xfefe);
        EXPECT_EQ(first.settings_[2].value_, 0u);
        EXPECT_TRUE(log.records_[1].kind_ == received_kind::settings);
        EXPECT_EQ(log.records_[1].settings_count_, 0u);
        EXPECT_TRUE(log.records_[1].checked_bounds_);
    }
    const ruvia::http2_received_settings empty;
    bool threw = false;
    try {
        (void)empty.at((std::numeric_limits<std::size_t>::max)());
    } catch (const std::out_of_range&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, http2_receive_invalid_later_setting_does_not_publish_partial_frame) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    std::string wire = server_preface();
    std::string payload;
    append_setting(payload, 3, 19);
    append_setting(payload, 4, 0x80000000U);
    connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, payload);
    connect_drain_append_frame(wire, ruvia::http2_frame_type::priority, 0, 7, priority_payload(0, 1, false));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::flow_control_error);
    received_log log;
    log.drain(server);
    EXPECT_EQ(log.count_, 1u);
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
    EXPECT_EQ(log.records_[0].settings_count_, 0u);
}

TEST(Ruvia, http2_receive_byte_fragments_publish_only_complete_frames) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    received_log log;
    std::string wire = server_preface();
    const auto settings_end = wire.size();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(5));
    const auto first_window_end = wire.size();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(11));
    for (std::size_t index = 0; index != wire.size(); ++index) {
        const auto status = server.feed(std::string_view(wire).substr(index, 1));
        EXPECT_TRUE(status == ruvia::http2_feed_result::accepted);
        log.drain(server);
        const auto expected = static_cast<std::size_t>(index + 1 >= settings_end) +
                              static_cast<std::size_t>(index + 1 >= first_window_end) +
                              static_cast<std::size_t>(index + 1 == wire.size());
        EXPECT_EQ(log.count_, expected);
    }
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
    EXPECT_TRUE(log.records_[1].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[1].value_, 5u);
    EXPECT_TRUE(log.records_[2].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[2].value_, 11u);
    EXPECT_TRUE(!server.connection_error());
}

TEST(Ruvia, http2_receive_padded_headers_priority_precedes_continued_request_and_later_metadata) {
    for (const bool continued : {false, true}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
        received_log log;
        auto block = request_block();
        const auto split = continued ? block.size() / 2 : block.size();
        std::string payload(1, '\x03');
        payload += priority_payload(0, 256, true);
        payload.append(block.data(), split);
        payload.append(3, '\0');
        std::string wire = server_preface();
        connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(3));
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, continued ? 0x29 : 0x2d, 1, payload);
        if (continued) {
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            log.drain(server);
            EXPECT_EQ(log.count_, 3u);
            EXPECT_TRUE(server.header_block_in_progress());
            wire.clear();
            connect_drain_append_frame(wire, ruvia::http2_frame_type::continuation, 4, 1, std::string_view(block).substr(split));
        }
        // A PRIORITY frame publishes nothing (RFC 9113 §5.3.2 deprecates it).
        connect_drain_append_frame(wire, ruvia::http2_frame_type::priority, 0, 5, priority_payload(0, 1, false));
        connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(9));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        EXPECT_EQ(log.count_, 5u);
        EXPECT_TRUE(!log.overflow_);
        const std::array kinds{received_kind::settings, received_kind::window_update, received_kind::priority,
            received_kind::request_head, received_kind::window_update};
        for (std::size_t index = 0; index != kinds.size(); ++index) {
            EXPECT_TRUE(log.records_[index].kind_ == kinds[index]);
        }
        EXPECT_EQ(log.records_[1].value_, 3u);
        EXPECT_EQ(log.records_[2].stream_id_, 1u);
        EXPECT_EQ(log.records_[2].value_, 0u);
        EXPECT_EQ(log.records_[2].weight_, 256u);
        EXPECT_TRUE(log.records_[2].exclusive_);
        const auto& request = log.records_[3];
        EXPECT_TRUE(request.request_matches_);
        EXPECT_EQ(request.stream_id_, 1u);
        EXPECT_EQ(request.pseudo_count_, 4u);
        const std::array expected{ruvia::http2_request_pseudo_header::method, ruvia::http2_request_pseudo_header::scheme,
            ruvia::http2_request_pseudo_header::authority, ruvia::http2_request_pseudo_header::path};
        for (std::size_t index = 0; index != expected.size(); ++index) {
            EXPECT_TRUE(request.pseudo_order_[index] == expected[index]);
        }
        EXPECT_EQ(log.records_[4].value_, 9u);
        EXPECT_TRUE(!server.connection_error());
    }
}

TEST(Ruvia, http2_receive_window_updates_only_report_applied_credit) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    received_log log;
    auto block = request_block();
    std::string wire = server_preface();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 5, 3, block);
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    log.drain(server);
    EXPECT_EQ(log.count_, 2u);
    EXPECT_TRUE(log.records_[1].kind_ == received_kind::request_head);
    log.count_ = 0;
    wire.clear();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, window_payload(0));  // 跳过的闭流：忽略。
    connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 3, window_payload(17));
    connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(23));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    log.drain(server);
    EXPECT_EQ(log.count_, 2u);
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[0].stream_id_, 3u);
    EXPECT_EQ(log.records_[0].value_, 17u);
    EXPECT_TRUE(log.records_[1].kind_ == received_kind::window_update);
    EXPECT_EQ(log.records_[1].stream_id_, 0u);
    EXPECT_EQ(log.records_[1].value_, 23u);
    EXPECT_TRUE(!server.connection_error());
    // nghttp2 treats a zero increment on a live stream as a connection error.
    log.count_ = 0;
    wire.clear();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 3, window_payload(0));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    log.drain(server);
    EXPECT_EQ(log.count_, 0u);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
}

TEST(Ruvia, http2_receive_rejected_connection_window_and_malformed_priority_are_not_published) {
    for (const auto type : {ruvia::http2_frame_type::window_update, ruvia::http2_frame_type::priority}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
        std::string wire = server_preface();
        connect_drain_append_frame(wire, type, 0, type == ruvia::http2_frame_type::window_update ? 0 : 1, window_payload(0));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        received_log log;
        log.drain(server);
        EXPECT_EQ(log.count_, 1u);
        EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
        EXPECT_TRUE(server.connection_error() == (type == ruvia::http2_frame_type::window_update
                                                         ? ruvia::http2_error_code::protocol_error
                                                         : ruvia::http2_error_code::frame_size_error));
    }
}

TEST(Ruvia, http2_receive_control_metadata_needs_no_dynamic_allocation) {
    // PRIORITY frames are deprecated (RFC 9113 §5.3.2) and publish nothing.
    for (const bool publish : {false, true}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = publish});
        received_log log;
        const auto preface = server_preface();
        EXPECT_TRUE(server.feed(preface) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        log.count_ = 0;
        std::string priority;
        connect_drain_append_frame(priority, ruvia::http2_frame_type::priority, 0, 13, priority_payload(5, 19, false));
        std::string window;
        connect_drain_append_frame(window, ruvia::http2_frame_type::window_update, 0, 0, window_payload(7));
        EXPECT_TRUE(server.feed(priority) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        EXPECT_TRUE(server.feed(window) == ruvia::http2_feed_result::accepted);
        log.drain(server);
        EXPECT_EQ(log.count_, publish ? 1u : 0u);
        EXPECT_TRUE(!server.connection_error());
    }
}

TEST(Ruvia, http2_receive_malformed_headers_do_not_publish_priority_or_request) {
    // Invalid padding and a payload too short for the PRIORITY fields are both
    // reported by nghttp2 as connection PROTOCOL_ERROR.
    for (const bool missing_priority : {false, true}) {
        auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
        std::string wire = server_preface();
        std::string payload(1, missing_priority ? '\0' : '\x08');
        payload += priority_payload(0, 1, false);
        if (missing_priority) {
            payload.pop_back();
        }
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x2d, 1, payload);
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
        received_log log;
        log.drain(server);
        EXPECT_EQ(log.count_, 1u);
        EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
        EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
    }
}

TEST(Ruvia, http2_receive_window_overflow_does_not_publish_rejected_credit) {
    auto server = ruvia::http2_connection::server({.receive_frame_metadata_ = true});
    std::string wire = server_preface();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(0x7fffffffU));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    received_log log;
    log.drain(server);
    EXPECT_EQ(log.count_, 1u);
    EXPECT_TRUE(log.records_[0].kind_ == received_kind::settings);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::flow_control_error);
}

namespace http2_receive_metadata_test {
struct wire_observation_log final {
    std::vector<ruvia::http2_frame_metadata> frames_;
    struct field final {
        std::uint64_t first_;
        std::uint64_t frame_;
        std::size_t index_;
        std::string name_;
        std::string value_;
    };
    std::vector<field> fields_;
    std::size_t begins_{0};
    std::size_t ends_{0};

    static void frame(void* context, const ruvia::http2_frame_metadata& metadata) noexcept {
        static_cast<wire_observation_log*>(context)->frames_.push_back(metadata);
    }
    static void header(void* context, const ruvia::http2_header_metadata& metadata) noexcept {
        auto& log = *static_cast<wire_observation_log*>(context);
        switch (metadata.phase_) {
            case ruvia::http2_header_observation_phase::begin:
                ++log.begins_;
                break;
            case ruvia::http2_header_observation_phase::end:
                ++log.ends_;
                break;
            case ruvia::http2_header_observation_phase::field:
                log.fields_.push_back({metadata.first_frame_observation_sequence_, metadata.frame_observation_sequence_,
                    metadata.field_index_, std::string(metadata.name_), std::string(metadata.value_)});
                break;
        }
    }
};
}  // namespace http2_receive_metadata_test
using namespace http2_receive_metadata_test;

TEST(Ruvia, http2_wire_observers_preserve_raw_settings_priority_and_fragmented_hpack) {
    for (const auto fragment : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
        wire_observation_log observed;
        auto server = ruvia::http2_connection::server({.frame_observer_ = &wire_observation_log::frame,
            .header_observer_ = &wire_observation_log::header,
            .observer_context_ = &observed});
        std::string settings;
        append_setting(settings, 3, 17);
        append_setting(settings, 0xfafa, 123);
        append_setting(settings, 3, 29);
        append_setting(settings, 1, 1024);
        append_setting(settings, 1, 4096);
        std::string wire(ruvia::http2_client_preface);
        connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
        connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(7340033));
        connect_drain_append_frame(wire, ruvia::http2_frame_type::priority, 0, 3, priority_payload(0, 256, true));
        // RFC 7541 C.4.1: Huffman-coded authority inserted at dynamic index 62.
        const std::string_view block("\x82\x86\x84\x41\x8c\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 17);
        std::string payload(1, '\x03');
        payload += priority_payload(3, 42, false);
        payload += block.substr(0, 6);
        payload.append(3, '\0');
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x29, 1, payload);
        connect_drain_append_frame(wire, ruvia::http2_frame_type::continuation, 0x4, 1, block.substr(6));
        // A second block uses that dynamic-table entry in a different order.
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 3,
            std::string_view("\xbe\x84\x87\x82", 4));
        received_log requests;
        for (std::size_t cursor = 0; cursor < wire.size(); cursor += fragment) {
            EXPECT_TRUE(server.feed(std::string_view(wire).substr(cursor, fragment)) ==
                        ruvia::http2_feed_result::accepted);
            requests.drain(server);
        }
        ASSERT_EQ(observed.frames_.size(), 6u);
        for (std::size_t i = 0; i < observed.frames_.size(); ++i) {
            EXPECT_EQ(observed.frames_[i].observation_sequence_, i);
        }
        const auto& raw = observed.frames_[0];
        ASSERT_EQ(raw.settings().size(), 5u);
        EXPECT_TRUE(raw.settings_complete_);
        EXPECT_EQ(static_cast<std::uint16_t>(raw.settings()[1].id_), 0xfafa);
        EXPECT_EQ(raw.settings()[0].value_, 17u);
        EXPECT_EQ(raw.settings()[2].value_, 29u);
        EXPECT_EQ(raw.settings()[3].value_, 1024u);
        EXPECT_EQ(raw.settings()[4].value_, 4096u);
        ASSERT_TRUE(observed.frames_[1].window_increment_);
        EXPECT_EQ(*observed.frames_[1].window_increment_, 7340033u);
        ASSERT_TRUE(observed.frames_[2].priority_);
        EXPECT_EQ(observed.frames_[2].priority_->weight_, 256u);
        EXPECT_TRUE(observed.frames_[2].priority_->exclusive_);
        ASSERT_TRUE(observed.frames_[3].priority_);
        EXPECT_EQ(observed.frames_[3].priority_->stream_dependency_, 3u);
        EXPECT_EQ(observed.frames_[3].priority_->weight_, 42u);
        EXPECT_TRUE(observed.frames_[4].type_ == ruvia::http2_frame_type::continuation);
        ASSERT_EQ(observed.fields_.size(), 8u);
        EXPECT_EQ(observed.fields_[0].name_, ":method");
        EXPECT_EQ(observed.fields_[1].name_, ":scheme");
        EXPECT_EQ(observed.fields_[2].name_, ":path");
        EXPECT_EQ(observed.fields_[3].name_, ":authority");
        EXPECT_EQ(observed.fields_[3].value_, "www.example.com");
        EXPECT_EQ(observed.fields_[3].first_, 3u);
        EXPECT_EQ(observed.fields_[3].frame_, 4u);
        EXPECT_EQ(observed.fields_[4].name_, ":authority");
        EXPECT_EQ(observed.fields_[5].name_, ":path");
        EXPECT_EQ(observed.fields_[6].name_, ":scheme");
        EXPECT_EQ(observed.fields_[7].name_, ":method");
        EXPECT_EQ(observed.fields_[7].index_, 3u);
        EXPECT_EQ(observed.begins_, 2u);
        EXPECT_EQ(observed.ends_, 2u);
        EXPECT_FALSE(server.connection_error().has_value());
    }
}

TEST(Ruvia, http2_header_observer_sees_extended_connect_order_before_semantic_rejection) {
    for (const bool malformed : {false, true}) {
        wire_observation_log observed;
        auto server = ruvia::http2_connection::server({.header_observer_ = &wire_observation_log::header, .observer_context_ = &observed});
        std::string block;
        for (const auto& field : std::array{
                 std::pair{":protocol", "websocket"}, std::pair{":path", "/chat"},
                 std::pair{":scheme", "https"}, std::pair{":authority", "example.test:443"},
                 std::pair{":method", "CONNECT"}}) {
            ruvia::hpack_encoder::encode_header(block, field.first, field.second);
        }
        if (malformed) {
            ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
        }
        ruvia::hpack_encoder::encode_header(block, "sec-websocket-version", "13");
        auto wire = server_preface();
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
        ASSERT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        bool request_received = false;
        while (auto event = server.next_event()) {
            if (auto* request = event->request_head()) {
                request_received = true;
                const auto order = request->request_view().pseudo_header_order_;
                ASSERT_EQ(order.size(), 5u);
                EXPECT_TRUE(order[0] == ruvia::http2_request_pseudo_header::protocol);
                EXPECT_TRUE(order[4] == ruvia::http2_request_pseudo_header::method);
            }
        }
        EXPECT_EQ(request_received, !malformed);
        ASSERT_EQ(observed.fields_.size(), 6u);
        EXPECT_EQ(observed.fields_[0].name_, ":protocol");
        EXPECT_EQ(observed.fields_[4].name_, ":method");
        EXPECT_EQ(observed.fields_[5].name_, malformed ? ":method" : "sec-websocket-version");
        EXPECT_EQ(observed.fields_[0].first_, 1u);
        EXPECT_EQ(observed.frames_.size(), 0u);
    }
}

TEST(Ruvia, http2_frame_observer_includes_unknown_extensions_without_advertisement_semantics) {
    for (const auto role : {ruvia::http2_role::server, ruvia::http2_role::client}) {
        wire_observation_log observed;
        const ruvia::http2_connection_options options{
            .frame_observer_ = &wire_observation_log::frame, .observer_context_ = &observed};
        auto connection = role == ruvia::http2_role::server
                              ? ruvia::http2_connection::server(options)
                              : ruvia::http2_connection::client(options);
        std::string wire = role == ruvia::http2_role::server
                               ? std::string(ruvia::http2_client_preface)
                               : std::string{};
        connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
        connect_drain_append_frame(wire, static_cast<ruvia::http2_frame_type>(0xee), 0, 0, "unknown");
        // ORIGIN is not enabled, and servers never process ALTSVC. These
        // malformed opaque payloads must remain ignored with observation on.
        connect_drain_append_frame(wire, ruvia::http2_frame_type::origin, 0, 0, "not an origin list");
        if (role == ruvia::http2_role::server) {
            connect_drain_append_frame(wire, ruvia::http2_frame_type::alternative_service, 0, 0, "x");
        }
        connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(19));
        EXPECT_TRUE(connection.feed(wire) == ruvia::http2_feed_result::accepted);
        EXPECT_FALSE(connection.connection_error().has_value());
        while (auto event = connection.next_event()) {
            EXPECT_EQ(event->origin_advertisement(), nullptr);
            EXPECT_EQ(event->alternative_service_advertisement(), nullptr);
        }
        ASSERT_EQ(observed.frames_.size(), role == ruvia::http2_role::server ? 5u : 4u);
        EXPECT_EQ(static_cast<unsigned>(observed.frames_[1].type_), 0xeeu);
        EXPECT_EQ(observed.frames_.back().observation_sequence_, observed.frames_.size() - 1);
        ASSERT_TRUE(observed.frames_.back().window_increment_);
        EXPECT_EQ(*observed.frames_.back().window_increment_, 19u);
    }
}

TEST(Ruvia, http2_frame_observation_does_not_weaken_mandatory_protocol_rejection) {
    wire_observation_log observed;
    auto server = ruvia::http2_connection::server({.frame_observer_ = &wire_observation_log::frame, .observer_context_ = &observed});
    auto wire = server_preface();
    connect_drain_append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, window_payload(0));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::protocol_failure);
    EXPECT_TRUE(server.connection_error() == ruvia::http2_error_code::protocol_error);
    ASSERT_EQ(observed.frames_.size(), 2u);
    ASSERT_TRUE(observed.frames_[1].window_increment_);
    EXPECT_EQ(*observed.frames_[1].window_increment_, 0u);
}

TEST(Ruvia, http2_frame_observer_preserves_enabled_origin_and_alternative_service_decoding) {
    for (const bool observe : {false, true}) {
        for (const auto fragment : {std::size_t{1}, std::size_t{4096}}) {
            wire_observation_log observed;
            auto client = ruvia::http2_connection::client({.receive_origin_advertisements_ = true,
                .frame_observer_ = observe ? &wire_observation_log::frame : nullptr,
                .observer_context_ = &observed});
            const std::array<std::string_view, 1> origins{"https://example.test"};
            auto origin_frame = ruvia::encode_http2_origin_frame(origins);
            auto alternative_frame = ruvia::encode_http2_alternative_service_frame(0, origins[0], "h3=\":443\"");
            ASSERT_TRUE(std::holds_alternative<std::vector<char>>(origin_frame));
            ASSERT_TRUE(std::holds_alternative<std::vector<char>>(alternative_frame));
            const auto& origin_bytes = std::get<std::vector<char>>(origin_frame);
            const auto& alternative_bytes = std::get<std::vector<char>>(alternative_frame);
            std::string wire;
            connect_drain_append_frame(wire, ruvia::http2_frame_type::settings, 0, 0);
            wire.append(origin_bytes.data(), origin_bytes.size());
            connect_drain_append_frame(wire, static_cast<ruvia::http2_frame_type>(0xee), 0, 0, "discard me");
            wire.append(alternative_bytes.data(), alternative_bytes.size());
            std::size_t origin_count = 0;
            std::size_t alternative_count = 0;
            for (std::size_t cursor = 0; cursor < wire.size(); cursor += fragment) {
                ASSERT_TRUE(client.feed(std::string_view(wire).substr(cursor, fragment)) ==
                            ruvia::http2_feed_result::accepted);
                while (auto event = client.next_event()) {
                    if (const auto* origin = event->origin_advertisement()) {
                        ++origin_count;
                        ASSERT_EQ(origin->origins_.size(), 1u);
                        EXPECT_TRUE(origin->origins_[0] == origins[0]);
                    }
                    if (const auto* alternative = event->alternative_service_advertisement()) {
                        ++alternative_count;
                        EXPECT_EQ(alternative->stream_id_, 0u);
                        EXPECT_TRUE(alternative->origin_ == origins[0]);
                        EXPECT_TRUE(alternative->field_value_ == "h3=\":443\"");
                    }
                }
            }
            EXPECT_EQ(origin_count, 1u);
            EXPECT_EQ(alternative_count, 1u);
            EXPECT_FALSE(client.connection_error().has_value());
            ASSERT_EQ(observed.frames_.size(), observe ? 4u : 0u);
            if (observe) {
                EXPECT_TRUE(observed.frames_[1].type_ == ruvia::http2_frame_type::origin);
                EXPECT_EQ(observed.frames_[1].observation_sequence_, 1u);
                EXPECT_TRUE(observed.frames_[3].type_ == ruvia::http2_frame_type::alternative_service);
                EXPECT_EQ(observed.frames_[3].observation_sequence_, 3u);
            }
        }
    }
}

namespace {

void connect_drain_submit_buffered_response(
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
    connect_drain_submit_buffered_response(server, 2, response);
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
                EXPECT_TRUE(request.headers_.size() == 1 &&
                            request.headers_[0].name() == "x-owned" && request.headers_[0].value() == value);
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
        (void)ruvia::test::drain_output(client);
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
        const auto reset_wire = ruvia::test::drain_output(client);
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
    ruvia::test::output_relay relay;
    for (const bool enabled : {false, true}) {
        auto client = ruvia::http2_connection::client({.enable_push_ = enabled});
        auto server = ruvia::http2_connection::server({});
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
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/"});
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
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(second_blocked) &&
                std::get<ruvia::http2_response_head_submit_error>(second_blocked) ==
                    ruvia::http2_response_head_submit_error::peer_stream_limit_reached);
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
        connect_drain_submit_buffered_response(server, 1, response);
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
            const auto committed =
                server.submit_streaming_response_head(std::get<0>(first).stream_id(), ruvia::http_response{});
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
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_head) &&
                std::get<ruvia::http2_response_head_submit_error>(closed_head) ==
                    ruvia::http2_response_head_submit_error::closed);
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
                    connect_drain_submit_buffered_response(server, std::get<0>(push).stream_id(), response);
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
    auto connect_client = ruvia::http2_connection::client({.enable_push_ = true});
    auto connect_server = ruvia::http2_connection::server({});
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

    std::string push_block;
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
    auto drain_client = ruvia::http2_connection::client({.enable_push_ = true});
    auto drain_server = ruvia::http2_connection::server({});
    relay.transfer(drain_client, drain_server);
    relay.transfer(drain_server, drain_client);
    relay.transfer(drain_client, drain_server);
    const auto parent_id = drain_client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test", .target_ = "/"});
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(drain_promise).first(9),
        static_cast<std::uint32_t>(4 + dyn_push_block.size()), ruvia::http2_frame_type::push_promise, 0x4, 1));
    drain_promise[12] = 2;
    drain_promise.append(dyn_push_block);
    EXPECT_TRUE(drain_client.feed(drain_promise) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!drain_client.next_event());
    EXPECT_TRUE(ruvia::test::drain_output(drain_client).empty());

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
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
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
    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    response.body("done");
    connect_drain_submit_buffered_response(server, std::get<0>(pushed).stream_id(), response);

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
    (void)ruvia::test::drain_output(server);

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
        (void)ruvia::test::drain_output(server);
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
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
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
    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    EXPECT_TRUE(server.submit_connect_response_head(1, response) ==
                ruvia::http2_submit_status::accepted);
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(promise).first(9),
        static_cast<std::uint32_t>(4 + push_block.size()), ruvia::http2_frame_type::push_promise, 0x4, 1));
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
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
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
        (void)ruvia::test::drain_output(server);
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
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto sub1 = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({.enable_push_ = true});
    auto server = ruvia::http2_connection::server({});
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(partial_frame).first(9),
        static_cast<std::uint32_t>(partial_block.size()), ruvia::http2_frame_type::headers, 0x1, 1));
    partial_frame.append(partial_block);
    EXPECT_TRUE(server.feed(partial_frame) == ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::invalid_state);
    std::array<char, 9> cont{};
    EXPECT_TRUE(ruvia::encode_http2_frame_header(cont, 0, ruvia::http2_frame_type::continuation, 0x4, 1));
    EXPECT_TRUE(server.feed(std::string_view(cont.data(), cont.size())) == ruvia::http2_feed_result::accepted);
    auto req1 = server.next_event();
    (void)server.next_event();
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::accepted);

    auto pushed = server.submit_push_request(1, {.authority_ = "example.test", .path_ = "/pushed"});
    EXPECT_TRUE(pushed.index() == 0);
    // Pushed stream in reserved (local) state cannot send ALTSVC before response headers open it.
    // An interim response is a response HEADERS frame too (RFC 9113 §8.4.2): the
    // stream is half-closed (remote) afterwards and the server may advertise on it.
    std::uint32_t pushed_id = 0;
    if (pushed.index() == 0) {
        pushed_id = std::get<0>(pushed).stream_id();
        EXPECT_TRUE(server.submit_alternative_service_advertisement(pushed_id, "", "h3=\":443\"") ==
                    ruvia::http2_submit_status::invalid_state);
        EXPECT_TRUE(server.submit_interim_response_head(pushed_id, ruvia::http_interim_response_head(ruvia::http_status::early_hints)) ==
                    ruvia::http2_submit_status::accepted);
        EXPECT_TRUE(server.submit_alternative_service_advertisement(pushed_id, "", "h3=\":443\"") ==
                    ruvia::http2_submit_status::accepted);
        (void)server.release(std::move(std::get<0>(pushed)));
    }

    ruvia::http_response resp;
    resp.status(ruvia::http_status::no_content);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        server.submit_response_head(1, resp, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, resp))));
    // Stream 1 is now closed (retained only by req1 lease); ALTSVC submission must be rejected.
    EXPECT_TRUE(server.submit_alternative_service_advertisement(1, "", "h3=\":443\"") ==
                ruvia::http2_submit_status::invalid_state);
    if (req1 && req1->request_head()) {
        (void)server.release(std::move(*req1->request_head()));
    }
    relay.transfer(server, client);
    bool pushed_altsvc_seen = false;
    while (auto event = client.next_event()) {
        if (const auto* advertisement = event->alternative_service_advertisement();
            advertisement != nullptr && advertisement->stream_id_ == pushed_id) {
            pushed_altsvc_seen = advertisement->field_value_ == "h3=\":443\"";
        }
    }
    EXPECT_TRUE(pushed_altsvc_seen);
    EXPECT_TRUE(!client.connection_error());

    // 2. Open CONNECT tunnel ignores ALTSVC on stream (RFC 7838 section 4 / RFC 9113
    // section 5.5 discard) and rejects server submission.
    (void)client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    const auto connect_submitted = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect_submitted));
    const auto connect_id = std::get<std::uint32_t>(connect_submitted);
    relay.transfer(client, server);
    std::optional<ruvia::http2_request_head_event> req3_lease;
    std::optional<ruvia::http2_request_head_event> connect_lease;
    while (auto ev = server.next_event()) {
        if (auto* rh = ev->request_head()) {
            if (rh->stream_id() == connect_id) {
                EXPECT_TRUE(server.submit_alternative_service_advertisement(connect_id, "", "h3=\":443\"") ==
                            ruvia::http2_submit_status::invalid_state);
                ruvia::http_response ok_resp;
                ok_resp.status(ruvia::http_status::ok);
                EXPECT_TRUE(server.submit_connect_response_head(connect_id, ok_resp) == ruvia::http2_submit_status::accepted);
                EXPECT_TRUE(server.submit_alternative_service_advertisement(connect_id, "", "h3=\":443\"") ==
                            ruvia::http2_submit_status::invalid_state);
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
    EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(altsvc_on_tunnel).first(9),
        8, ruvia::http2_frame_type::alternative_service, 0, connect_id));
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
        EXPECT_TRUE(ruvia::encode_http2_frame_header(std::span(altsvc_on_reserved).first(9),
            8, ruvia::http2_frame_type::alternative_service, 0, pushed2_id));
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
    auto client = ruvia::http2_connection::client(
        {.enable_push_ = true, .receive_origin_advertisements_ = true});
    auto server = ruvia::http2_connection::server({});
    relay.transfer(client, server);
    relay.transfer(server, client);
    relay.transfer(client, server);

    const auto connect_submitted = client.submit_request_head(
        ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
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
    EXPECT_TRUE(server.submit_connect_response_head(connect_id, ok_resp) ==
                ruvia::http2_submit_status::accepted);
    relay.transfer(server, client);
    while (client.next_event()) {
    }

    const auto altsvc_tunnel =
        std::get<0>(ruvia::encode_http2_alternative_service_frame(connect_id, "", "h3=\":443\"", 16384));
    const auto altsvc_idle =
        std::get<0>(ruvia::encode_http2_alternative_service_frame(99, "", "h3=\":443\"", 16384));
    EXPECT_TRUE(client.feed(std::string_view(altsvc_tunnel.data(), altsvc_tunnel.size())) ==
                ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.next_event().has_value());
    EXPECT_TRUE(client.feed(std::string_view(altsvc_idle.data(), altsvc_idle.size())) ==
                ruvia::http2_feed_result::accepted);
    EXPECT_TRUE(!client.next_event().has_value());
    EXPECT_TRUE(!client.connection_error().has_value());

    const auto get_submitted = client.submit_request_head(
        ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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
    ruvia::test::output_relay relay;
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
    ruvia::test::output_relay relay;
    auto client = ruvia::http2_connection::client({});
    auto server = ruvia::http2_connection::server({});
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
        ruvia::http_response resp;
        resp.status(ruvia::http_status::no_content);
        (void)server.submit_response_head(
            1, resp, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, resp));
        (void)server.release(std::move(*req_head));
    }
}

namespace {

using ruvia::http2_cleartext_preface_probe;
using ruvia::http2_client_preface;
using ruvia::probe_http2_cleartext_preface;

}  // namespace

TEST(Ruvia, http2_cleartext_preface_accepts_only_prior_knowledge) {
    EXPECT_TRUE(probe_http2_cleartext_preface("GET / HTTP/1.1\r\n") ==
                http2_cleartext_preface_probe::http1);
    EXPECT_TRUE(probe_http2_cleartext_preface("") == http2_cleartext_preface_probe::http1);
    EXPECT_TRUE(probe_http2_cleartext_preface("PRI * HT") == http2_cleartext_preface_probe::need_more_preface);
    EXPECT_TRUE(probe_http2_cleartext_preface(http2_client_preface) ==
                http2_cleartext_preface_probe::complete_preface);
    EXPECT_TRUE(probe_http2_cleartext_preface("PRI /not-http2\r\n") ==
                http2_cleartext_preface_probe::drop_connection);
}

TEST(Ruvia, frame_public_codec_preserves_wire_bytes_and_buffer_boundaries) {
    std::array<char, 11> storage{};
    storage.fill('#');
    auto output = std::span(storage).subspan(1, ruvia::http2_frame_header_bytes);
    EXPECT_TRUE(ruvia::encode_http2_frame_header(output, 0xabcdef,
        static_cast<ruvia::http2_frame_type>(0xfe), 0x81, 0x7f123456));
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
        const auto result_value = ruvia::decode_http_origin_advertisement(
            std::span(std::get<0>(frame)).subspan(ruvia::http2_frame_header_bytes));
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
        const auto decoded = ruvia::decode_http2_alternative_service(stream_id,
            std::span(std::get<0>(result_value)).subspan(ruvia::http2_frame_header_bytes));
        ASSERT_EQ(decoded.index(), 0u);
        EXPECT_EQ(std::get<0>(decoded).origin_, origin);
        EXPECT_EQ(std::get<0>(decoded).field_value_, value);

        const auto limited = ruvia::encode_http2_alternative_service_frame(stream_id, origin, value, size - 1);
        ASSERT_NE(limited.index(), 0u);
        EXPECT_EQ(std::get<1>(limited), ruvia::http_connection_advertisement_error::limit);
    }
    const auto http2_limited = ruvia::encode_http2_origin_frame(origins,
        static_cast<std::uint32_t>(payload_bytes - 1));
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
    auto prepared = writer.prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), {.method_ = "GET", .target_ = "/ws"}, request_buffer);
    ruvia::http1_client_response_parser parser(
        std::move(std::get<ruvia::prepared_http1_client_request>(prepared)).take_exchange_state());
    auto parsed = parser.parse("HTTP/1.1 200 OK\r\n" + std::string(fields) + "\r\n");
    if (parsed.parsed() == nullptr) {
        throw std::runtime_error("expected parsed response");
    }
    return std::move(*parsed.parsed()).take_head();
}

// The server's selection on an open Extended CONNECT stream.
std::variant<ruvia::websocket_client_negotiation_result_view, ruvia::websocket_client_handshake_error> open_stream_selection(
    const ruvia::websocket_client_negotiation& negotiation, const ruvia::http_client_response_head& head) {
    return ruvia::validate_extended_connect_websocket_client_response(negotiation, head, true);
}

template <typename head_type>
concept ws_client_negotiation_accepts_response_head =
    requires(const ruvia::websocket_client_negotiation& negotiation, head_type&& head) {
        ruvia::validate_extended_connect_websocket_client_response(negotiation, std::forward<head_type>(head), true);
    };

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
        const auto connect_drain_append_frame = [&](std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
                                                    std::uint32_t stream_id, std::string_view payload) {
            std::array<char, ruvia::http2_frame_header_bytes> header{};
            EXPECT_TRUE(ruvia::encode_http2_frame_header(
                header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
            wire.append(header.data(), header.size());
            wire.append(payload);
        };
        std::string settings_wire;
        constexpr std::array<char, 6> enable_connect_protocol{0x00, 0x08, 0x00, 0x00, 0x00, 0x01};
        connect_drain_append_frame(settings_wire, ruvia::http2_frame_type::settings, 0, 0,
            std::string_view(enable_connect_protocol.data(), enable_connect_protocol.size()));
        EXPECT_TRUE(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }

        ruvia::websocket_client_negotiation negotiation({});
        const auto submitted =
            ruvia::submit_http2_websocket_client_request(connection, negotiation, "https", "example.test", "/ws");
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        std::string block;
        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
        ruvia::hpack_encoder::encode_header(block, name, value);
        std::string wire;
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, 0x4, std::get<std::uint32_t>(submitted), block);
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
        EXPECT_TRUE(ruvia::encode_http2_frame_header(
            header, static_cast<std::uint32_t>(enable_connect_protocol.size()),
            ruvia::http2_frame_type::settings, 0, 0));
        settings_wire.append(header.data(), header.size());
        settings_wire.append(enable_connect_protocol.data(), enable_connect_protocol.size());
        EXPECT_TRUE(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }
        ruvia::websocket_client_negotiation negotiation({});
        const std::array<ruvia::http_header_view, 1> borrowed_headers{{{"x-trace", "borrowed"}}};
        const auto submitted = ruvia::submit_http2_websocket_client_request(
            connection, negotiation, "https", "example.test", "/ws", borrowed_headers);
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
        EXPECT_TRUE(std::holds_alternative<ruvia::websocket_client_handshake_error>(res) &&
                    std::get<ruvia::websocket_client_handshake_error>(res) ==
                        ruvia::websocket_client_handshake_error::forbidden_field);
    }
}

// RFC 8441 section 5: END_STREAM after the 2xx is the server's orderly tunnel
// close; only END_STREAM on the response HEADERS leaves no WebSocket stream.
// The fact is fixed by the HEADERS frame even when the DATA that ends the
// stream arrives in the same feed().
TEST(Ruvia, websocket_h2_client_response_head_reports_end_stream_only_from_headers) {
    for (const bool end_on_headers : {false, true}) {
        auto connection = ruvia::http2_connection::client({});
        const auto connect_drain_append_frame = [&](std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
                                                    std::uint32_t stream_id, std::string_view payload) {
            std::array<char, ruvia::http2_frame_header_bytes> header{};
            EXPECT_TRUE(ruvia::encode_http2_frame_header(
                header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id));
            wire.append(header.data(), header.size());
            wire.append(payload);
        };
        std::string settings_wire;
        constexpr std::array<char, 6> enable_connect_protocol{0x00, 0x08, 0x00, 0x00, 0x00, 0x01};
        connect_drain_append_frame(settings_wire, ruvia::http2_frame_type::settings, 0, 0,
            std::string_view(enable_connect_protocol.data(), enable_connect_protocol.size()));
        EXPECT_TRUE(connection.feed(settings_wire) == ruvia::http2_feed_result::accepted);
        while (connection.next_event()) {
        }
        ruvia::websocket_client_negotiation negotiation({});
        const auto submitted =
            ruvia::submit_http2_websocket_client_request(connection, negotiation, "https", "example.test", "/ws");
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            return;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        std::string block;
        ruvia::hpack_encoder::encode_status(block, ruvia::http_status::ok);
        std::string wire;
        connect_drain_append_frame(wire, ruvia::http2_frame_type::headers, end_on_headers ? 0x5 : 0x4, stream_id, block);
        if (!end_on_headers) {
            // The server's Close frame (1000, "bye") with END_STREAM.
            connect_drain_append_frame(wire, ruvia::http2_frame_type::data, 0x1, stream_id, std::string_view("\x88\x05\x03\xe8"
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
        const auto negotiated =
            ruvia::validate_extended_connect_websocket_client_response(negotiation, head->head(), !head->stream_ended());
        if (end_on_headers) {
            EXPECT_TRUE(std::holds_alternative<ruvia::websocket_client_handshake_error>(negotiated) &&
                        std::get<ruvia::websocket_client_handshake_error>(negotiated) ==
                            ruvia::websocket_client_handshake_error::stream_closed);
        } else {
            EXPECT_TRUE(negotiated.index() == 0);
            auto data = connection.next_event();
            EXPECT_TRUE(data && data->tunnel_data() != nullptr);
            auto end = connection.next_event();
            EXPECT_TRUE(end && end->tunnel_end() != nullptr);
        }
    }
}
