#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"

#include "http3_relay.h"
namespace {

static_assert(std::is_nothrow_move_constructible_v<ruvia::http3_request_head>);
static_assert(!std::is_move_assignable_v<ruvia::http3_request_head>);
// Only a server connection creates validated heads.
static_assert(!std::is_constructible_v<ruvia::http3_request_head, std::int64_t,
    std::vector<ruvia::http_header>, bool, bool>);

using field_list = std::initializer_list<ruvia::test::http3_wire_field>;

struct received_request final {
    ruvia::http3_transport_result result_;
    ruvia::test::http3_event_log log_;
    std::optional<ruvia::http3_request_head> head_;
};

// Sends `fields` verbatim as the HEADERS frame of request stream
// `stream_id`, which ends with it when `stream_ended`, and keeps what the
// server published. The wire reaches field sections a client never sends.
[[nodiscard]] received_request receive_request(ruvia::test::http3_peers& peers,
    std::span<const ruvia::test::http3_wire_field> fields, bool stream_ended, std::int64_t stream_id = 0) {
    std::string wire;
    ruvia::test::append_http3_wire_headers(wire, fields);
    received_request received;
    received.result_ = peers.server_.read_stream(stream_id, ruvia::test::http3_bytes(wire), stream_ended, peers.timestamp_++);
    while (auto event = peers.server_.next_event()) {
        received.log_(*event);
        if (auto* head = event->request_head()) {
            received.head_.emplace(std::move(*head).take_head());
        }
    }
    return received;
}

// A malformed request is a stream error of type H3_MESSAGE_ERROR (RFC 9114
// §4.1.2) that no message event reveals.
[[nodiscard]] bool rejected(const received_request& received, const ruvia::http3_connection& server,
    std::int64_t stream_id = 0) {
    return !received.head_.has_value() && !received.log_.names_message(stream_id) &&
           ruvia::test::rejected_http3_message(received.result_, server, received.log_, stream_id);
}

[[nodiscard]] bool parses(field_list fields, bool stream_ended = true) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    const auto received = receive_request(peers, std::span(fields.begin(), fields.size()), stream_ended);
    if (received.head_.has_value()) {
        EXPECT_TRUE(received.log_.transport_actions_.empty());
        return true;
    }
    EXPECT_TRUE(rejected(received, peers.server_));
    return false;
}

[[nodiscard]] std::optional<ruvia::http_server_expectation_plan> expectation_plan(
    field_list fields, bool stream_ended, ruvia::http_unsupported_expectation_policy policy) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    const auto received = receive_request(peers, std::span(fields.begin(), fields.size()), stream_ended);
    if (!received.head_) {
        return std::nullopt;
    }
    return received.head_->expectation_plan(policy);
}

}  // namespace

// A GET decoded by nghttp3 maps to an http_request: the query is split from
// :path, Host comes from :authority, and split Cookie crumbs are rejoined
// with "; " (RFC 9114 §4.2.1).
TEST(Ruvia, http3_request_head_parses_get_received_over_nghttp3) {
    ruvia::test::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    constexpr std::array request_fields{
        ruvia::http_header_view{"cookie", "session=abc"},
        ruvia::http_header_view{"accept", "text/html"},
        ruvia::http_header_view{"cookie", "theme=dark"},
    };
    EXPECT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{
                                                         .authority_ = "example.test", .target_ = "/search?q=h3&page=2", .headers_ = request_fields}) == ruvia::http3_submit_status::accepted);
    std::optional<ruvia::http3_request_head> head;
    EXPECT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        if (auto* received = event.request_head()) {
            EXPECT_EQ(received->stream_id(), std::int64_t{0});
            head.emplace(std::move(*received).take_head());
        }
    }));
    ASSERT_TRUE(head.has_value());
    EXPECT_EQ(head->stream_id(), std::int64_t{0});
    EXPECT_TRUE(head->method() == "GET");
    EXPECT_TRUE(head->scheme() == "https");
    EXPECT_TRUE(head->authority() == "example.test");
    EXPECT_TRUE(head->path() == "/search?q=h3&page=2");
    EXPECT_TRUE(!head->connect());
    EXPECT_TRUE(head->stream_ended());
    EXPECT_FALSE(head->early_data());
    EXPECT_TRUE(head->content() == ruvia::http_request_content_indication::no_content);

    const auto request = head->build_request();
    EXPECT_TRUE(request.method() == "GET");
    EXPECT_TRUE(request.known_method() == ruvia::http_known_method::get);
    EXPECT_TRUE(request.protocol_version() == ruvia::http_protocol_version::http3);
    EXPECT_TRUE(request.target_form() == ruvia::http_request_target_form::http3);
    EXPECT_TRUE(request.target() == "/search?q=h3&page=2");
    EXPECT_TRUE(request.path() == "/search");
    EXPECT_TRUE(request.query_string() == "q=h3&page=2");
    EXPECT_TRUE(request.last_raw_query_value("page") == std::string_view("2"));
    EXPECT_TRUE(request.scheme() == "https");
    EXPECT_TRUE(request.authority() == "example.test");
    EXPECT_TRUE(request.header("host") == std::string_view("example.test"));
    EXPECT_TRUE(request.header("accept") == std::string_view("text/html"));
    EXPECT_TRUE(request.header("cookie") == std::string_view("session=abc; theme=dark"));
    EXPECT_TRUE(request.cookie("session") == std::string_view("abc"));
    EXPECT_TRUE(request.cookie("theme") == std::string_view("dark"));
    EXPECT_TRUE(request.body().empty());
}

