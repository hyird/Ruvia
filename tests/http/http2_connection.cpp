#include "http2_connection.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_decoding_fixture.h"
#include "http_response.h"
#include "http_response_server.h"

namespace {

template <typename head_type, typename body_type>
constexpr bool can_build_http2_request = requires(head_type&& head, body_type&& body) { std::forward<head_type>(head).build_request(std::forward<body_type>(body)); };

static_assert(can_build_http2_request<ruvia::http2_request_head_event&, std::string&>);
static_assert(can_build_http2_request<const ruvia::http2_request_head_event&, const std::string&>);
static_assert(can_build_http2_request<ruvia::http2_request_head_event&, std::string_view>);
static_assert(can_build_http2_request<ruvia::http2_request_head_event&, const char (&)[5]>);
static_assert(!can_build_http2_request<ruvia::http2_request_head_event&, std::string>);
static_assert(!can_build_http2_request<ruvia::http2_request_head_event&, const std::string>);
static_assert(!can_build_http2_request<ruvia::http2_request_head_event, std::string_view>);
static_assert(!can_build_http2_request<const ruvia::http2_request_head_event, std::string_view>);

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload_value) {
    std::array<char, ruvia::http2_frame_header_bytes> header_value{};
    if (!ruvia::encode_http2_frame_header(header_value, static_cast<std::uint32_t>(payload_value.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid test HTTP/2 frame");
    }
    wire.append(header_value.data(), header_value.size());
    wire.append(payload_value);
}

void append_header_section(std::string& wire, std::uint32_t stream_id, std::string_view block, bool end_stream, bool fragmented) {
    const auto flags = static_cast<std::uint8_t>(end_stream ? 0x1 : 0);
    if (!fragmented) {
        append_frame(wire, ruvia::http2_frame_type::headers, static_cast<std::uint8_t>(flags | 0x4), stream_id, block);
        return;
    }
    const auto split = block.size() / 2;
    append_frame(wire, ruvia::http2_frame_type::headers, flags, stream_id, block.substr(0, split));
    append_frame(wire, ruvia::http2_frame_type::continuation, 0x4, stream_id, block.substr(split));
}

void append_peer_settings(std::string& wire) {
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
}

void append_response(std::string& wire, std::uint32_t stream_id, std::string_view status, std::string_view body = {}, std::string_view trailer_name = {}, std::string_view trailer_value = {}) {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":status", status);
    const auto head_flags = static_cast<std::uint8_t>(0x4 | (body.empty() && trailer_name.empty() ? 0x1 : 0));
    append_frame(wire, ruvia::http2_frame_type::headers, head_flags, stream_id, block);
    if (!body.empty()) {
        append_frame(wire, ruvia::http2_frame_type::data, static_cast<std::uint8_t>(trailer_name.empty() ? 0x1 : 0), stream_id, body);
    }
    if (!trailer_name.empty()) {
        block.clear();
        ruvia::hpack_encoder::encode_header(block, trailer_name, trailer_value);
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, stream_id, block);
    }
}

std::string client_response_wire(std::string_view body, bool include_header = false, bool end_stream = true) {
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

std::string server_request_wire(std::string_view body, std::string_view method = "POST", std::string_view expectation = {}, bool end_stream = true) {
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
        append_frame(wire, ruvia::http2_frame_type::data, end_stream ? 0x1 : 0, 1, body);
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
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
    if (!std::holds_alternative<std::uint32_t>(submitted)) {
        throw std::logic_error("test request was not submitted");
    }
    (void)ruvia::test::drain_output(client);
    return client;
}

ruvia::http2_connection prepared_client_method(std::string_view method) {
    auto client = ruvia::http2_connection::client({});
    (void)ruvia::test::drain_output(client);
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = method, .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
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

ruvia::http2_submit_status submit_buffered_response(ruvia::http2_connection& server, std::uint32_t stream_id, const ruvia::http_response& response) {
    std::variant<ruvia::http_buffered_response_write_plan, ruvia::http2_response_head_submit_error> head = ruvia::http2_response_head_submit_error::response_plan_mismatch;
    for (const auto method : {ruvia::http_known_method::get, ruvia::http_known_method::post, ruvia::http_known_method::options, ruvia::http_known_method::head, ruvia::http_known_method::put, ruvia::http_known_method::delete_value, ruvia::http_known_method::patch, ruvia::http_known_method::connect, ruvia::http_known_method::unknown}) {
        head = server.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(method, response));
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
        const auto data = server.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, response.body_bytes().size());
        if (data.status_ == ruvia::http2_data_submit_status::closed) {
            return ruvia::http2_submit_status::closed;
        }
        if (data.status_ != ruvia::http2_data_submit_status::complete || data.consumed_bytes_ != response.body_bytes().size()) {
            return ruvia::http2_submit_status::invalid_state;
        }
    }
    return ruvia::http2_submit_status::accepted;
}
}  // namespace

