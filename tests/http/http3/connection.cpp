#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
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

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"

#include "http3_relay.h"
#include "test_exceptions.h"

namespace {

template <typename bytes_type>
constexpr bool can_read_http3_stream = requires(ruvia::http3_connection& connection, bytes_type&& bytes) {
    connection.read_stream(0, std::forward<bytes_type>(bytes), false, 0);
};

template <typename connection_type>
constexpr bool can_read_http3_stream_braced_bytes = requires(connection_type& connection) {
    connection.read_stream(0, {std::byte{1}, std::byte{2}}, false, 0);
};

// DATA events come only from the codec, and their credit is linear.
static_assert(!std::is_constructible_v<ruvia::http3_data_event, std::int64_t, std::string_view, bool>);
static_assert(!std::is_copy_constructible_v<ruvia::http3_received_data_credit>);
static_assert(!std::is_copy_assignable_v<ruvia::http3_received_data_credit>);
static_assert(!std::is_move_assignable_v<ruvia::http3_received_data_credit>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::http3_received_data_credit>);
static_assert(!std::is_default_constructible_v<ruvia::http3_received_data_credit>);

static_assert(can_read_http3_stream<std::vector<std::byte>&>);
static_assert(can_read_http3_stream<const std::vector<std::byte>&>);
static_assert(can_read_http3_stream<std::array<std::byte, 3>&>);
static_assert(can_read_http3_stream<const std::array<std::byte, 3>&>);
static_assert(can_read_http3_stream<std::byte (&)[3]>);
static_assert(can_read_http3_stream<const std::byte (&)[3]>);
static_assert(can_read_http3_stream<std::span<std::byte>>);
static_assert(can_read_http3_stream<std::span<const std::byte>>);
static_assert(can_read_http3_stream<std::span<std::byte, 3>>);
static_assert(can_read_http3_stream<std::span<const std::byte, 3>>);
static_assert(can_read_http3_stream<ruvia::borrowed_span<std::byte>>);
static_assert(can_read_http3_stream<const ruvia::borrowed_span<std::byte>>);
static_assert(can_read_http3_stream<ruvia::borrowed_span<std::byte>&>);
static_assert(!can_read_http3_stream<std::vector<std::byte>>);
static_assert(!can_read_http3_stream<const std::vector<std::byte>>);
static_assert(!can_read_http3_stream<std::array<std::byte, 3>>);
static_assert(!can_read_http3_stream<const std::array<std::byte, 3>>);
static_assert(!can_read_http3_stream<std::initializer_list<std::byte>>);
static_assert(!can_read_http3_stream<std::initializer_list<std::byte>&>);
static_assert(!can_read_http3_stream<const std::initializer_list<std::byte>&>);
static_assert(!can_read_http3_stream_braced_bytes<ruvia::http3_connection>);

constexpr ruvia::http3_regular_request_head_view get_request_head{
    .authority_ = "example.test", .target_ = "/resource"};

[[nodiscard]] auto submit_no_content_response(ruvia::http3_connection& connection, std::int64_t stream_id) {
    ruvia::http_response response;
    response.status(ruvia::http_status::no_content);
    response.header("Server", "ruvia");
    return connection.submit_response_head(stream_id, response,
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response));
}

// RFC 9114 §8.1 reserves 0x1f * N + 0x21 for greasing; a receiver keeps the
// value and treats it as H3_NO_ERROR.
constexpr auto reserved_error = static_cast<ruvia::http3_error_code>(0x1f * 7 + 0x21);

}  // namespace

TEST(Ruvia, http3_transport_feedback_blocks_and_resumes_request_output) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ruvia::test::http3_event_log received;
    ASSERT_TRUE(peers.to_server(received));
    ASSERT_TRUE(peers.to_client(received));
    ASSERT_EQ(peers.client_.submit_request_head(0, get_request_head), ruvia::http3_submit_status::accepted);
    ASSERT_EQ(peers.client_.block_stream(0), ruvia::http3_transport_status::accepted);
    ASSERT_TRUE(peers.to_server(received));
    EXPECT_TRUE(received.headers_.empty());
    ASSERT_EQ(peers.client_.unblock_stream(0), ruvia::http3_transport_status::accepted);
    ASSERT_TRUE(peers.to_server(received));
    ASSERT_EQ(received.headers_.size(), std::size_t{1});
    EXPECT_EQ(received.headers_[0].stream_id_, 0);
    EXPECT_TRUE(received.ended(0));
}

TEST(Ruvia, http3_transport_feedback_preserves_direction_and_peer_error) {
    for (const bool receive_reset : {false, true}) {
        SCOPED_TRACE(receive_reset);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log received;
        ASSERT_TRUE(peers.send_request(0, get_request_head, false, received));
        auto& connection = peers.client_;
        const auto status = receive_reset
                                ? connection.shutdown_stream_read(0, ruvia::http3_error_code::request_cancelled)
                                : connection.shutdown_stream_write(0, ruvia::http3_error_code::request_cancelled);
        ASSERT_EQ(status, ruvia::http3_transport_status::accepted);
        auto event = connection.next_event();
        ASSERT_TRUE(event.has_value());
        const auto* action = event->transport_action();
        ASSERT_NE(action, nullptr);
        EXPECT_EQ(action->origin(), ruvia::http3_transport_action_origin::peer);
        EXPECT_EQ(action->stream_id(), 0);
        EXPECT_EQ(action->error(), ruvia::http3_error_code::request_cancelled);
        EXPECT_EQ(action->action(), receive_reset ? ruvia::http3_transport_action::reset_stream : ruvia::http3_transport_action::stop_sending);
        if (receive_reset) {
            EXPECT_EQ(connection.finish_stream(0), ruvia::http3_finish_status::accepted);
        } else {
            EXPECT_EQ(connection.finish_stream(0), ruvia::http3_finish_status::closed);
            ASSERT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
                submit_no_content_response(peers.server_, 0)));
            ruvia::test::http3_event_log response;
            ASSERT_TRUE(peers.to_client(response));
            EXPECT_TRUE(response.ended(0));
        }
        ASSERT_EQ(connection.shutdown_stream_read(0), ruvia::http3_transport_status::accepted);
        EXPECT_FALSE(connection.next_event().has_value());
        EXPECT_EQ(connection.close_stream(0, std::nullopt, std::nullopt), ruvia::http3_transport_status::accepted);
    }
}