TEST(Ruvia, http3_request_head_keeps_cookie_crumbs_after_moving) {
    const std::string long_cookie = "session=" + std::string(128, 'x');
    const std::string long_joined = long_cookie + "; b=2";
    struct cookie_case final {
        std::array<std::string_view, 3> crumbs_;
        std::string_view expected_;
    };
    const std::array cases{
        cookie_case{{"", "", ""}, ""},
        cookie_case{{"", long_cookie, ""}, long_cookie},
        cookie_case{{long_cookie, "", "b=2"}, long_joined},
        cookie_case{{"a=1", "", ""}, "a=1"},
        cookie_case{{"", "", "a=1"}, "a=1"},
        cookie_case{{"", "a=1", "b=2"}, "a=1; b=2"},
        cookie_case{{"a=1", "", "a=2"}, "a=1; a=2"},
        cookie_case{{"a=1", "b=2", ""}, "a=1; b=2"},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.expected_);
        {
            std::optional<ruvia::http3_request_head> head;
            {
                ruvia::test::http3_peers peers;
                ASSERT_TRUE(peers.bind());
                const std::array fields{
                    ruvia::test::http3_wire_field{":method", "GET"},
                    ruvia::test::http3_wire_field{":scheme", "https"},
                    ruvia::test::http3_wire_field{":authority", "example.test"},
                    ruvia::test::http3_wire_field{":path", "/"},
                    ruvia::test::http3_wire_field{"cookie", test_case.crumbs_[0]},
                    ruvia::test::http3_wire_field{"cookie", test_case.crumbs_[1]},
                    ruvia::test::http3_wire_field{"cookie", test_case.crumbs_[2]},
                };
                auto received = receive_request(peers, fields, true);
                ASSERT_TRUE(received.head_.has_value());
                head.emplace(std::move(*received.head_));
            }
            // The head outlives the connection that produced it.
            const auto request = head->build_request();
            if (test_case.expected_.empty()) {
                EXPECT_TRUE(!request.header("cookie").has_value());
            } else {
                EXPECT_TRUE(request.header("cookie") == test_case.expected_);
            }
        }
    }
}

TEST(Ruvia, http3_request_head_bounds_single_and_joined_cookie_values) {
    for (const bool split : {false, true}) {
        for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
            SCOPED_TRACE(split);
            SCOPED_TRACE(excess);
            // The field section limit stays above the per-field bound.
            ruvia::test::http3_peers peers({.max_field_section_size_ = 4 * ruvia::max_http_header_bytes});
            ASSERT_TRUE(peers.bind());
            const std::string first = "a=" + std::string(ruvia::max_http_header_bytes - 2 - (split ? 5 : 0) + excess, 'x');
            std::vector<ruvia::test::http3_wire_field> fields{
                {":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
                {"cookie", first}};
            if (split) {
                fields.push_back({"cookie", "b=2"});
            }
            const auto received = receive_request(peers, fields, true);
            if (excess != 0 && !split) {
                // nghttp3's QPACK decoder refuses a single value above 64 KiB
                // as a connection error before the codec sees the field.
                EXPECT_EQ(received.result_.status_, ruvia::http3_transport_status::protocol_error);
                EXPECT_FALSE(received.log_.names_message(0));
            } else if (excess != 0) {
                EXPECT_TRUE(rejected(received, peers.server_));
                EXPECT_TRUE(ruvia::test::has_local_message_error(received.log_, 0));
            } else {
                ASSERT_TRUE(received.head_.has_value());
                const auto request = received.head_->build_request();
                EXPECT_TRUE(request.header("cookie") == first + (split ? "; b=2" : ""));
            }
        }
    }
}