TEST(Ruvia, http2_public_request_submission_exposes_one_exclusive_success_or_failure_contract) {
    auto client = ruvia::http2_connection::client();
    const auto invalid = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "", .authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_request_head_submit_error>(invalid));
    if (const auto* failure = std::get_if<ruvia::http2_request_head_submit_error>(&invalid)) {
        EXPECT_TRUE(*failure == ruvia::http2_request_head_submit_error::invalid_message);
    }
    const auto extended = client.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "websocket", .authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_request_head_submit_error>(extended));
    if (const auto* failure = std::get_if<ruvia::http2_request_head_submit_error>(&extended)) {
        EXPECT_TRUE(*failure == ruvia::http2_request_head_submit_error::peer_capability_unavailable);
    }
    const auto regular = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(regular));
    if (const auto* submitted = std::get_if<std::uint32_t>(&regular)) {
        EXPECT_EQ(*submitted, std::uint32_t{1});
    }
    const auto connect = client.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "example.test:443"});
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(connect));
    if (const auto* submitted = std::get_if<std::uint32_t>(&connect)) {
        EXPECT_EQ(*submitted, std::uint32_t{3});
    }
    auto server = ruvia::http2_connection::server();
    const auto wrong_role = server.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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

    EXPECT_TRUE(client.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::closed);
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

    EXPECT_TRUE(client.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
    EXPECT_TRUE(credit.valid());
    (void)ruvia::http2_received_data_credit(std::move(credit));
    EXPECT_TRUE(!credit.valid());
    EXPECT_TRUE(!client.connection_error());
}

