#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http3_connection.h"

#include "http3_relay.h"
#include "test_harness.h"

namespace {

constexpr std::array get_request_fields{
    ruvia::http3_field_view{":method", "GET"},
    ruvia::http3_field_view{":scheme", "https"},
    ruvia::http3_field_view{":authority", "example.test"},
    ruvia::http3_field_view{":path", "/resource"},
};

constexpr std::array no_content_response_fields{
    ruvia::http3_field_view{":status", "204"},
    ruvia::http3_field_view{"server", "ruvia"},
};

// RFC 9114 §5.2: a shutdown-notice GOAWAY from a server names the largest
// client-initiated bidirectional stream identifier, 2^62 - 4.
constexpr std::int64_t shutdown_notice_stream_id = (std::int64_t{1} << 62) - 4;

// H3_REQUEST_CANCELLED, RFC 9114 §8.1.
constexpr std::uint64_t request_cancelled = 0x10c;

}  // namespace

RUVIA_TEST(http3_nghttp3_client_server_exchange_headers_and_stream_end) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());

    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.send_request(0, get_request_fields, true, request));
    RUVIA_CHECK_EQ(request.headers_.size(), std::size_t{1});
    if (request.headers_.size() != 1) {
        return;
    }
    RUVIA_CHECK_EQ(request.headers_[0].stream_id_, std::int64_t{0});
    RUVIA_CHECK_EQ(request.headers_[0].fields_.size(), get_request_fields.size());
    RUVIA_CHECK(request.headers_[0].stream_ended_);
    RUVIA_CHECK(request.headers_[0].field(":path") == std::string_view("/resource"));
    RUVIA_CHECK(request.ended(0));

    RUVIA_CHECK(peers.server_.submit_response(0, no_content_response_fields, true) ==
                ruvia::http3_status::accepted);
    ruvia::testing::http3_event_log response;
    RUVIA_CHECK(peers.to_client(response));
    RUVIA_CHECK_EQ(response.headers_.size(), std::size_t{1});
    if (response.headers_.size() != 1) {
        return;
    }
    RUVIA_CHECK_EQ(response.headers_[0].stream_id_, std::int64_t{0});
    RUVIA_CHECK_EQ(response.headers_[0].fields_.size(), no_content_response_fields.size());
    RUVIA_CHECK(response.headers_[0].stream_ended_);
    RUVIA_CHECK(response.headers_[0].field(":status") == std::string_view("204"));
    RUVIA_CHECK(response.ended(0));
    RUVIA_CHECK_EQ(response.sequence_, std::string("HE"));
}

RUVIA_TEST(http3_nghttp3_streaming_body_is_owned_until_acknowledged) {
    ruvia::testing::http3_peers peers({.own_data_events_ = true});
    RUVIA_CHECK(peers.bind());

    constexpr std::array request_fields{
        ruvia::http3_field_view{":method", "POST"},
        ruvia::http3_field_view{":scheme", "https"},
        ruvia::http3_field_view{":authority", "example.test"},
        ruvia::http3_field_view{":path", "/upload"},
        ruvia::http3_field_view{"content-length", "3"},
    };
    RUVIA_CHECK(peers.client_.submit_request(0, request_fields, false) == ruvia::http3_status::accepted);
    const auto submitted = peers.client_.submit_data(0, ruvia::testing::http3_bytes("abc"));
    RUVIA_CHECK(submitted.status_ == ruvia::http3_status::accepted);
    RUVIA_CHECK_EQ(submitted.accepted_bytes_, std::size_t{3});
    RUVIA_CHECK(peers.client_.finish_stream(0) == ruvia::http3_status::accepted);

    // The relay scrubs every delivered input buffer, so owned DATA is the
    // only way the payload can still read "abc" after delivery.
    std::optional<ruvia::http3_data_event> retained;
    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.to_server([&](ruvia::http3_event& event) {
        request(event);
        if (auto* data = std::get_if<ruvia::http3_data_event>(&event)) {
            retained.emplace(std::move(*data));
        }
    }));

    RUVIA_CHECK_EQ(request.headers_.size(), std::size_t{1});
    RUVIA_CHECK(retained.has_value() && retained->stream_id_ == 0 && retained->bytes_ == "abc");
    RUVIA_CHECK_EQ(request.data(0), std::string("abc"));
    RUVIA_CHECK(request.ended(0));
    RUVIA_CHECK_EQ(request.sequence_, std::string("HDE"));
}