// RFC 9114 §4.4: CONNECT carries only :method and :authority, and its target
// is the authority form.
TEST(Ruvia, http3_request_head_parses_connect_authority_target) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    ASSERT_EQ(peers.client_.submit_request_head(0, ruvia::http3_connect_request_head_view{.authority_ = "proxy.test:443"}),
        ruvia::http3_submit_status::accepted);
    std::optional<ruvia::http3_request_head> head;
    ASSERT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        if (auto* received = event.request_head()) {
            head.emplace(std::move(*received).take_head());
        }
    }));
    ASSERT_TRUE(head.has_value());
    EXPECT_TRUE(head->connect());
    EXPECT_FALSE(head->stream_ended());
    EXPECT_TRUE(head->authority() == "proxy.test:443");
    EXPECT_TRUE(head->scheme().empty());
    EXPECT_TRUE(head->path().empty());

    const auto request = head->build_request("tunnel");
    EXPECT_TRUE(request.known_method() == ruvia::http_known_method::connect);
    EXPECT_TRUE(request.protocol_version() == ruvia::http_protocol_version::http3);
    EXPECT_TRUE(request.target() == "proxy.test:443");
    EXPECT_TRUE(request.authority() == "proxy.test:443");
    EXPECT_TRUE(request.query_string().empty());
    EXPECT_TRUE(request.body() == "tunnel");

    // A CONNECT that names a scheme or path, or lacks its authority, is malformed.
    EXPECT_TRUE(!parses({{":method", "CONNECT"}, {":scheme", "https"}, {":authority", "proxy.test:443"}}, false));
    EXPECT_TRUE(!parses({{":method", "CONNECT"}, {":authority", "proxy.test:443"}, {":path", "/"}}, false));
    EXPECT_TRUE(!parses({{":method", "CONNECT"}}, false));
    EXPECT_TRUE(!parses({{":method", "CONNECT"}, {":authority", "proxy.test"}}, false));
    EXPECT_TRUE(!parses({{":method", "CONNECT"}, {":authority", ":443"}}, false));
    EXPECT_TRUE(!parses({{":method", "CONNECT"}, {":authority", "user@proxy.test:443"}}, false));
}

TEST(Ruvia, http3_request_head_rejects_authority_and_host_disagreement) {
    EXPECT_TRUE(parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
        {"host", "example.test"}}));
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
        {"host", "other.test"}}));
}

// The asterisk form names the server itself and exists only for OPTIONS.
TEST(Ruvia, http3_request_head_rejects_asterisk_path_outside_options) {
    EXPECT_TRUE(parses({{":method", "OPTIONS"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "*"}}));
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "*"}}));
}

