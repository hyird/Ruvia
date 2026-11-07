#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_request_head.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/http_status.h"

#include "http3_relay.h"
#include "test_harness.h"

namespace {

constexpr std::array get_request_fields{
    ruvia::http3_field_view{":method", "GET"},
    ruvia::http3_field_view{":scheme", "https"},
    ruvia::http3_field_view{":authority", "example.test"},
    ruvia::http3_field_view{":path", "/resource"},
};

constexpr std::array head_request_fields{
    ruvia::http3_field_view{":method", "HEAD"},
    ruvia::http3_field_view{":scheme", "https"},
    ruvia::http3_field_view{":authority", "example.test"},
    ruvia::http3_field_view{":path", "/resource"},
};

template <typename error_type>
[[nodiscard]] bool throws_as(auto&& operation) {
    try {
        operation();
    } catch (const error_type&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

[[nodiscard]] bool rejected_with(const auto& result, ruvia::http3_response_head_error error) {
    const auto* actual = std::get_if<ruvia::http3_response_head_error>(&result);
    return actual != nullptr && *actual == error;
}

}  // namespace

// RFC 9114 §4.1-§4.2: a buffered response becomes :status, lowercase regular
// fields, a generated Date and the planned Content-Length, followed by DATA.
RUVIA_TEST(http3_buffered_response_head_maps_status_fields_and_content_length) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());
    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.send_request(0, get_request_fields, true, request));
    RUVIA_CHECK(request.ended(0));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.status(ruvia::http_status::ok);
    response.header("X-Trace-Id", "trace-7");
    response.body("hello world");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    RUVIA_CHECK(plan.send_body());
    RUVIA_CHECK_EQ(plan.content_length(), std::uint64_t{11});

    const auto submitted = peers.server_.submit_response_head(0, response, plan);
    const auto* accepted = std::get_if<ruvia::http_buffered_response_write_plan>(&submitted);
    RUVIA_CHECK(accepted != nullptr && accepted->content_length() == 11);
    // A second final head on the same stream is refused.
    RUVIA_CHECK(rejected_with(peers.server_.submit_response_head(0, response, plan),
        ruvia::http3_response_head_error::invalid_state));

    const auto data = peers.server_.submit_data(0, ruvia::testing::http3_bytes(response.body()));
    RUVIA_CHECK(data.status_ == ruvia::http3_status::accepted);
    RUVIA_CHECK_EQ(data.accepted_bytes_, std::size_t{11});
    RUVIA_CHECK(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);

    ruvia::testing::http3_event_log client;
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK_EQ(client.sequence_, std::string("HDE"));
    RUVIA_CHECK_EQ(client.headers_.size(), std::size_t{1});
    if (client.headers_.size() != 1) {
        return;
    }
    const auto& head = client.headers_[0];
    RUVIA_CHECK(!head.stream_ended_);
    RUVIA_CHECK(!head.fields_.empty() && head.fields_[0].name_ == ":status" && head.fields_[0].value_ == "200");
    RUVIA_CHECK(head.field("x-trace-id") == std::string_view("trace-7"));
    RUVIA_CHECK(!head.field("X-Trace-Id").has_value());
    RUVIA_CHECK(head.field("content-length") == std::string_view("11"));
    const auto date = head.field("date");
    RUVIA_CHECK(date.has_value() && date->ends_with(" GMT"));
    for (const auto& field : head.fields_) {
        for (const char byte : field.name_) {
            RUVIA_CHECK(byte < 'A' || byte > 'Z');
        }
    }
    RUVIA_CHECK_EQ(client.data(0), std::string("hello world"));
    RUVIA_CHECK(client.ended(0));
}