// A request stream that QUIC reports closed is published once, with the
// application error code only when the closure carried one.
RUVIA_TEST(http3_close_stream_publishes_stream_close_event) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());

    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.send_request(0, get_request_fields, true, request));
    RUVIA_CHECK(peers.send_request(4, get_request_fields, true, request));
    RUVIA_CHECK(request.ended(0) && request.ended(4));
    RUVIA_CHECK(peers.server_.submit_response(0, no_content_response_fields, true) ==
                ruvia::http3_status::accepted);
    ruvia::testing::http3_event_log response;
    RUVIA_CHECK(peers.to_client(response));
    RUVIA_CHECK(response.ended(0));

    RUVIA_CHECK(peers.server_.close_stream(0, std::nullopt, std::nullopt) == ruvia::http3_status::accepted);
    ruvia::testing::http3_event_log closed;
    while (auto event = peers.server_.next_event()) {
        closed(*event);
    }
    RUVIA_CHECK_EQ(closed.closes_.size(), std::size_t{1});
    if (closed.closes_.size() == 1) {
        RUVIA_CHECK_EQ(closed.closes_[0].stream_id_, std::int64_t{0});
        RUVIA_CHECK(!closed.closes_[0].application_error_code_.has_value());
    }

    RUVIA_CHECK(peers.server_.close_stream(4, request_cancelled, std::nullopt) == ruvia::http3_status::accepted);
    closed.clear();
    while (auto event = peers.server_.next_event()) {
        closed(*event);
    }
    RUVIA_CHECK_EQ(closed.closes_.size(), std::size_t{1});
    if (closed.closes_.size() == 1) {
        RUVIA_CHECK_EQ(closed.closes_[0].stream_id_, std::int64_t{4});
        RUVIA_CHECK(closed.closes_[0].application_error_code_ == std::optional<std::uint64_t>(request_cancelled));
    }
    RUVIA_CHECK_EQ(closed.sequence_, std::string("C"));

    // A closed stream accepts no further response.
    RUVIA_CHECK(peers.server_.submit_response(0, no_content_response_fields, true) !=
                ruvia::http3_status::accepted);
}

// RFC 9114 §5.2 graceful shutdown: the notice still admits in-flight
// requests, the final GOAWAY names the first unprocessed request stream, and
// the server is drained only once every admitted request stream has closed.
RUVIA_TEST(http3_server_drain_sends_notice_then_final_goaway) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());

    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.send_request(0, get_request_fields, true, request));
    RUVIA_CHECK(request.ended(0));

    RUVIA_CHECK(peers.server_.begin_drain() == ruvia::http3_status::accepted);
    RUVIA_CHECK(!peers.server_.drain_final());
    RUVIA_CHECK(!peers.server_.drained());
    ruvia::testing::http3_event_log client;
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK_EQ(client.goaways_.size(), std::size_t{1});
    RUVIA_CHECK(!client.goaways_.empty() && client.goaways_[0] == shutdown_notice_stream_id);

    RUVIA_CHECK(peers.server_.finish_drain() == ruvia::http3_status::accepted);
    RUVIA_CHECK(peers.server_.drain_final());
    RUVIA_CHECK(!peers.server_.drained());
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK_EQ(client.goaways_.size(), std::size_t{2});
    RUVIA_CHECK(client.goaways_.size() == 2 && client.goaways_[1] == 4);
    // Stream 0 was admitted before the final GOAWAY and remains open.
    RUVIA_CHECK(!peers.server_.drained());

    RUVIA_CHECK(peers.server_.submit_response(0, no_content_response_fields, true) ==
                ruvia::http3_status::accepted);
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK(client.ended(0));
    RUVIA_CHECK(!peers.server_.drained());

    RUVIA_CHECK(peers.server_.close_stream(0, std::nullopt, std::nullopt) == ruvia::http3_status::accepted);
    while (auto event = peers.server_.next_event()) {
        (void)event;
    }
    RUVIA_CHECK(peers.server_.drained());
    // A repeated final drain is idempotent.
    RUVIA_CHECK(peers.server_.finish_drain() == ruvia::http3_status::accepted);
    RUVIA_CHECK(peers.server_.drain_final());
}