TEST(Ruvia, http2_public_peer_reset_preserves_outstanding_data_credit) {
    auto client = prepared_client();
    auto wire = client_response_wire("x", false, false);
    constexpr std::array<char, 4> cancel_payload{0, 0, 0, 8};
    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view(cancel_payload.data(), cancel_payload.size()));
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
    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view(refused_payload.data(), refused_payload.size()));
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
    EXPECT_TRUE(std::holds_alternative<std::uint32_t>(client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"})));

    auto started = prepared_client();
    auto started_wire = client_response_wire("", false, false);
    append_frame(started_wire, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view(refused_payload.data(), refused_payload.size()));
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
    for (const auto crumbs : {std::array<std::string_view, 3>{"", "a=b", "c=d"}, std::array<std::string_view, 3>{"a=b", "", "c=d"}, std::array<std::string_view, 3>{"a=b", "c=d", ""}}) {
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
    EXPECT_TRUE(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
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
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view(zero_window.data(), zero_window.size()));
        EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
        ruvia::http_response response;
        EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(server.submit_streaming_response_head(1, std::move(response))));
        const auto blocked = server.submit_data(1, "blocked", ruvia::http2_end_stream::keep_open, 7);
        EXPECT_TRUE(blocked.status_ == ruvia::http2_data_submit_status::backpressured);
        EXPECT_EQ(blocked.consumed_bytes_, std::size_t{0});
        EXPECT_TRUE(server.send_window_state(1).has_value());
        EXPECT_EQ(server.pending_data_output_bytes(1), std::size_t{0});

        if (peer_reset) {
            constexpr std::array<char, 4> cancel{0, 0, 0, 8};
            wire.clear();
            append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, 1, std::string_view(cancel.data(), cancel.size()));
            EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
            const auto closed = server.next_event();
            EXPECT_TRUE(closed && closed->stream_closed());
            EXPECT_TRUE(!server.next_event());
        } else {
            EXPECT_TRUE(server.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
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
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view(zero_window.data(), zero_window.size()));
    EXPECT_TRUE(server.feed(wire) == ruvia::http2_feed_result::accepted);
    ruvia::http_response response;
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(server.submit_streaming_response_head(1, std::move(response))));
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
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 1, std::string_view(increment.data(), increment.size()));
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
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(server.submit_streaming_response_head(1, std::move(response))));

    const auto head_output = ruvia::test::drain_output(server);
    const auto head = ruvia::parse_http2_frame_header(std::span<const char>(head_output.data(), head_output.size()));
    EXPECT_TRUE(head.has_value());
    EXPECT_TRUE(head && head->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    EXPECT_TRUE(head && head->stream_id_ == 1);
    EXPECT_TRUE(head && (head->flags_ & 0x1U) == 0);

    EXPECT_TRUE(server.submit_data(1, "event: update\n\ndata: ok\n\n", ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
    const auto data_output = ruvia::test::drain_output(server);
    const auto data = ruvia::parse_http2_frame_header(std::span<const char>(data_output.data(), data_output.size()));
    EXPECT_TRUE(data.has_value());
    EXPECT_TRUE(data && data->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
    EXPECT_TRUE(data && (data->flags_ & 0x1U) != 0);

    EXPECT_TRUE(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
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
    const auto committed = server.submit_streaming_response_head(1, std::move(response), ruvia::http_response_trailer_intent::present);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
    if (const auto* plan = std::get_if<ruvia::http_response_stream_commit_plan>(&committed)) {
        EXPECT_TRUE(plan->framing() == ruvia::http_response_stream_framing::http2_frames);
        EXPECT_TRUE(plan->head_disposition() == ruvia::http_response_stream_head_disposition::body_open);
        EXPECT_TRUE(plan->trailers_available());
    }
    ruvia::http_response duplicate;
    const auto rejected = server.submit_streaming_response_head(1, std::move(duplicate), ruvia::http_response_trailer_intent::none);
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(rejected));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(rejected) && std::get<ruvia::http2_response_head_submit_error>(rejected) == ruvia::http2_response_head_submit_error::invalid_state);
    (void)ruvia::test::drain_output(server);

    const std::array<ruvia::http_header_view, 1> fields_value{{{"x-final", "done"}}};
    const auto trailers = ruvia::validate_http_response_trailers(fields_value);
    EXPECT_TRUE(server.submit_data(1, "body", ruvia::http2_end_stream::keep_open, 65535).status_ == ruvia::http2_data_submit_status::complete);
    EXPECT_TRUE(server.finish_response(1, trailers) == ruvia::http2_finish_status::accepted);
    std::string frame_bytes;
    EXPECT_TRUE(server.take_output_batch(1, frame_bytes).status_ == ruvia::http2_output_batch_status::taken);
    const auto trailer_frame = ruvia::parse_http2_frame_header(std::span<const char>(frame_bytes.data(), frame_bytes.size()));
    EXPECT_TRUE(trailer_frame && trailer_frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data));
    frame_bytes.clear();
    EXPECT_TRUE(server.take_output_batch(1, frame_bytes).status_ == ruvia::http2_output_batch_status::taken);
    const auto terminal = ruvia::parse_http2_frame_header(std::span<const char>(frame_bytes.data(), frame_bytes.size()));
    EXPECT_TRUE(terminal && terminal->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    EXPECT_TRUE(terminal && (terminal->flags_ & 0x1U) != 0);
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)server.finish_response(1, trailers);
    }));
    EXPECT_TRUE(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_retried_data_and_trailers_keep_hpack_state_across_window_reopen) {
    for (const auto value_size : {ruvia::http2_default_max_frame_size - 12, ruvia::http2_default_max_frame_size - 11}) {
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
            append_frame(window_settings, ruvia::http2_frame_type::settings, 0, 0, std::string_view(zero_window.data(), zero_window.size()));
            EXPECT_TRUE(server.feed(window_settings) == ruvia::http2_feed_result::accepted);
            EXPECT_TRUE(!server.next_event());
            (void)ruvia::test::drain_output(server);
            ruvia::http_response response;
            response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
            const auto committed = server.submit_streaming_response_head(1, std::move(response), ruvia::http_response_trailer_intent::present);
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
            append_frame(table_settings, ruvia::http2_frame_type::settings, 0, 0, std::string_view(zero_table.data(), zero_table.size()));
            EXPECT_TRUE(server.feed(table_settings) == ruvia::http2_feed_result::accepted);
            EXPECT_TRUE(!server.next_event());
            (void)ruvia::test::drain_output(server);
            constexpr std::array<char, 4> increment{0, 0, 0, 1};
            std::string window_update;
            append_frame(window_update, ruvia::http2_frame_type::window_update, 0, 1, std::string_view(increment.data(), increment.size()));
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
                EXPECT_TRUE(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
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
    const auto committed = server.submit_streaming_response_head(1, std::move(response), ruvia::http_response_trailer_intent::none);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed));
    EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(committed) && std::get<ruvia::http_response_stream_commit_plan>(committed).head_disposition() == ruvia::http_response_stream_head_disposition::message_ended);
    const auto output = ruvia::test::drain_output(server);
    const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
    EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    EXPECT_TRUE(frame && (frame->flags_ & 0x1U) != 0);
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)server.submit_data(1, "", ruvia::http2_end_stream::end_stream, 65535);
    }));
    EXPECT_TRUE(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
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
                    const auto found = std::find_if(fields.begin(), fields.end(), [name](const auto& field) {
                        return field.name() == name;
                    });
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
        const auto plan = ruvia::plan_buffered_http_response_write(request->request_head()->build_request().known_method(), response);
        const auto submitted = server.submit_response_head(1, response, plan);
        EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
        (void)server.release(std::move(*request->request_head()));
        const auto response_wire = ruvia::test::drain_output(server);
        EXPECT_TRUE(client.feed(response_wire) == ruvia::http2_feed_result::accepted);
        const auto head = client.next_event();
        EXPECT_TRUE(head && head->response_head());
        if (head && head->response_head()) {
            const auto fields = head->response_head()->head().headers();
            const auto found = std::find_if(fields.begin(), fields.end(), [](const auto& field) {
                return field.name() == "content-length";
            });
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
    const auto wrong_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::head, response);
    const auto mismatch = server.submit_response_head(1, response, wrong_plan);
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(mismatch));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(mismatch) && std::get<ruvia::http2_response_head_submit_error>(mismatch) == ruvia::http2_response_head_submit_error::response_plan_mismatch);
    const auto write_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::post, response);
    const auto submitted = server.submit_response_head(1, response, write_plan);
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted));
    if (const auto* plan = std::get_if<ruvia::http_buffered_response_write_plan>(&submitted)) {
        EXPECT_TRUE(plan->body_plan().response_status() == ruvia::http_status::ok);
        EXPECT_TRUE(plan->send_body());
        EXPECT_TRUE(plan->content_length() == 7);
    }
    const auto head_output = ruvia::test::drain_output(server);
    const auto head = ruvia::parse_http2_frame_header(std::span<const char>(head_output.data(), head_output.size()));
    EXPECT_TRUE(head && head->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers));
    EXPECT_TRUE(head && (head->flags_ & 0x1U) == 0);
    EXPECT_TRUE(server.submit_data(1, "payload", ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);

    EXPECT_TRUE(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_response_head_submit_exposes_shared_failure_values) {
    auto closed = ruvia::http2_connection::server({});
    ruvia::http_response closed_response;
    const auto closed_buffered = closed.submit_response_head(1, closed_response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, closed_response));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_buffered));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_buffered) && std::get<ruvia::http2_response_head_submit_error>(closed_buffered) == ruvia::http2_response_head_submit_error::closed);
    const auto closed_streaming = closed.submit_streaming_response_head(1, ruvia::http_response(), ruvia::http_response_trailer_intent::none);
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_streaming));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(closed_streaming) && std::get<ruvia::http2_response_head_submit_error>(closed_streaming) == ruvia::http2_response_head_submit_error::closed);

    auto client = prepared_client();
    ruvia::http_response client_response;
    const auto invalid_state = client.submit_streaming_response_head(1, std::move(client_response), ruvia::http_response_trailer_intent::none);
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_state));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_state) && std::get<ruvia::http2_response_head_submit_error>(invalid_state) == ruvia::http2_response_head_submit_error::invalid_state);

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
    const auto invalid_message = server.submit_streaming_response_head(1, std::move(invalid_response), ruvia::http_response_trailer_intent::none);
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_message));
    EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(invalid_message) && std::get<ruvia::http2_response_head_submit_error>(invalid_message) == ruvia::http2_response_head_submit_error::invalid_message);
    EXPECT_TRUE(!server.wants_write());
    EXPECT_TRUE(server.release(std::move(*request_head)) == ruvia::http2_server_request_release_status::released);
}