// The connection answers a request its own validation rejects (nghttp3
// accepts both heads below) with H3_MESSAGE_ERROR on that stream alone (RFC 9114 §4.1.2): local
// STOP_SENDING and RESET_STREAM, no message event, and the connection keeps
// serving other streams.
TEST(Ruvia, http3_server_resets_only_the_stream_of_a_semantically_malformed_request) {
    const std::array<std::vector<ruvia::test::http3_wire_field>, 2> malformed{{
        {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
            {"host", "other.test"}},
        {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/%zz"}},
    }};
    for (const auto& fields : malformed) {
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        const auto received = receive_request(peers, fields, false);
        EXPECT_EQ(received.result_.status_, ruvia::http3_transport_status::accepted);
        EXPECT_TRUE(ruvia::test::has_local_message_error(received.log_, 0));
        EXPECT_FALSE(received.head_.has_value());
        EXPECT_EQ(received.log_.sequence_, std::string("TT"));
        EXPECT_FALSE(peers.server_.connection_error().has_value());
        // Content and the end of the rejected stream stay unpublished.
        std::string tail;
        ruvia::test::append_http3_wire_frame(tail, 0, "late");
        ruvia::test::http3_event_log late;
        EXPECT_EQ(ruvia::test::receive_http3_wire(peers.server_, 0, tail, true, peers.timestamp_, late).status_,
            ruvia::http3_transport_status::accepted);
        EXPECT_FALSE(late.names_message(0));
        const std::array valid{
            ruvia::test::http3_wire_field{":method", "GET"},
            ruvia::test::http3_wire_field{":scheme", "https"},
            ruvia::test::http3_wire_field{":authority", "example.test"},
            ruvia::test::http3_wire_field{":path", "/"},
        };
        const auto next = receive_request(peers, valid, true, 4);
        ASSERT_TRUE(next.head_.has_value());
        EXPECT_EQ(next.head_->stream_id(), 4);
        EXPECT_TRUE(next.log_.ended(4));
    }
}

// RFC 9114 §4.3.1: an https request needs :authority or Host.
TEST(Ruvia, http3_request_head_requires_authority_or_host_for_https) {
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {":path", "/"}}));

    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    const std::array fields{
        ruvia::test::http3_wire_field{":method", "GET"},
        ruvia::test::http3_wire_field{":scheme", "https"},
        ruvia::test::http3_wire_field{":path", "/"},
        ruvia::test::http3_wire_field{"host", "example.test"},
    };
    const auto received = receive_request(peers, fields, true);
    ASSERT_TRUE(received.head_.has_value());
    const auto request = received.head_->build_request();
    EXPECT_TRUE(request.authority() == "example.test");
    EXPECT_TRUE(request.header("host") == std::string_view("example.test"));
}

// A HEADERS frame that ended the stream carries no content, so a nonzero
// Content-Length contradicts the framing.
TEST(Ruvia, http3_request_head_rejects_content_length_on_ended_stream) {
    const field_list fields{{":method", "POST"}, {":scheme", "https"}, {":authority", "example.test"},
        {":path", "/upload"}, {"content-length", "5"}};
    EXPECT_TRUE(!parses(fields, true));
    EXPECT_TRUE(parses(fields, false));
    EXPECT_TRUE(parses({{":method", "POST"}, {":scheme", "https"}, {":authority", "example.test"},
                           {":path", "/upload"}, {"content-length", "0"}},
        true));

    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    const auto received = receive_request(peers, std::span(fields.begin(), fields.size()), false);
    ASSERT_TRUE(received.head_.has_value());
    EXPECT_TRUE(received.head_->declared_content_length() == std::optional<std::uint64_t>(5));
    EXPECT_TRUE(received.head_->content() == ruvia::http_request_content_indication::will_follow);
}

TEST(Ruvia, http3_request_head_rejects_invalid_fields) {
    const auto with_field = [](std::string_view name, std::string_view value) {
        return parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
            {name, value}});
    };
    EXPECT_TRUE(with_field("x-trace", "valid value"));
    // nghttp3 removes a regular field whose value is invalid before the codec
    // sees it, so the request arrives without it.
    for (const std::string_view invalid : {std::string_view("bad\x01value"), std::string_view("split\r\nvalue"),
             std::string_view("nul\0byte", 8)}) {
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        const std::array fields{
            ruvia::test::http3_wire_field{":method", "GET"},
            ruvia::test::http3_wire_field{":scheme", "https"},
            ruvia::test::http3_wire_field{":authority", "example.test"},
            ruvia::test::http3_wire_field{":path", "/"},
            ruvia::test::http3_wire_field{"x-trace", invalid},
        };
        const auto received = receive_request(peers, fields, true);
        ASSERT_TRUE(received.head_.has_value());
        EXPECT_FALSE(received.log_.headers_[0].field("x-trace").has_value());
    }
    // RFC 9114 §4.2: uppercase names and connection-specific fields are malformed.
    EXPECT_TRUE(!with_field("X-Trace", "value"));
    EXPECT_TRUE(!with_field("connection", "keep-alive"));
    EXPECT_TRUE(!with_field("transfer-encoding", "chunked"));
    // Pseudo-header fields after a regular field, or unknown ones, are malformed.
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {"accept", "*/*"}, {":authority", "example.test"},
        {":path", "/"}}));
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
        {":status", "200"}}));
}