TEST(Ruvia, http3_nghttp3_client_server_exchange_headers_and_stream_end) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_EQ(request.headers_.size(), std::size_t{1});
    if (request.headers_.size() != 1) {
        return;
    }
    EXPECT_EQ(request.headers_[0].stream_id_, std::int64_t{0});
    // The four pseudo-header fields, then the request's Host from :authority.
    EXPECT_EQ(request.headers_[0].fields_.size(), std::size_t{5});
    EXPECT_TRUE(request.headers_[0].stream_ended_);
    EXPECT_TRUE(request.headers_[0].field(":path") == std::string_view("/resource"));
    EXPECT_TRUE(request.headers_[0].field("host") == std::string_view("example.test"));
    EXPECT_TRUE(request.ended(0));
    // A message without a trailer section ends with an empty one.
    EXPECT_TRUE(request.end(0) != nullptr && request.end(0)->trailers_.empty());
    EXPECT_EQ(request.sequence_, std::string("HE"));

    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        submit_no_content_response(peers.server_, 0)));
    ruvia::test::http3_event_log response;
    EXPECT_TRUE(peers.to_client(response));
    EXPECT_EQ(response.headers_.size(), std::size_t{1});
    if (response.headers_.size() != 1) {
        return;
    }
    EXPECT_EQ(response.headers_[0].stream_id_, std::int64_t{0});
    EXPECT_TRUE(response.headers_[0].field("server") == "ruvia");
    EXPECT_TRUE(response.headers_[0].stream_ended_);
    EXPECT_TRUE(response.headers_[0].status_ && response.headers_[0].status_->value() == 204);
    EXPECT_TRUE(response.ended(0));
    EXPECT_EQ(response.sequence_, std::string("HE"));
}

struct http3_credit_sink final {
    static void record(void* context, std::int64_t stream_id, std::size_t bytes) noexcept {
        static_cast<http3_credit_sink*>(context)->returned_.emplace_back(stream_id, bytes);
    }
    [[nodiscard]] std::size_t total(std::int64_t stream_id) const noexcept {
        std::size_t bytes = 0;
        for (const auto& [id, count] : returned_) {
            bytes += id == stream_id ? count : 0;
        }
        return bytes;
    }
    std::vector<std::pair<std::int64_t, std::size_t>> returned_;
};

[[nodiscard]] bool submit_post(ruvia::test::http3_peers& peers, std::int64_t stream_id, std::string_view content) {
    return peers.client_.submit_request_head(stream_id, ruvia::http3_regular_request_head_view{
                                                            .method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::known_length(content.size())}) == ruvia::http3_submit_status::accepted &&
           peers.client_.submit_data(stream_id, ruvia::test::http3_bytes(content)).consumed_bytes_ == content.size() && peers.client_.finish_stream(stream_id) == ruvia::http3_finish_status::accepted;
}

TEST(Ruvia, http3_received_data_credit_returns_once_when_released) {
    http3_credit_sink sink;
    ruvia::test::http3_peers peers({.own_data_events_ = true,
        .received_data_credit_sink_ = &http3_credit_sink::record,
        .received_data_credit_context_ = &sink});
    ASSERT_TRUE(peers.bind());
    ASSERT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{
                                                         .method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming()}) == ruvia::http3_submit_status::accepted);
    std::vector<ruvia::http3_received_data_credit> credits;
    for (const std::string_view chunk : {"abc", "de"}) {
        ASSERT_EQ(peers.client_.submit_data(0, ruvia::test::http3_bytes(chunk)).consumed_bytes_, chunk.size());
        ASSERT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
            if (auto* data = event.data()) {
                credits.push_back(data->take_credit());
                // A taken credit leaves none behind in the event.
                EXPECT_FALSE(data->take_credit().valid());
            }
        }));
    }
    ASSERT_EQ(credits.size(), std::size_t{2});
    EXPECT_TRUE(sink.returned_.empty());
    credits[0].merge(std::move(credits[1]));
    EXPECT_FALSE(credits[1].valid());
    EXPECT_TRUE(sink.returned_.empty());
    EXPECT_TRUE(ruvia::test::throws_on([&] { credits[0].merge(std::move(credits[1])); }));
    auto moved = std::move(credits[0]);
    EXPECT_FALSE(credits[0].valid());
    credits.clear();
    EXPECT_TRUE(sink.returned_.empty());
    {
        const auto released = std::move(moved);
    }
    ASSERT_EQ(sink.returned_.size(), std::size_t{1});
    EXPECT_EQ(sink.returned_[0], (std::pair<std::int64_t, std::size_t>{0, 5}));
}