// RFC 9110 §9.3.2: a HEAD response is the head alone, ending the stream.
RUVIA_TEST(http3_buffered_response_head_to_head_request_ends_with_head) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());
    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.send_request(0, head_request_fields, true, request));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.body("hello world");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::head, response);
    RUVIA_CHECK(!plan.send_body());
    RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(0, response, plan)));
    // The head ended the stream: no content can follow.
    RUVIA_CHECK(peers.server_.submit_data(0, ruvia::testing::http3_bytes("x")).status_ ==
                ruvia::http3_status::invalid_state);
    RUVIA_CHECK(peers.server_.finish_stream(0) == ruvia::http3_status::invalid_state);

    ruvia::testing::http3_event_log client;
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK_EQ(client.sequence_, std::string("HE"));
    RUVIA_CHECK(client.headers_.size() == 1 && client.headers_[0].stream_ended_ &&
                client.headers_[0].field(":status") == std::string_view("200"));
    RUVIA_CHECK(client.data(0).empty());
    RUVIA_CHECK(client.ended(0));
}

// RFC 9114 §4.2: connection-specific fields have no HTTP/3 meaning.
RUVIA_TEST(http3_response_head_rejects_connection_specific_fields) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());
    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.send_request(0, get_request_fields, true, request));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.header("Connection", "close");
    response.body("bye");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    RUVIA_CHECK(rejected_with(peers.server_.submit_response_head(0, response, plan),
        ruvia::http3_response_head_error::invalid_message));

    ruvia::http_response streamed(&resource);
    streamed.header("Keep-Alive", "timeout=5");
    RUVIA_CHECK(rejected_with(peers.server_.submit_streaming_response_head(0, ruvia::http_known_method::get,
                                  std::move(streamed), ruvia::http_response_trailer_intent::none),
        ruvia::http3_response_head_error::invalid_message));

    // A rejected head sends nothing and leaves the stream able to answer.
    ruvia::testing::http3_event_log client;
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK(client.headers_.empty());
    response.remove_header("Connection");
    const auto valid_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(0, response, valid_plan)));
    RUVIA_CHECK(peers.server_.submit_data(0, ruvia::testing::http3_bytes("bye")).accepted_bytes_ == 3);
    RUVIA_CHECK(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK(client.headers_.size() == 1 && !client.headers_[0].field("connection").has_value());
    RUVIA_CHECK_EQ(client.data(0), std::string("bye"));
    RUVIA_CHECK(client.ended(0));
}

// The planned Content-Length binds the DATA total exactly: excess is refused
// before anything is copied and an early end leaves the stream open.
RUVIA_TEST(http3_response_content_length_mismatch_throws_length_error) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());
    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.send_request(0, get_request_fields, true, request));
    RUVIA_CHECK(peers.send_request(4, get_request_fields, true, request));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.body("hello");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(0, response, plan)));
    RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(4, response, plan)));

    RUVIA_CHECK(throws_as<std::length_error>(
        [&] { (void)peers.server_.submit_data(0, ruvia::testing::http3_bytes("hello!")); }));
    RUVIA_CHECK_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
    RUVIA_CHECK(peers.server_.submit_data(0, ruvia::testing::http3_bytes("hello")).accepted_bytes_ == 5);
    RUVIA_CHECK(throws_as<std::length_error>(
        [&] { (void)peers.server_.submit_data(0, ruvia::testing::http3_bytes("!")); }));
    RUVIA_CHECK(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);

    RUVIA_CHECK(peers.server_.submit_data(4, ruvia::testing::http3_bytes("hel")).accepted_bytes_ == 3);
    RUVIA_CHECK(throws_as<std::length_error>([&] { (void)peers.server_.finish_stream(4); }));
    RUVIA_CHECK(peers.server_.submit_data(4, ruvia::testing::http3_bytes("lo")).accepted_bytes_ == 2);
    RUVIA_CHECK(peers.server_.finish_stream(4) == ruvia::http3_status::accepted);

    ruvia::testing::http3_event_log client;
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK_EQ(client.data(0), std::string("hello"));
    RUVIA_CHECK_EQ(client.data(4), std::string("hello"));
    RUVIA_CHECK(client.ended(0) && client.ended(4));
    RUVIA_CHECK(!peers.client_.connection_error().has_value());
}