// RFC 9110 §10.1.1: 100-continue is answered only when content follows; an
// unknown expectation is refused under the reject policy.
TEST(Ruvia, http3_request_head_plans_expectations) {
    const field_list continue_with_content{{":method", "PUT"}, {":scheme", "https"}, {":authority", "example.test"},
        {":path", "/object"}, {"content-length", "10"}, {"expect", "100-continue"}};
    EXPECT_TRUE(expectation_plan(continue_with_content, false, ruvia::http_unsupported_expectation_policy::reject) ==
                ruvia::http_server_expectation_plan::send_continue);

    const field_list continue_without_content{{":method", "PUT"}, {":scheme", "https"},
        {":authority", "example.test"}, {":path", "/object"}, {"expect", "100-continue"}};
    EXPECT_TRUE(expectation_plan(continue_without_content, true, ruvia::http_unsupported_expectation_policy::reject) ==
                ruvia::http_server_expectation_plan::no_action);

    const field_list unknown{{":method", "PUT"}, {":scheme", "https"}, {":authority", "example.test"},
        {":path", "/object"}, {"content-length", "10"}, {"expect", "x-unknown-expectation"}};
    EXPECT_TRUE(expectation_plan(unknown, false, ruvia::http_unsupported_expectation_policy::reject) ==
                ruvia::http_server_expectation_plan::reject_unsupported);
    EXPECT_TRUE(expectation_plan(unknown, false, ruvia::http_unsupported_expectation_policy::ignore) ==
                ruvia::http_server_expectation_plan::no_action);
}

// RFC 9110 §6.5.1: framing and routing fields are discarded from a trailer
// section; RFC 9114 §4.3: a pseudo-header field there is malformed.
TEST(Ruvia, http3_request_trailers_drop_forbidden_names_and_reject_pseudo_fields) {
    std::vector<ruvia::http_header> fields;
    fields.emplace_back("content-length", "5");
    fields.emplace_back("x-checksum", "sha-256=abc");
    fields.emplace_back("host", "example.test");
    fields.emplace_back("server-timing", "db;dur=5");

    ruvia::http_request_trailers trailers;
    EXPECT_TRUE(!trailers.append_received(fields).has_value());
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
    EXPECT_TRUE(trailers.field("x-checksum") == std::string_view("sha-256=abc"));
    EXPECT_TRUE(trailers.field("server-timing") == std::string_view("db;dur=5"));
    EXPECT_TRUE(!trailers.field("content-length").has_value());
    EXPECT_TRUE(!trailers.field("host").has_value());

    std::vector<ruvia::http_header> pseudo;
    pseudo.emplace_back("x-extra", "1");
    pseudo.emplace_back(":path", "/");
    EXPECT_TRUE(trailers.append_received(pseudo) ==
                std::optional<ruvia::http_request_trailer_error>(ruvia::http_request_trailer_error::invalid_field));
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
    EXPECT_TRUE(!trailers.field("x-extra").has_value());

    std::vector<ruvia::http_header> invalid_value;
    invalid_value.emplace_back("x-extra", "bad\r\nvalue");
    EXPECT_TRUE(trailers.append_received(invalid_value) ==
                std::optional<ruvia::http_request_trailer_error>(ruvia::http_request_trailer_error::invalid_field));
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
}