TEST(Ruvia, http3_received_data_credits_of_different_streams_do_not_merge) {
    http3_credit_sink sink;
    std::optional<ruvia::http3_received_data_credit> first;
    std::optional<ruvia::http3_received_data_credit> second;
    {
        ruvia::test::http3_peers peers({.received_data_credit_sink_ = &http3_credit_sink::record,
            .received_data_credit_context_ = &sink});
        ASSERT_TRUE(peers.bind());
        ASSERT_TRUE(submit_post(peers, 0, "ab"));
        ASSERT_TRUE(submit_post(peers, 4, "cde"));
        ASSERT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
            if (auto* data = event.data()) {
                (data->stream_id() == 0 ? first : second).emplace(data->take_credit());
            }
        }));
        ASSERT_TRUE(first.has_value() && second.has_value());
        EXPECT_TRUE(ruvia::test::throws_on([&] { first->merge(std::move(*second)); }));
        EXPECT_TRUE(first->valid() && second->valid());
        first.reset();
        EXPECT_EQ(sink.total(0), std::size_t{2});
        EXPECT_EQ(sink.total(4), std::size_t{0});
    }
    // A credit that outlives its codec returns nothing.
    second.reset();
    EXPECT_EQ(sink.total(4), std::size_t{0});
}

TEST(Ruvia, http3_data_event_move_assignment_returns_the_target_credit) {
    http3_credit_sink sink;
    ruvia::test::http3_peers peers({.own_data_events_ = true,
        .received_data_credit_sink_ = &http3_credit_sink::record,
        .received_data_credit_context_ = &sink});
    ASSERT_TRUE(peers.bind());
    ASSERT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{
                                                         .method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming()}) == ruvia::http3_submit_status::accepted);
    std::optional<ruvia::http3_data_event> retained;
    for (const std::string_view chunk : {"abc", "de"}) {
        ASSERT_EQ(peers.client_.submit_data(0, ruvia::test::http3_bytes(chunk)).consumed_bytes_, chunk.size());
        ASSERT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
            if (auto* data = event.data()) {
                if (retained) {
                    *retained = std::move(*data);
                } else {
                    retained.emplace(std::move(*data));
                }
            }
        }));
    }
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->bytes(), "de");
    EXPECT_EQ(sink.total(0), std::size_t{3});
    retained.reset();
    EXPECT_EQ(sink.total(0), std::size_t{5});
}

TEST(Ruvia, http3_nghttp3_streaming_body_is_owned_until_acknowledged) {
    ruvia::test::http3_peers peers({.own_data_events_ = true});
    EXPECT_TRUE(peers.bind());

    constexpr std::array fields{ruvia::http_header_view{"Content-Type", "application/octet-stream"}};
    EXPECT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{
                                                         .method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .headers_ = fields, .content_ = ruvia::http_request_content_plan::known_length(3)}) == ruvia::http3_submit_status::accepted);
    const auto submitted = peers.client_.submit_data(0, ruvia::test::http3_bytes("abc"));
    EXPECT_TRUE(submitted.status_ == ruvia::http3_data_submit_status::complete);
    EXPECT_EQ(submitted.consumed_bytes_, std::size_t{3});
    EXPECT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_finish_status::accepted);

    // The relay scrubs every delivered input buffer, so owned DATA is the
    // only way the payload can still read "abc" after delivery.
    std::optional<ruvia::http3_data_event> retained;
    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        request(event);
        if (auto* data = event.data()) {
            retained.emplace(std::move(*data));
        }
    }));

    EXPECT_EQ(request.headers_.size(), std::size_t{1});
    EXPECT_TRUE(retained.has_value() && retained->stream_id() == 0 && retained->bytes() == "abc");
    EXPECT_EQ(request.data(0), std::string("abc"));
    EXPECT_TRUE(request.ended(0));
    EXPECT_EQ(request.sequence_, std::string("HDE"));
}

// A request stream that QUIC reports closed is published once, with the
// application error code only when the closure carried one.
TEST(Ruvia, http3_close_stream_publishes_stream_close_event) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_TRUE(peers.send_request(4, get_request_head, true, request));
    EXPECT_TRUE(request.ended(0) && request.ended(4));
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        submit_no_content_response(peers.server_, 0)));
    ruvia::test::http3_event_log response;
    EXPECT_TRUE(peers.to_client(response));
    EXPECT_TRUE(response.ended(0));

    EXPECT_TRUE(peers.server_.close_stream(0, std::nullopt, std::nullopt) == ruvia::http3_transport_status::accepted);
    ruvia::test::http3_event_log closed;
    while (auto event = peers.server_.next_event()) {
        closed(*event);
    }
    EXPECT_EQ(closed.closes_.size(), std::size_t{1});
    if (closed.closes_.size() == 1) {
        EXPECT_EQ(closed.closes_[0].stream_id(), std::int64_t{0});
        EXPECT_FALSE(closed.closes_[0].error().has_value());
    }

    // An unknown code keeps its exact value (RFC 9114 §9).
    EXPECT_TRUE(peers.server_.close_stream(4, reserved_error, std::nullopt) == ruvia::http3_transport_status::accepted);
    closed.clear();
    while (auto event = peers.server_.next_event()) {
        closed(*event);
    }
    EXPECT_EQ(closed.closes_.size(), std::size_t{1});
    if (closed.closes_.size() == 1) {
        EXPECT_EQ(closed.closes_[0].stream_id(), std::int64_t{4});
        EXPECT_EQ(closed.closes_[0].error(), std::optional(reserved_error));
        EXPECT_EQ(static_cast<std::uint64_t>(*closed.closes_[0].error()), std::uint64_t{0x1f * 7 + 0x21});
    }
    EXPECT_EQ(closed.sequence_, std::string("C"));

    // A closed stream accepts no further response.
    EXPECT_TRUE(std::holds_alternative<ruvia::http3_response_head_error>(
        submit_no_content_response(peers.server_, 0)));
}