// RFC 9114 §4.1: a streaming response carries its trailer section in a
// HEADERS frame after the DATA frames.
RUVIA_TEST(http3_streaming_response_head_sends_trailers_after_data) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());
    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.send_request(0, get_request_fields, true, request));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response head(&resource);
    head.header("Content-Type", "text/plain");
    const auto submitted = peers.server_.submit_streaming_response_head(
        0, ruvia::http_known_method::get, std::move(head), ruvia::http_response_trailer_intent::present);
    const auto* commit = std::get_if<ruvia::http_response_stream_commit_plan>(&submitted);
    RUVIA_CHECK(commit != nullptr);
    if (commit == nullptr) {
        return;
    }
    RUVIA_CHECK(commit->framing() == ruvia::http_response_stream_framing::http3_frames);
    RUVIA_CHECK(commit->trailers_available());
    RUVIA_CHECK(commit->head_disposition() == ruvia::http_response_stream_head_disposition::body_open);

    RUVIA_CHECK(peers.server_.submit_data(0, ruvia::testing::http3_bytes("chunk-1;")).accepted_bytes_ == 8);
    RUVIA_CHECK(peers.server_.submit_data(0, ruvia::testing::http3_bytes("chunk-2")).accepted_bytes_ == 7);
    const std::array trailer_fields{ruvia::http_header_view{"X-Checksum", "c2f1"}};
    RUVIA_CHECK(peers.server_.finish_response(0, ruvia::validate_http_response_trailers(
                                                     std::span<const ruvia::http_header_view>(trailer_fields))) ==
                ruvia::http3_status::accepted);
    // The trailer section was the last thing the stream may carry.
    RUVIA_CHECK(peers.server_.submit_data(0, ruvia::testing::http3_bytes("late")).status_ ==
                ruvia::http3_status::invalid_state);

    ruvia::testing::http3_event_log client;
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK_EQ(client.sequence_, std::string("HDHE"));
    RUVIA_CHECK_EQ(client.headers_.size(), std::size_t{2});
    if (client.headers_.size() != 2) {
        return;
    }
    RUVIA_CHECK(client.headers_[0].field(":status") == std::string_view("200"));
    RUVIA_CHECK(client.headers_[0].field("content-type") == std::string_view("text/plain"));
    RUVIA_CHECK(!client.headers_[0].field("content-length").has_value());
    RUVIA_CHECK(!client.headers_[0].stream_ended_);
    RUVIA_CHECK_EQ(client.data(0), std::string("chunk-1;chunk-2"));
    RUVIA_CHECK_EQ(client.headers_[1].fields_.size(), std::size_t{1});
    RUVIA_CHECK(client.headers_[1].field("x-checksum") == std::string_view("c2f1"));
    RUVIA_CHECK(client.headers_[1].stream_ended_);
    RUVIA_CHECK(client.ended(0));
}

// RFC 9114 §4.1: interim responses precede the final head on the same stream.
RUVIA_TEST(http3_interim_response_precedes_final_head) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());
    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.send_request(0, get_request_fields, true, request));

    const std::array links{ruvia::http_header_view{"Link", "</style.css>; rel=preload; as=style"}};
    const ruvia::http_interim_response_head early_hints(
        ruvia::http_status::early_hints, std::span<const ruvia::http_header_view>(links));
    RUVIA_CHECK(peers.server_.submit_interim_response_head(0, early_hints) == ruvia::http3_status::accepted);

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response response(&resource);
    response.body("page");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    RUVIA_CHECK(std::holds_alternative<ruvia::http_buffered_response_write_plan>(
        peers.server_.submit_response_head(0, response, plan)));
    // Once the final head is committed no interim response may follow.
    RUVIA_CHECK(peers.server_.submit_interim_response_head(0, early_hints) == ruvia::http3_status::invalid_state);
    RUVIA_CHECK(peers.server_.submit_data(0, ruvia::testing::http3_bytes("page")).accepted_bytes_ == 4);
    RUVIA_CHECK(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);

    ruvia::testing::http3_event_log client;
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK_EQ(client.sequence_, std::string("HHDE"));
    RUVIA_CHECK_EQ(client.headers_.size(), std::size_t{2});
    if (client.headers_.size() != 2) {
        return;
    }
    RUVIA_CHECK(client.headers_[0].field(":status") == std::string_view("103"));
    RUVIA_CHECK(client.headers_[0].field("link") == std::string_view("</style.css>; rel=preload; as=style"));
    RUVIA_CHECK(!client.headers_[0].stream_ended_);
    RUVIA_CHECK(client.headers_[1].field(":status") == std::string_view("200"));
    RUVIA_CHECK(client.headers_[1].field("content-length") == std::string_view("4"));
    RUVIA_CHECK_EQ(client.data(0), std::string("page"));
}