TEST(Ruvia, http3_request_head_enforces_materialized_field_count_limit) {
    // Host synthesis and a nonempty Cookie each consume one materialized field.
    for (const bool with_cookie : {false, true}) {
        const auto regular_limit = ruvia::max_http_header_fields - 1 - static_cast<std::size_t>(with_cookie);
        for (const std::size_t regular_count : {regular_limit, regular_limit + 1}) {
            SCOPED_TRACE(regular_count);
            ruvia::test::http3_peers peers;
            ASSERT_TRUE(peers.bind());
            std::vector<ruvia::test::http3_wire_field> fields{
                {":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}};
            fields.resize(fields.size() + regular_count, {"x-field", "v"});
            if (with_cookie) {
                fields.push_back({"cookie", "a=1"});
            }
            const auto received = receive_request(peers, fields, true);
            if (regular_count == regular_limit) {
                ASSERT_TRUE(received.head_.has_value());
                const auto request = received.head_->build_request();
                EXPECT_EQ(request.headers().size(), ruvia::max_http_header_fields);
            } else {
                EXPECT_TRUE(rejected(received, peers.server_));
            }
        }
    }
}

TEST(Ruvia, http3_regular_request_submission_rejects_invalid_heads_without_consuming_stream_ids) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    const ruvia::http3_regular_request_head_view valid{
        .authority_ = "example.test", .target_ = "/search?q=h3"};
    auto invalid = valid;
    invalid.method_ = "GET\r\n";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = valid;
    invalid.method_ = "CONNECT";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = valid;
    invalid.scheme_ = "1https";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = valid;
    invalid.authority_ = "user@example.test";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = valid;
    invalid.target_ = "https://other.test/resource";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = valid;
    invalid.method_ = "TRACE";
    invalid.content_ = ruvia::http_request_content_plan::known_length(1);
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    constexpr std::array forbidden_fields{ruvia::http_header_view{"Connection", "close"}};
    invalid = valid;
    invalid.headers_ = forbidden_fields;
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    constexpr std::array pseudo_fields{ruvia::http_header_view{":path", "/injected"}};
    invalid.headers_ = pseudo_fields;
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    EXPECT_TRUE(server.headers_.empty());
    ASSERT_TRUE(peers.client_.submit_request_head(0, valid) == ruvia::http3_submit_status::accepted);
    ASSERT_TRUE(peers.to_server(server));
    ASSERT_EQ(server.headers_.size(), std::size_t{1});
    EXPECT_TRUE(server.headers_[0].field(":method") == "GET");
    EXPECT_TRUE(server.headers_[0].field(":authority") == "example.test");
    EXPECT_TRUE(server.headers_[0].field(":path") == "/search?q=h3");
    EXPECT_TRUE(server.ended(0));
}

TEST(Ruvia, http3_all_request_head_forms_obey_local_and_negotiated_section_budgets) {
    for (const bool local_limit : {false, true}) {
        SCOPED_TRACE(local_limit);
        ruvia::http3_connection_options server_options;
        server_options.enable_connect_protocol_ = true;
        ruvia::http3_connection_options client_options;
        if (local_limit) {
            client_options.max_field_section_size_ = 512;
        } else {
            server_options.max_field_section_size_ = 512;
        }
        ruvia::test::http3_peers peers(server_options, client_options);
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(peers.to_client(client));
        const std::string large_value(1024, 'x');
        const std::array oversized{ruvia::http_header_view{"x-large", large_value}};
        ruvia::http3_regular_request_head_view ordinary{
            .authority_ = "example.test", .headers_ = oversized};
        ruvia::http3_connect_request_head_view connect{
            .authority_ = "example.test:443", .headers_ = oversized};
        ruvia::http3_extended_connect_request_head_view extended{
            .protocol_ = "connect-udp", .authority_ = "example.test", .target_ = "/ws", .headers_ = oversized};
        EXPECT_TRUE(peers.client_.submit_request_head(0, ordinary) == ruvia::http3_submit_status::invalid_message);
        EXPECT_TRUE(peers.client_.submit_request_head(4, connect) == ruvia::http3_submit_status::invalid_message);
        EXPECT_TRUE(peers.client_.submit_request_head(8, extended) == ruvia::http3_submit_status::invalid_message);
        ruvia::test::http3_event_log server;
        ASSERT_TRUE(peers.to_server(server));
        EXPECT_TRUE(server.headers_.empty());
        ordinary.headers_ = std::span<const ruvia::http_header_view>{};
        connect.headers_ = std::span<const ruvia::http_header_view>{};
        extended.headers_ = std::span<const ruvia::http_header_view>{};
        ASSERT_TRUE(peers.client_.submit_request_head(0, ordinary) == ruvia::http3_submit_status::accepted);
        ASSERT_TRUE(peers.client_.submit_request_head(4, connect) == ruvia::http3_submit_status::accepted);
        ASSERT_TRUE(peers.client_.submit_request_head(8, extended) == ruvia::http3_submit_status::accepted);
        ASSERT_TRUE(peers.to_server(server));
        ASSERT_EQ(server.headers_.size(), std::size_t{3});
        EXPECT_TRUE(server.ended(0));
        EXPECT_FALSE(server.ended(4));
        EXPECT_FALSE(server.ended(8));
        EXPECT_TRUE(server.headers_[1].field(":method") == "CONNECT");
        EXPECT_TRUE(server.headers_[1].field(":authority") == "example.test:443");
        EXPECT_FALSE(server.headers_[1].field(":scheme").has_value());
        EXPECT_FALSE(server.headers_[1].field(":path").has_value());
        EXPECT_TRUE(server.headers_[2].field(":protocol") == "connect-udp");
        EXPECT_TRUE(server.headers_[2].field(":path") == "/ws");
    }
}

TEST(Ruvia, http3_connect_request_submission_validates_authority_and_preserves_tunnel_bytes) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    for (const std::string_view authority : {"example.test", ":443", "user@example.test:443", "example.test:99999"}) {
        SCOPED_TRACE(authority);
        EXPECT_TRUE(peers.client_.submit_request_head(0,
                        ruvia::http3_connect_request_head_view{.authority_ = authority}) == ruvia::http3_submit_status::invalid_message);
    }
    constexpr std::array framing{ruvia::http_header_view{"Content-Length", "3"}};
    EXPECT_TRUE(peers.client_.submit_request_head(0,
                    ruvia::http3_connect_request_head_view{.authority_ = "example.test:443", .headers_ = framing}) ==
                ruvia::http3_submit_status::invalid_message);
    ASSERT_TRUE(peers.client_.submit_request_head(0,
                    ruvia::http3_connect_request_head_view{.authority_ = "example.test:443"}) == ruvia::http3_submit_status::accepted);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    ruvia::http_response accepted;
    ASSERT_EQ(peers.server_.submit_connect_response_head(0, accepted), ruvia::http3_submit_status::accepted);
    EXPECT_EQ(peers.server_.submit_connect_response_head(0, accepted), ruvia::http3_submit_status::invalid_state);
    ruvia::test::http3_event_log client;
    ASSERT_TRUE(peers.to_client(client));
    EXPECT_EQ(peers.client_.submit_data(0, ruvia::test::http3_bytes("tunnel")).consumed_bytes_, std::size_t{6});
    ASSERT_TRUE(peers.client_.finish_stream(0) == ruvia::http3_finish_status::accepted);
    ASSERT_TRUE(peers.to_server(server));
    ASSERT_EQ(server.headers_.size(), std::size_t{1});
    EXPECT_TRUE(server.headers_[0].field(":authority") == "example.test:443");
    EXPECT_FALSE(server.headers_[0].field("content-length").has_value());
    EXPECT_EQ(server.data(0), "tunnel");
    EXPECT_TRUE(server.ended(0));
}