// RFC 9114 §5.2 graceful shutdown: the notice still admits in-flight
// requests, the final GOAWAY names the first unprocessed request stream, and
// the server is drained only once every admitted request stream has closed.
TEST(Ruvia, http3_server_drain_sends_notice_then_final_goaway) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log request;
    EXPECT_TRUE(peers.send_request(0, get_request_head, true, request));
    EXPECT_TRUE(request.ended(0));

    EXPECT_TRUE(peers.server_.begin_drain() == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(!peers.server_.drain_final());
    EXPECT_TRUE(!peers.server_.drained());
    ruvia::test::http3_event_log client;
    EXPECT_TRUE(peers.to_client(client));
    // The shutdown notice bounds no stream (RFC 9114 §5.2).
    EXPECT_EQ(client.goaways_.size(), std::size_t{1});
    EXPECT_TRUE(!client.goaways_.empty() && !client.goaways_[0].has_value());
    EXPECT_TRUE(client.unprocessed_.empty());

    EXPECT_TRUE(peers.server_.finish_drain() == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.server_.drain_final());
    EXPECT_TRUE(!peers.server_.drained());
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_EQ(client.goaways_.size(), std::size_t{2});
    EXPECT_TRUE(client.goaways_.size() == 2 && client.goaways_[1] == std::optional<std::int64_t>(4));
    EXPECT_TRUE(client.unprocessed_.empty());
    // Stream 0 was admitted before the final GOAWAY and remains open.
    EXPECT_TRUE(!peers.server_.drained());

    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        submit_no_content_response(peers.server_, 0)));
    EXPECT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.ended(0));
    EXPECT_TRUE(!peers.server_.drained());

    EXPECT_TRUE(peers.server_.close_stream(0, std::nullopt, std::nullopt) == ruvia::http3_transport_status::accepted);
    while (auto event = peers.server_.next_event()) {
        (void)event;
    }
    EXPECT_TRUE(peers.server_.drained());
    // A repeated final drain is idempotent.
    EXPECT_TRUE(peers.server_.finish_drain() == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.server_.drain_final());
}

TEST(Ruvia, http3_peer_termination_keeps_unknown_error_codes) {
    for (const bool receive_reset : {false, true}) {
        SCOPED_TRACE(receive_reset);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, false, server));
        // The server sees the client's RESET_STREAM or STOP_SENDING.
        const auto status = receive_reset ? peers.server_.shutdown_stream_read(0, reserved_error)
                                          : peers.server_.shutdown_stream_write(0, reserved_error);
        ASSERT_EQ(status, ruvia::http3_transport_status::accepted);
        ruvia::test::http3_event_log log;
        while (auto event = peers.server_.next_event()) {
            log(*event);
        }
        ASSERT_EQ(log.transport_actions_.size(), std::size_t{1});
        const auto& action = log.transport_actions_[0];
        EXPECT_EQ(action.stream_id(), 0);
        EXPECT_EQ(action.origin(), ruvia::http3_transport_action_origin::peer);
        EXPECT_EQ(action.action(), receive_reset ? ruvia::http3_transport_action::reset_stream
                                                 : ruvia::http3_transport_action::stop_sending);
        EXPECT_EQ(static_cast<std::uint64_t>(action.error()), std::uint64_t{0x1f * 7 + 0x21});
        EXPECT_EQ(peers.server_.close_stream(0, reserved_error, reserved_error), ruvia::http3_transport_status::accepted);
        log.clear();
        while (auto event = peers.server_.next_event()) {
            log(*event);
        }
        ASSERT_EQ(log.closes_.size(), std::size_t{1});
        EXPECT_EQ(log.closes_[0].error(), std::optional(reserved_error));
    }
}

[[nodiscard]] bool has_local_cancel(const ruvia::test::http3_event_log& log, std::int64_t stream_id,
    ruvia::http3_transport_action action) {
    for (const auto& event : log.transport_actions_) {
        if (event.stream_id() == stream_id && event.action() == action &&
            event.origin() == ruvia::http3_transport_action_origin::local &&
            event.error() == ruvia::http3_error_code::request_cancelled) {
            return true;
        }
    }
    return false;
}