TEST(Ruvia, http2_public_response_head_validates_stable_representation_metadata) {
    for (const auto invalid : {ruvia::http_header_view{"Content-Type", "text plain"}, ruvia::http_header_view{"Content-Encoding", "gzip;q=1"}, ruvia::http_header_view{"Trailer", "Content-Length"}}) {
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
                const auto rejected = server.submit_streaming_response_head(1, std::move(response), ruvia::http_response_trailer_intent::none);
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
                const auto committed = server.submit_streaming_response_head(1, std::move(response), ruvia::http_response_trailer_intent::none);
                const auto* plan = std::get_if<ruvia::http_response_stream_commit_plan>(&committed);
                EXPECT_TRUE(plan != nullptr);
                if (plan != nullptr && plan->head_disposition() == ruvia::http_response_stream_head_disposition::body_open) {
                    EXPECT_TRUE(server.submit_data(1, {}, ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
                }
            } else {
                const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
                EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(server.submit_response_head(1, response, plan)));
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
                EXPECT_TRUE(std::none_of(fields.begin(), fields.end(), [](const auto& field) {
                    return field.name() == "transfer-encoding";
                }));
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
                const auto rejected = server.submit_streaming_response_head(1, std::move(response), ruvia::http_response_trailer_intent::none);
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
    const auto frame = ruvia::parse_http2_frame_header(std::span<const char>(output.data(), output.size()));
    EXPECT_TRUE(frame.has_value());
    EXPECT_TRUE(frame && frame->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
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
    const auto reset = ruvia::parse_http2_frame_header(std::span<const char>(output.data() + settings_ack_bytes, output.size() - settings_ack_bytes));
    EXPECT_TRUE(reset && reset->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream));
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
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)other.release(std::move(*escaped->request_head()));
    }));
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
        append_frame(frame, type, 0, 0, type == ruvia::http2_frame_type::ping ? std::string_view("12345678") : std::string_view{});
        for (std::size_t count = 0; count < 1000; ++count) {
            EXPECT_TRUE(client.feed(frame) == ruvia::http2_feed_result::accepted);
        }
        std::string pending;
        for (std::size_t count = 0; count < 1000; ++count) {
            append_frame(pending, type, 0x1, 0, type == ruvia::http2_frame_type::ping ? std::string_view("12345678") : std::string_view{});
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
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
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
        EXPECT_TRUE(server.submit_reset(stream_id, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::accepted);
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
                const auto submitted = server.submit_response_head(valid_stream_id, response, ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
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
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/"});
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
            append_frame(settings, ruvia::http2_frame_type::settings, 0, 0, blocked_window ? std::string_view(zero_window.data(), zero_window.size()) : std::string_view{});
            EXPECT_TRUE(client.feed(settings) == ruvia::http2_feed_result::accepted);
            const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::streaming()});
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
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .content_ = ruvia::http_request_content_plan::streaming()});
        EXPECT_TRUE(std::holds_alternative<std::uint32_t>(submitted));
        if (!std::holds_alternative<std::uint32_t>(submitted)) {
            continue;
        }
        const auto stream_id = std::get<std::uint32_t>(submitted);
        if (peer_goaway) {
            constexpr std::array<char, 8> payload{0, 0, 0, 1, 0, 0, 0, 0};
            std::string goaway;
            append_frame(goaway, ruvia::http2_frame_type::goaway, 0, 0, std::string_view(payload.data(), payload.size()));
            EXPECT_TRUE(client.feed(goaway) == ruvia::http2_feed_result::accepted);
        } else {
            client.begin_drain();
        }
        EXPECT_TRUE(!client.connection_error());
        (void)ruvia::test::drain_output(client);
        EXPECT_TRUE(client.submit_data(stream_id, "content", ruvia::http2_end_stream::end_stream, 65535).status_ == ruvia::http2_data_submit_status::complete);
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
                EXPECT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(server.submit_streaming_response_head(1, std::move(response))));
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
            EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(buffered_head) && std::get<ruvia::http2_response_head_submit_error>(buffered_head) == ruvia::http2_response_head_submit_error::closed);
            const auto streamed_head = server.submit_streaming_response_head(1, std::move(streamed), ruvia::http_response_trailer_intent::none);
            EXPECT_TRUE(std::holds_alternative<ruvia::http2_response_head_submit_error>(streamed_head) && std::get<ruvia::http2_response_head_submit_error>(streamed_head) == ruvia::http2_response_head_submit_error::closed);
            EXPECT_TRUE(submit_buffered_response(server, 1, buffered) == ruvia::http2_submit_status::closed);
            EXPECT_TRUE(server.submit_connect_response_head(1, buffered) == ruvia::http2_submit_status::closed);
            EXPECT_TRUE(server.submit_interim_response_head(1, interim) == ruvia::http2_submit_status::closed);
            EXPECT_TRUE(server.finish_response(1, trailers) == ruvia::http2_finish_status::closed);
            const auto websocket = server.submit_websocket_handshake(1, built_request, validation, {});
            EXPECT_TRUE(std::holds_alternative<ruvia::http2_websocket_handshake_submit_error>(websocket) && std::get<ruvia::http2_websocket_handshake_submit_error>(websocket) == ruvia::http2_websocket_handshake_submit_error::closed);
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
        EXPECT_TRUE(end->message_end()->content_semantics() == ruvia::http2_message_content_semantics::content);
    }
    EXPECT_TRUE(!client.next_event());
    EXPECT_TRUE(!client.connection_error());
}