TEST(Ruvia, http3_extended_connect_request_submission_requires_negotiation_and_valid_fields) {
    ruvia::http3_extended_connect_request_head_view request{
        .protocol_ = "websocket", .authority_ = "example.test", .target_ = "/ws"};
    {
        ruvia::test::http3_peers peers;
        ASSERT_TRUE(peers.bind());
        ruvia::test::http3_event_log client;
        ASSERT_TRUE(peers.to_client(client));
        EXPECT_TRUE(peers.client_.submit_request_head(0, request) == ruvia::http3_submit_status::peer_capability_unavailable);
    }
    ruvia::test::http3_peers peers({.enable_connect_protocol_ = true});
    ASSERT_TRUE(peers.bind());
    EXPECT_TRUE(peers.client_.submit_request_head(0, request) == ruvia::http3_submit_status::peer_settings_pending);
    ruvia::test::http3_event_log client;
    ASSERT_TRUE(peers.to_client(client));
    auto invalid = request;
    invalid.protocol_ = "bad protocol";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = request;
    invalid.target_ = "https://other.test/ws";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    invalid = request;
    invalid.scheme_ = "1https";
    EXPECT_TRUE(peers.client_.submit_request_head(0, invalid) == ruvia::http3_submit_status::invalid_message);
    constexpr std::array fields{ruvia::http_header_view{"Sec-WebSocket-Version", "13"}};
    request.headers_ = fields;
    ASSERT_TRUE(peers.client_.submit_request_head(0, request) == ruvia::http3_submit_status::accepted);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    ASSERT_EQ(server.headers_.size(), std::size_t{1});
    EXPECT_TRUE(server.headers_[0].field(":method") == "CONNECT");
    EXPECT_TRUE(server.headers_[0].field(":protocol") == "websocket");
    EXPECT_TRUE(server.headers_[0].field("sec-websocket-version") == "13");
    EXPECT_FALSE(server.ended(0));
}