// RFC 9114 §5.2: request streams at or above the final GOAWAY identifier were
// not processed and may be retried; the client cancels what is still open and
// no later event names them.
TEST(Ruvia, http3_client_reports_requests_beyond_goaway_as_unprocessed) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.send_request(0, get_request_head, true, server));
    // Streams 4 and 8 never reach the server before its final GOAWAY.
    ASSERT_EQ(peers.client_.submit_request_head(4, get_request_head), ruvia::http3_submit_status::accepted);
    ASSERT_EQ(peers.client_.submit_request_head(8, get_request_head), ruvia::http3_submit_status::accepted);
    ASSERT_EQ(peers.server_.begin_drain(), ruvia::http3_submit_status::accepted);
    ruvia::test::http3_event_log client;
    ASSERT_TRUE(peers.to_client(client));
    // The shutdown notice names no stream, so every request may still finish.
    EXPECT_EQ(client.sequence_, std::string("G"));

    ASSERT_EQ(peers.server_.finish_drain(), ruvia::http3_submit_status::accepted);
    client.clear();
    ASSERT_TRUE(peers.to_client(client));
    ASSERT_EQ(client.goaways_.size(), std::size_t{1});
    EXPECT_EQ(client.goaways_[0], std::optional<std::int64_t>(4));
    EXPECT_EQ(client.unprocessed_, (std::vector<std::int64_t>{4, 8}));
    EXPECT_EQ(client.sequence_, std::string("GUTTUTT"));
    for (const std::int64_t stream_id : {4, 8}) {
        SCOPED_TRACE(stream_id);
        EXPECT_TRUE(has_local_cancel(client, stream_id, ruvia::http3_transport_action::stop_sending));
        EXPECT_TRUE(has_local_cancel(client, stream_id, ruvia::http3_transport_action::reset_stream));
    }

    // Stream 0 was admitted and still completes.
    ASSERT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        submit_no_content_response(peers.server_, 0)));
    client.clear();
    ASSERT_TRUE(peers.to_client(client));
    EXPECT_TRUE(client.ended(0));
    EXPECT_TRUE(client.unprocessed_.empty());

    // QUIC retires the cancelled streams without another event naming them.
    for (const std::int64_t stream_id : {4, 8}) {
        EXPECT_EQ(peers.client_.close_stream(stream_id, std::nullopt, ruvia::http3_error_code::request_cancelled),
            ruvia::http3_transport_status::accepted);
    }
    EXPECT_EQ(peers.client_.close_stream(0, std::nullopt, std::nullopt), ruvia::http3_transport_status::accepted);
    client.clear();
    while (auto event = peers.client_.next_event()) {
        client(*event);
    }
    ASSERT_EQ(client.closes_.size(), std::size_t{1});
    EXPECT_EQ(client.closes_[0].stream_id(), 0);
}

// RFC 9114 §4.1.1: H3_REQUEST_REJECTED before the final response head means
// the server did not process the request; after it, the termination is an
// ordinary peer notification.
TEST(Ruvia, http3_client_reports_rejected_requests_as_unprocessed_only_before_the_final_head) {
    for (const bool peer_reset : {true, false}) {
        SCOPED_TRACE(peer_reset);
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.send_request(0, get_request_head, false, server));
        // RESET_STREAM ends the client's receive half, STOP_SENDING its send half.
        const auto terminate = [&](std::int64_t stream_id) {
            return peer_reset ? peers.client_.shutdown_stream_read(stream_id, ruvia::http3_error_code::request_rejected)
                              : peers.client_.shutdown_stream_write(stream_id, ruvia::http3_error_code::request_rejected);
        };
        ASSERT_EQ(terminate(0), ruvia::http3_transport_status::accepted);
        ruvia::test::http3_event_log client;
        while (auto event = peers.client_.next_event()) {
            client(*event);
        }
        EXPECT_EQ(client.unprocessed_, (std::vector<std::int64_t>{0}));
        EXPECT_EQ(client.sequence_, std::string("UT"));
        // The client cancels the half the server's frame left open.
        EXPECT_TRUE(has_local_cancel(client, 0,
            peer_reset ? ruvia::http3_transport_action::reset_stream : ruvia::http3_transport_action::stop_sending));
        EXPECT_EQ(peers.client_.close_stream(0, ruvia::http3_error_code::request_rejected,
                      ruvia::http3_error_code::request_cancelled),
            ruvia::http3_transport_status::accepted);
        EXPECT_FALSE(peers.client_.next_event().has_value());

        ASSERT_TRUE(peers.send_request(4, get_request_head, true, server));
        ruvia::http_response head;
        ASSERT_TRUE(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
            peers.server_.submit_streaming_response_head(4, std::move(head))));
        client.clear();
        ASSERT_TRUE(peers.to_client(client));
        ASSERT_EQ(client.headers_.size(), std::size_t{1});
        ASSERT_EQ(terminate(4), ruvia::http3_transport_status::accepted);
        client.clear();
        while (auto event = peers.client_.next_event()) {
            client(*event);
        }
        EXPECT_TRUE(client.unprocessed_.empty());
        ASSERT_EQ(client.transport_actions_.size(), std::size_t{1});
        EXPECT_EQ(client.transport_actions_[0].stream_id(), 4);
        EXPECT_EQ(client.transport_actions_[0].origin(), ruvia::http3_transport_action_origin::peer);
        EXPECT_EQ(client.transport_actions_[0].error(), ruvia::http3_error_code::request_rejected);
    }
}

struct http3_observation final {
    static void frame(void* context, const ruvia::http3_frame_metadata& metadata) noexcept {
        static_cast<http3_observation*>(context)->frames_.push_back(metadata);
    }
    static void field(void* context, const ruvia::http3_field_metadata& metadata) noexcept {
        auto& self = *static_cast<http3_observation*>(context);
        self.fields_.push_back({metadata.stream_id_, metadata.section_, metadata.field_index_, metadata.phase_,
            std::string(metadata.name_), std::string(metadata.value_)});
    }

    struct field_record final {
        std::int64_t stream_id_;
        ruvia::http3_field_section_kind section_;
        std::size_t field_index_;
        ruvia::http3_field_observation_phase phase_;
        std::string name_;
        std::string value_;
    };

    std::vector<ruvia::http3_frame_metadata> frames_;
    std::vector<field_record> fields_;
};