// submit_data() holds at most max_stream_output_bytes_ unacknowledged DATA per
// stream; acknowledgements free the budget and delivery stays in order.
RUVIA_TEST(http3_submit_data_is_bounded_by_unacknowledged_output) {
    ruvia::testing::http3_peers peers({.max_stream_output_bytes_ = 8});
    RUVIA_CHECK(peers.bind());
    ruvia::testing::http3_event_log request;
    RUVIA_CHECK(peers.send_request(0, get_request_fields, true, request));

    std::pmr::unsynchronized_pool_resource resource;
    ruvia::http_response head(&resource);
    RUVIA_CHECK(std::holds_alternative<ruvia::http_response_stream_commit_plan>(
        peers.server_.submit_streaming_response_head(
            0, ruvia::http_known_method::get, std::move(head), ruvia::http_response_trailer_intent::none)));

    constexpr std::string_view payload = "abcdefghijklmnopqrst";
    const auto first = peers.server_.submit_data(0, ruvia::testing::http3_bytes(payload));
    RUVIA_CHECK(first.status_ == ruvia::http3_status::accepted);
    RUVIA_CHECK_EQ(first.accepted_bytes_, std::size_t{8});
    const auto blocked = peers.server_.submit_data(0, ruvia::testing::http3_bytes(payload.substr(8)));
    RUVIA_CHECK(blocked.status_ == ruvia::http3_status::would_block);
    RUVIA_CHECK_EQ(blocked.accepted_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(peers.server_.pending_output_bytes(0), std::size_t{8});

    // Delivery alone frees nothing: the budget waits for the acknowledgement.
    ruvia::testing::http3_event_log client;
    std::vector<ruvia::testing::http3_withheld_acknowledgement> withheld;
    RUVIA_CHECK(peers.to_client(client, &withheld));
    RUVIA_CHECK_EQ(client.data(0), std::string(payload.substr(0, 8)));
    RUVIA_CHECK_EQ(peers.server_.pending_output_bytes(0), std::size_t{8});
    RUVIA_CHECK(peers.server_.submit_data(0, ruvia::testing::http3_bytes(payload.substr(8))).status_ ==
                ruvia::http3_status::would_block);

    RUVIA_CHECK(ruvia::testing::acknowledge_http3(peers.server_, withheld));
    RUVIA_CHECK_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
    const auto second = peers.server_.submit_data(0, ruvia::testing::http3_bytes(payload.substr(8)));
    RUVIA_CHECK(second.status_ == ruvia::http3_status::accepted);
    RUVIA_CHECK_EQ(second.accepted_bytes_, std::size_t{8});
    RUVIA_CHECK_EQ(peers.server_.pending_output_bytes(0), std::size_t{8});

    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
    const auto third = peers.server_.submit_data(0, ruvia::testing::http3_bytes(payload.substr(16)));
    RUVIA_CHECK(third.status_ == ruvia::http3_status::accepted);
    RUVIA_CHECK_EQ(third.accepted_bytes_, std::size_t{4});
    RUVIA_CHECK(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);
    RUVIA_CHECK(peers.to_client(client));

    RUVIA_CHECK_EQ(client.data(0), std::string(payload));
    RUVIA_CHECK(client.ended(0));
    RUVIA_CHECK_EQ(peers.server_.pending_output_bytes(0), std::size_t{0});
}

RUVIA_TEST(http3_connect_response_has_no_content_framing_and_preserves_half_close) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());
    constexpr std::array fields{
        ruvia::http3_field_view{":method", "CONNECT"},
        ruvia::http3_field_view{":authority", "echo.test:443"},
    };
    ruvia::testing::http3_event_log server;
    RUVIA_CHECK(peers.send_request(0, fields, false, server));
    ruvia::http_response response;
    response.status(ruvia::http_status::ok);
    response.header("content-length", "0");
    RUVIA_CHECK(peers.server_.submit_connect_response_head(0, response) ==
                ruvia::http3_response_head_error::invalid_message);
    ruvia::http_response accepted;
    accepted.status(ruvia::http_status::ok);
    RUVIA_CHECK(!peers.server_.submit_connect_response_head(0, accepted));
    ruvia::testing::http3_event_log client;
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK(client.headers_.size() == 1 && !client.headers_[0].stream_ended_ &&
                !client.headers_[0].field("content-length") && !client.headers_[0].field("transfer-encoding"));
    RUVIA_CHECK(peers.client_.submit_data(0, ruvia::testing::http3_bytes("request")).accepted_bytes_ == 7);
    RUVIA_CHECK(peers.client_.finish_stream(0) == ruvia::http3_status::accepted);
    RUVIA_CHECK(peers.to_server(server));
    RUVIA_CHECK(server.data(0) == "request" && server.ended(0));
    RUVIA_CHECK(peers.server_.submit_data(0, ruvia::testing::http3_bytes("reply")).accepted_bytes_ == 5);
    RUVIA_CHECK(peers.server_.finish_stream(0) == ruvia::http3_status::accepted);
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK(client.data(0) == "reply" && client.ended(0));
}