TEST(Ruvia, http2_public_informational_metadata_does_not_frame_or_leak_into_final_responses) {
    for (const std::string_view length : {"0", "123"}) {
        auto client = prepared_client();
        const auto following = client.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "example.test"});
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
        append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, std::string_view(payload.data(), payload.size()));
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
    for (const auto& [status, value] : {std::pair{std::string_view("600"), 600}, std::pair{std::string_view("099"), 99}, std::pair{std::string_view("000"), 0}}) {
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
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/two"});
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
        EXPECT_TRUE(end->message_end()->content_semantics() == ruvia::http2_message_content_semantics::metadata_only);
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
            ruvia::hpack_encoder::encode_header(block, "content-length", std::to_string(bulk.size() * bulk_frames));
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
        EXPECT_TRUE(head && head->response_head() && head->response_head()->head().status() == ruvia::http_status::reset_content);
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
        EXPECT_TRUE(end->message_end()->content_semantics() == ruvia::http2_message_content_semantics::metadata_only);
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
    EXPECT_TRUE(end->message_end()->content_semantics() == ruvia::http2_message_content_semantics::content);
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
        EXPECT_TRUE(goaway_error(ruvia::test::drain_output(server)) == static_cast<std::uint32_t>(ruvia::http2_error_code::protocol_error));
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
        EXPECT_TRUE(goaway_error(ruvia::test::drain_output(server)) == static_cast<std::uint32_t>(ruvia::http2_error_code::enhance_your_calm));
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
    EXPECT_TRUE(client.submit_reset(1, ruvia::http2_error_code::cancel) == ruvia::http2_submit_status::closed);
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

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)first_credit.merge(std::move(second_credit));
    }));
    EXPECT_TRUE(first_credit.valid());
    EXPECT_TRUE(second_credit.valid());
    (void)ruvia::http2_received_data_credit(std::move(first_credit));
    (void)ruvia::http2_received_data_credit(std::move(second_credit));
    EXPECT_TRUE(!first_client.connection_error());
    EXPECT_TRUE(!second_client.connection_error());
}

TEST(Ruvia, http2_public_data_credit_merge_rejects_different_stream_unchanged) {
    auto client = prepared_client();
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .target_ = "/two"});
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

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        first_credit.merge(std::move(second_credit));
    }));
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
    EXPECT_TRUE(end->message_end()->content_semantics() == ruvia::http2_message_content_semantics::content);
    EXPECT_TRUE(end->message_end()->trailers().size() == 1);
    auto trailers = std::move(*end->message_end()).take_trailers();
    EXPECT_TRUE(trailers.size() == 1);
    if (!trailers.empty()) {
        EXPECT_TRUE(trailers.front().name() == "x-request-trace");
        EXPECT_TRUE(trailers.front().value() == "done");
    }
    EXPECT_TRUE(server.release(std::move(*request->request_head())) == ruvia::http2_server_request_release_status::released);
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
    EXPECT_TRUE(server.release(std::move(*request->request_head())) == ruvia::http2_server_request_release_status::released);
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
                    kept_checksum = first_trailers != 0 && end->trailers().front().name() == "x-checksum" && end->trailers().front().value() == "kept";
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