// The observers see the received frame headers and QPACK field sections in
// wire order, before HTTP validation.
TEST(Ruvia, http3_frame_and_field_observers_report_received_wire_order) {
    http3_observation observation;
    ruvia::test::http3_peers peers({.frame_observer_ = &http3_observation::frame,
        .field_observer_ = &http3_observation::field,
        .observer_context_ = &observation});
    ASSERT_TRUE(peers.bind());
    ASSERT_EQ(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{.method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .content_ = ruvia::http_request_content_plan::streaming()}),
        ruvia::http3_submit_status::accepted);
    ASSERT_EQ(peers.client_.submit_data(0, ruvia::test::http3_bytes("abc")).status_,
        ruvia::http3_data_submit_status::complete);
    constexpr std::array trailers{ruvia::http_header_view{"x-checksum", "c2f1"}};
    ASSERT_EQ(peers.client_.finish_request(0, trailers), ruvia::http3_finish_status::accepted);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    ASSERT_TRUE(server.ended(0));

    ASSERT_FALSE(observation.frames_.empty());
    for (std::size_t index = 0; index < observation.frames_.size(); ++index) {
        EXPECT_EQ(observation.frames_[index].observation_sequence_, index);
    }
    // The client control stream opens with SETTINGS (RFC 9114 §6.2.1).
    const auto control = std::ranges::find_if(observation.frames_, [](const auto& frame) { return frame.stream_id_ == 2; });
    ASSERT_NE(control, observation.frames_.end());
    EXPECT_EQ(control->type_, ruvia::http3_frame_type::settings);
    std::vector<ruvia::http3_frame_metadata> request;
    std::ranges::copy_if(observation.frames_, std::back_inserter(request), [](const auto& frame) { return frame.stream_id_ == 0; });
    ASSERT_EQ(request.size(), std::size_t{3});
    EXPECT_EQ(request[0].type_, ruvia::http3_frame_type::headers);
    EXPECT_EQ(request[1].type_, ruvia::http3_frame_type::data);
    EXPECT_EQ(request[1].payload_length_, std::uint64_t{3});
    EXPECT_EQ(request[2].type_, ruvia::http3_frame_type::headers);
    EXPECT_LT(request[0].observation_sequence_, request[1].observation_sequence_);
    EXPECT_LT(request[1].observation_sequence_, request[2].observation_sequence_);
    // QPACK encoder and decoder streams carry no frames.
    EXPECT_TRUE(std::ranges::none_of(observation.frames_, [](const auto& frame) { return frame.stream_id_ == 6 || frame.stream_id_ == 10; }));

    using phase = ruvia::http3_field_observation_phase;
    using section = ruvia::http3_field_section_kind;
    ASSERT_FALSE(observation.fields_.empty());
    const auto& begin = observation.fields_.front();
    EXPECT_EQ(begin.stream_id_, 0);
    EXPECT_EQ(begin.section_, section::header);
    EXPECT_EQ(begin.phase_, phase::begin);
    EXPECT_TRUE(begin.name_.empty() && begin.value_.empty());
    const auto header_end = std::ranges::find_if(observation.fields_, [](const auto& record) { return record.phase_ == phase::end; });
    ASSERT_NE(header_end, observation.fields_.end());
    EXPECT_EQ(header_end->section_, section::header);
    std::size_t header_fields = 0;
    bool saw_method = false;
    for (auto record = observation.fields_.begin() + 1; record != header_end; ++record) {
        EXPECT_EQ(record->phase_, phase::field);
        EXPECT_EQ(record->section_, section::header);
        EXPECT_EQ(record->field_index_, header_fields++);
        saw_method = saw_method || (record->name_ == ":method" && record->value_ == "POST");
    }
    EXPECT_TRUE(saw_method);
    EXPECT_EQ(header_end->field_index_, header_fields);
    ASSERT_EQ(observation.fields_.end() - header_end, 4);
    const auto trailer = header_end + 1;
    EXPECT_EQ(trailer[0].section_, section::trailer);
    EXPECT_EQ(trailer[0].phase_, phase::begin);
    EXPECT_EQ(trailer[1].section_, section::trailer);
    EXPECT_EQ(trailer[1].phase_, phase::field);
    EXPECT_EQ(trailer[1].name_, "x-checksum");
    EXPECT_EQ(trailer[1].value_, "c2f1");
    EXPECT_EQ(trailer[2].section_, section::trailer);
    EXPECT_EQ(trailer[2].phase_, phase::end);
    EXPECT_EQ(trailer[2].field_index_, std::size_t{1});
}