TEST(Ruvia, http3_typed_streaming_request_sends_validated_trailers_after_exact_content_length) {
    ruvia::test::http3_peers peers;
    ASSERT_TRUE(peers.bind());
    constexpr std::array headers{ruvia::http_header_view{"Content-Type", "application/octet-stream"}};
    ASSERT_TRUE(peers.client_.submit_request_head(0, ruvia::http3_regular_request_head_view{
                                                         .method_ = "POST", .authority_ = "example.test", .target_ = "/upload", .headers_ = headers, .content_ = ruvia::http_request_content_plan::streaming(3)}) == ruvia::http3_submit_status::accepted);
    constexpr std::array trailers{
        ruvia::http_header_view{"X-Checksum", "sha-256=abc"},
        ruvia::http_header_view{"Server-Timing", "db;dur=5"},
    };
    // Content short of the declared length rejects the trailers as a value.
    EXPECT_TRUE(peers.client_.finish_request(0, trailers) == ruvia::http3_finish_status::invalid_message);
    EXPECT_EQ(peers.client_.submit_data(0, ruvia::test::http3_bytes("abc")).consumed_bytes_, std::size_t{3});
    for (const std::string_view name : {":path", "Content-Length", "Connection", "Host"}) {
        SCOPED_TRACE(name);
        const std::array invalid{ruvia::http_header_view{name, "invalid"}};
        EXPECT_TRUE(peers.client_.finish_request(0, invalid) == ruvia::http3_finish_status::invalid_message);
    }
    ASSERT_TRUE(peers.client_.finish_request(0, trailers) == ruvia::http3_finish_status::accepted);
    ruvia::test::http3_event_log server;
    ASSERT_TRUE(peers.to_server(server));
    ASSERT_EQ(server.headers_.size(), std::size_t{1});
    EXPECT_TRUE(server.headers_[0].field("content-length") == "3");
    EXPECT_EQ(server.data(0), "abc");
    // The server receives the decoded trailer section with the message end.
    const auto* end = server.end(0);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(end->trailers_.size(), std::size_t{2});
    EXPECT_TRUE(end->field("x-checksum") == "sha-256=abc");
    EXPECT_TRUE(end->field("server-timing") == "db;dur=5");
    EXPECT_EQ(server.sequence_, "HDE");
}

TEST(Ruvia, http3_connect_rejects_nonzero_content_length_with_open_stream) {
    EXPECT_TRUE(parses({
                           {":method", "CONNECT"},
                           {":authority", "example.test:443"},
                           {"content-length", "0"},
                       },
        false));
    EXPECT_FALSE(parses({
                            {":method", "CONNECT"},
                            {":authority", "example.test:443"},
                            {"content-length", "5"},
                        },
        false));

    ruvia::test::http3_peers peers({.enable_connect_protocol_ = true});
    ASSERT_TRUE(peers.bind());
    const std::array extended_nonzero{
        ruvia::test::http3_wire_field{":method", "CONNECT"},
        ruvia::test::http3_wire_field{":protocol", "connect-udp"},
        ruvia::test::http3_wire_field{":scheme", "https"},
        ruvia::test::http3_wire_field{":authority", "example.test"},
        ruvia::test::http3_wire_field{":path", "/.well-known/masque/udp/192.0.2.1/443/"},
        ruvia::test::http3_wire_field{"capsule-protocol", "?1"},
        ruvia::test::http3_wire_field{"content-length", "5"},
    };
    const auto received = receive_request(peers, extended_nonzero, false);
    EXPECT_TRUE(rejected(received, peers.server_));
}