RUVIA_TEST(http3_websocket_extended_connect_commits_negotiated_200) {
    ruvia::testing::http3_peers peers({.enable_connect_protocol_ = true});
    RUVIA_CHECK(peers.bind());
    ruvia::testing::http3_event_log client;
    RUVIA_CHECK(peers.to_client(client));
    constexpr std::array fields{
        ruvia::http3_field_view{":method", "CONNECT"},
        ruvia::http3_field_view{":scheme", "https"},
        ruvia::http3_field_view{":authority", "example.test"},
        ruvia::http3_field_view{":path", "/ws"},
        ruvia::http3_field_view{":protocol", "websocket"},
        ruvia::http3_field_view{"sec-websocket-version", "13"},
    };
    RUVIA_CHECK(peers.client_.submit_request(0, fields, false) == ruvia::http3_status::accepted);
    std::optional<ruvia::http3_request_head> head;
    RUVIA_CHECK(peers.to_server([&](ruvia::http3_event& event) {
        if (auto* headers = std::get_if<ruvia::http3_headers_event>(&event)) {
            head = ruvia::http3_request_head::parse(std::move(*headers), std::pmr::get_default_resource());
        }
    }));
    RUVIA_CHECK(head.has_value());
    if (!head) {
        return;
    }
    const auto request = head->build_request(std::pmr::get_default_resource());
    const auto validation = ruvia::validate_http3_websocket_handshake(*head, request);
    RUVIA_CHECK(validation.accepted() != nullptr);
    RUVIA_CHECK(std::holds_alternative<ruvia::websocket_extended_connect_negotiation>(
        peers.server_.submit_websocket_handshake(0, request, validation)));
    RUVIA_CHECK(peers.to_client(client));
    RUVIA_CHECK(client.headers_.size() == 1 && client.headers_[0].field(":status") == "200" &&
                !client.headers_[0].field("content-length") && !client.headers_[0].field("sec-websocket-accept"));
}