TEST(Ruvia, http3_settings_and_datagrams_are_negotiated_and_stream_scoped) {
    ruvia::test::http3_peers peers(
        {.enable_connect_protocol_ = true, .enable_h3_datagram_ = true},
        {.enable_h3_datagram_ = true});
    EXPECT_TRUE(peers.bind());
    EXPECT_TRUE(!peers.client_.peer_settings().has_value());
    ruvia::test::http3_event_log log;
    EXPECT_TRUE(peers.to_client(log));
    EXPECT_TRUE(peers.to_server(log));
    const auto settings = peers.client_.peer_settings();
    EXPECT_TRUE(settings && settings->enable_connect_protocol_ && settings->h3_datagram_);
    EXPECT_TRUE(!peers.client_.datagram_prefix(0));
    EXPECT_TRUE(peers.send_request(0, get_request_head, false, log));
    const auto prefix = peers.client_.datagram_prefix(0);
    EXPECT_TRUE(prefix && prefix->size_ == 1 && prefix->bytes_[0] == std::byte{0});
    const std::string wire("\0\0echo", 6);
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_transport_status::accepted);
    // Public input calls must drain the prior owned event before appending more.
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_transport_status::events_pending);
    auto event = peers.server_.next_event();
    const auto* datagram = event ? event->datagram() : nullptr;
    EXPECT_TRUE(datagram && datagram->stream_id() == 0 && datagram->payload() == std::string_view("\0echo", 5));
    EXPECT_TRUE(!peers.server_.next_event());
    // An unknown request stream is dropped, not retained for future streams.
    const std::string unknown("\1\0echo", 6);
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(unknown)).status_ == ruvia::http3_transport_status::accepted);
    EXPECT_TRUE(!peers.server_.next_event());
    EXPECT_TRUE(peers.server_.close_stream(0, ruvia::http3_error_code::request_cancelled, ruvia::http3_error_code::request_cancelled) == ruvia::http3_transport_status::accepted);
    while (peers.server_.next_event()) {
    }
    EXPECT_TRUE(!peers.server_.datagram_prefix(0));
    EXPECT_TRUE(peers.server_.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_transport_status::accepted);
    EXPECT_TRUE(!peers.server_.next_event());
}

TEST(Ruvia, http3_datagram_truncated_prefix_and_disabled_setting_are_connection_errors) {
    auto enabled = ruvia::http3_connection::server({.enable_h3_datagram_ = true});
    const auto truncated = enabled.read_datagram(ruvia::test::http3_bytes("\x40"));
    EXPECT_TRUE(truncated.status_ == ruvia::http3_transport_status::protocol_error);
    EXPECT_EQ(truncated.error_code_, std::optional(ruvia::http3_error_code::datagram_error));
    auto disabled = ruvia::http3_connection::server();
    const std::string wire("\0hello", 6);
    EXPECT_TRUE(disabled.read_datagram(ruvia::test::http3_bytes(wire)).status_ == ruvia::http3_transport_status::protocol_error);
}

TEST(Ruvia, http3_origin_frame_advertises_initial_and_dynamic_origins) {
    constexpr std::array<std::string_view, 2> initial_origins{
        "https://example.test",
        "https://api.example.test:8443",
    };
    ruvia::test::http3_peers peers({.origins_ = initial_origins});
    EXPECT_TRUE(peers.bind());

    ruvia::test::http3_event_log client_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_EQ(client_log.origins_.size(), std::size_t{1});
    if (client_log.origins_.size() == 1) {
        EXPECT_EQ(client_log.origins_[0].size(), std::size_t{2});
        if (client_log.origins_[0].size() == 2) {
            EXPECT_EQ(client_log.origins_[0][0], std::string("https://example.test"));
            EXPECT_EQ(client_log.origins_[0][1], std::string("https://api.example.test:8443"));
        }
    }

    constexpr std::array<std::string_view, 1> dynamic_origins{"https://cdn.example.test"};
    EXPECT_TRUE(peers.client_.submit_origin_advertisement(dynamic_origins) ==
                ruvia::http3_submit_status::invalid_state);
    EXPECT_TRUE(peers.server_.submit_origin_advertisement(dynamic_origins) ==
                ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.server_.submit_origin_advertisement(std::span<const std::string_view>{}) ==
                ruvia::http3_submit_status::accepted);
    constexpr std::array<std::string_view, 1> invalid_origin{"not-an-origin"};
    EXPECT_TRUE(peers.server_.submit_origin_advertisement(invalid_origin) ==
                ruvia::http3_submit_status::invalid_message);

    client_log.clear();
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_EQ(client_log.origins_.size(), std::size_t{2});
    if (client_log.origins_.size() == 2) {
        EXPECT_EQ(client_log.origins_[0].size(), std::size_t{1});
        if (!client_log.origins_[0].empty()) {
            EXPECT_EQ(client_log.origins_[0][0], std::string("https://cdn.example.test"));
        }
        EXPECT_TRUE(client_log.origins_[1].empty());
    }
}

TEST(Ruvia, http3_priority_headers_and_priority_update_frames_reprioritize_streams) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client_log;
    ruvia::test::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    constexpr std::array priority_fields{ruvia::http_header_view{"priority", "u=1, i"}};
    const ruvia::http3_regular_request_head_view prioritized_request_head{
        .authority_ = "example.test", .target_ = "/resource", .headers_ = priority_fields};
    EXPECT_TRUE(peers.send_request(0, prioritized_request_head, true, server_log));
    auto initial_priority = peers.server_.stream_priority(0);
    EXPECT_TRUE(initial_priority.has_value());
    if (initial_priority) {
        EXPECT_EQ(initial_priority->urgency_.value(), std::uint8_t{1});
        EXPECT_TRUE(initial_priority->incremental_);
    }

    EXPECT_TRUE(peers.client_.submit_priority_update(
                    0, {.urgency_ = ruvia::http_priority_urgency(5), .incremental_ = false}) ==
                ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.to_server(server_log));
    auto updated_priority = peers.server_.stream_priority(0);
    EXPECT_TRUE(updated_priority.has_value());
    if (updated_priority) {
        EXPECT_EQ(updated_priority->urgency_.value(), std::uint8_t{5});
        EXPECT_TRUE(!updated_priority->incremental_);
    }

    ruvia::http_response response;
    response.header("Priority", "u=2, i");
    response.body("prioritized");
    EXPECT_TRUE(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(0, response,
            ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response))));
    auto overridden = peers.server_.stream_priority(0);
    EXPECT_TRUE(overridden.has_value());
    if (overridden) {
        EXPECT_EQ(overridden->urgency_.value(), std::uint8_t{2});
        EXPECT_TRUE(overridden->incremental_);
    }
    EXPECT_EQ(peers.server_.submit_data(0, ruvia::test::http3_bytes(response.body())).consumed_bytes_, response.body().size());
    EXPECT_TRUE(peers.server_.finish_stream(0) == ruvia::http3_finish_status::accepted);
    EXPECT_TRUE(peers.to_client(client_log));
    ASSERT_EQ(client_log.headers_.size(), std::size_t{1});
    EXPECT_TRUE(client_log.headers_[0].field("priority") == "u=2, i");
}

TEST(Ruvia, http3_request_enforces_declared_content_length) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client_log;
    ruvia::test::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    constexpr std::array fields{ruvia::http_header_view{"Content-Type", "application/octet-stream"}};
    EXPECT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{
                                                         .method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .headers_ = fields, .content_ = ruvia::http_request_content_plan::streaming(5)}) == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.client_.submit_data(0, ruvia::test::http3_bytes("abc")).status_ ==
                ruvia::http3_data_submit_status::complete);
    // Content short of or beyond the declared length is a value failure
    // that leaves the stream open.
    EXPECT_EQ(peers.client_.finish_stream(0), ruvia::http3_finish_status::invalid_message);
    const auto overrun = peers.client_.submit_data(0, ruvia::test::http3_bytes("def"));
    EXPECT_EQ(overrun.status_, ruvia::http3_data_submit_status::invalid_message);
    EXPECT_EQ(overrun.consumed_bytes_, std::size_t{0});
    EXPECT_TRUE(peers.client_.submit_data(0, ruvia::test::http3_bytes("de")).status_ ==
                ruvia::http3_data_submit_status::complete);
    EXPECT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_finish_status::accepted);
    EXPECT_TRUE(peers.to_server(server_log));
    EXPECT_EQ(server_log.data(0), "abcde");
    EXPECT_TRUE(server_log.ended(0));
    // Finished content is a caller ordering error, not a value.
    EXPECT_THROW((void)peers.client_.finish_stream(0), std::logic_error);
    EXPECT_THROW((void)peers.client_.submit_data(0, ruvia::test::http3_bytes("f")), std::logic_error);
}

TEST(Ruvia, http3_concurrent_streams_interleave_partial_header_blocks) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    ruvia::test::http3_event_log client_log;
    ruvia::test::http3_event_log server_log;
    EXPECT_TRUE(peers.to_client(client_log));
    EXPECT_TRUE(peers.to_server(server_log));

    EXPECT_TRUE(peers.client_.submit_request_head(0, get_request_head) == ruvia::http3_submit_status::accepted);
    EXPECT_TRUE(peers.client_.submit_request_head(4, get_request_head) == ruvia::http3_submit_status::accepted);

    std::vector<std::byte> stream0;
    std::vector<std::byte> stream4;
    ruvia::http3_stream_output out;
    while (peers.client_.peek_output(64 * 1024, out).status_ == ruvia::http3_transport_status::accepted) {
        const auto size = out.size();
        if (out.stream_id_ == 0 || out.stream_id_ == 4) {
            auto& dst = out.stream_id_ == 0 ? stream0 : stream4;
            for (std::size_t idx = 0; idx < out.buffer_count_; ++idx) {
                dst.insert(dst.end(), out.buffers_[idx].begin(), out.buffers_[idx].end());
            }
        } else {
            for (std::size_t idx = 0; idx < out.buffer_count_; ++idx) {
                EXPECT_TRUE(peers.server_.read_stream(out.stream_id_, out.buffers_[idx], out.fin_, 1).status_ ==
                            ruvia::http3_transport_status::accepted);
            }
        }
        EXPECT_TRUE(peers.client_.commit_output(out.stream_id_, size) == ruvia::http3_transport_status::accepted);
        if (size != 0) {
            EXPECT_TRUE(peers.client_.acknowledge_output(out.stream_id_, size) == ruvia::http3_transport_status::accepted);
        }
    }
    EXPECT_TRUE(stream0.size() > std::size_t{4});
    EXPECT_TRUE(stream4.size() > std::size_t{4});

    // Feed the first half of stream 0's HEADERS frame, then all of stream 4's
    // HEADERS frame, then the rest of stream 0's HEADERS frame.
    const auto split = stream0.size() / 2;
    EXPECT_TRUE(peers.server_.read_stream(0, std::span(stream0.data(), split), false, 1).status_ ==
                ruvia::http3_transport_status::accepted);
    EXPECT_TRUE(peers.server_.header_block_in_progress());
    EXPECT_TRUE(peers.server_.read_stream(4, stream4, true, 2).status_ == ruvia::http3_transport_status::accepted);
    while (auto ev = peers.server_.next_event()) {
        server_log(*ev);
    }
    EXPECT_TRUE(peers.server_.header_block_in_progress());
    EXPECT_TRUE(peers.server_.read_stream(
                                 0, std::span(stream0.data() + split, stream0.size() - split), true, 3)
                    .status_ == ruvia::http3_transport_status::accepted);
    while (auto ev = peers.server_.next_event()) {
        server_log(*ev);
    }
    EXPECT_TRUE(!peers.server_.header_block_in_progress());
    EXPECT_EQ(server_log.headers_.size(), std::size_t{2});
}
