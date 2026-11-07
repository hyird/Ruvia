#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_request_head.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_trailers.h"

#include "http3_relay.h"
#include "test_harness.h"

namespace {

using field_list = std::initializer_list<std::pair<std::string_view, std::string_view>>;

// A decoded field section as nghttp3 would publish it. Building it directly
// reaches the request semantics nghttp3 does not itself enforce.
[[nodiscard]] ruvia::http3_headers_event make_headers_event(
    std::pmr::memory_resource* resource, field_list fields, bool stream_ended) {
    ruvia::http3_headers_event event{0, std::pmr::vector<ruvia::http3_field>(resource), stream_ended};
    for (const auto& [name, value] : fields) {
        event.fields_.emplace_back(name, value, resource);
    }
    return event;
}

[[nodiscard]] bool parses(field_list fields, bool stream_ended = true) {
    std::pmr::unsynchronized_pool_resource resource;
    return ruvia::http3_request_head::parse(make_headers_event(&resource, fields, stream_ended), &resource)
        .has_value();
}

[[nodiscard]] std::optional<ruvia::http_server_expectation_plan> expectation_plan(
    field_list fields, bool stream_ended, ruvia::http_unsupported_expectation_policy policy) {
    std::pmr::unsynchronized_pool_resource resource;
    const auto head =
        ruvia::http3_request_head::parse(make_headers_event(&resource, fields, stream_ended), &resource);
    if (!head) {
        return std::nullopt;
    }
    return head->expectation_plan(policy);
}

}  // namespace

// A GET decoded by nghttp3 maps to an http_request: the query is split from
// :path, Host comes from :authority, and split Cookie crumbs are rejoined
// with "; " (RFC 9114 §4.2.1).
RUVIA_TEST(http3_request_head_parses_get_received_over_nghttp3) {
    ruvia::testing::http3_peers peers;
    RUVIA_CHECK(peers.bind());
    constexpr std::array request_fields{
        ruvia::http3_field_view{":method", "GET"},
        ruvia::http3_field_view{":scheme", "https"},
        ruvia::http3_field_view{":authority", "example.test"},
        ruvia::http3_field_view{":path", "/search?q=h3&page=2"},
        ruvia::http3_field_view{"cookie", "session=abc"},
        ruvia::http3_field_view{"accept", "text/html"},
        ruvia::http3_field_view{"cookie", "theme=dark"},
    };
    RUVIA_CHECK(peers.client_.submit_request(0, request_fields, true) == ruvia::http3_status::accepted);

    std::pmr::unsynchronized_pool_resource resource;
    std::optional<ruvia::http3_headers_event> received;
    RUVIA_CHECK(peers.to_server([&](ruvia::http3_event& event) {
        if (auto* headers = std::get_if<ruvia::http3_headers_event>(&event)) {
            received.emplace(std::move(*headers));
        }
    }));
    RUVIA_CHECK(received.has_value());
    if (!received) {
        return;
    }

    const auto head = ruvia::http3_request_head::parse(std::move(*received), &resource);
    RUVIA_CHECK(head.has_value());
    if (!head) {
        return;
    }
    RUVIA_CHECK_EQ(head->stream_id(), std::int64_t{0});
    RUVIA_CHECK(head->method() == "GET");
    RUVIA_CHECK(head->scheme() == "https");
    RUVIA_CHECK(head->authority() == "example.test");
    RUVIA_CHECK(head->path() == "/search?q=h3&page=2");
    RUVIA_CHECK(!head->connect());
    RUVIA_CHECK(head->stream_ended());
    RUVIA_CHECK(head->content() == ruvia::http_request_content_indication::no_content);

    const auto request = head->build_request(&resource);
    RUVIA_CHECK(request.method() == "GET");
    RUVIA_CHECK(request.known_method() == ruvia::http_known_method::get);
    RUVIA_CHECK(request.protocol_version() == ruvia::http_protocol_version::http3);
    RUVIA_CHECK(request.target_form() == ruvia::http_request_target_form::http3);
    RUVIA_CHECK(request.target() == "/search?q=h3&page=2");
    RUVIA_CHECK(request.path() == "/search");
    RUVIA_CHECK(request.query_string() == "q=h3&page=2");
    RUVIA_CHECK(request.last_raw_query_value("page") == std::string_view("2"));
    RUVIA_CHECK(request.scheme() == "https");
    RUVIA_CHECK(request.authority() == "example.test");
    RUVIA_CHECK(request.header("host") == std::string_view("example.test"));
    RUVIA_CHECK(request.header("accept") == std::string_view("text/html"));
    RUVIA_CHECK(request.header("cookie") == std::string_view("session=abc; theme=dark"));
    RUVIA_CHECK(request.cookie("session") == std::string_view("abc"));
    RUVIA_CHECK(request.cookie("theme") == std::string_view("dark"));
    RUVIA_CHECK(request.body().empty());
}

// RFC 9114 §4.4: CONNECT carries only :method and :authority, and its target
// is the authority form.
RUVIA_TEST(http3_request_head_parses_connect_authority_target) {
    std::pmr::unsynchronized_pool_resource resource;
    const auto head = ruvia::http3_request_head::parse(
        make_headers_event(&resource, {{":method", "CONNECT"}, {":authority", "proxy.test:443"}}, false),
        &resource);
    RUVIA_CHECK(head.has_value());
    if (!head) {
        return;
    }
    RUVIA_CHECK(head->connect());
    RUVIA_CHECK(head->authority() == "proxy.test:443");
    RUVIA_CHECK(head->scheme().empty());
    RUVIA_CHECK(head->path().empty());

    const auto request = head->build_request(&resource, "tunnel");
    RUVIA_CHECK(request.known_method() == ruvia::http_known_method::connect);
    RUVIA_CHECK(request.protocol_version() == ruvia::http_protocol_version::http3);
    RUVIA_CHECK(request.target() == "proxy.test:443");
    RUVIA_CHECK(request.authority() == "proxy.test:443");
    RUVIA_CHECK(request.query_string().empty());
    RUVIA_CHECK(request.body() == "tunnel");

    // A CONNECT that names a scheme or path, or lacks its authority, is malformed.
    RUVIA_CHECK(!parses({{":method", "CONNECT"}, {":scheme", "https"}, {":authority", "proxy.test:443"}}, false));
    RUVIA_CHECK(!parses({{":method", "CONNECT"}, {":authority", "proxy.test:443"}, {":path", "/"}}, false));
    RUVIA_CHECK(!parses({{":method", "CONNECT"}}, false));
    RUVIA_CHECK(!parses({{":method", "CONNECT"}, {":authority", "proxy.test"}}, false));
    RUVIA_CHECK(!parses({{":method", "CONNECT"}, {":authority", ":443"}}, false));
    RUVIA_CHECK(!parses({{":method", "CONNECT"}, {":authority", "user@proxy.test:443"}}, false));
}

RUVIA_TEST(http3_request_head_rejects_authority_and_host_disagreement) {
    RUVIA_CHECK(parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
        {"host", "example.test"}}));
    RUVIA_CHECK(!parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
        {"host", "other.test"}}));
}

// The asterisk form names the server itself and exists only for OPTIONS.
RUVIA_TEST(http3_request_head_rejects_asterisk_path_outside_options) {
    RUVIA_CHECK(parses({{":method", "OPTIONS"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "*"}}));
    RUVIA_CHECK(!parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "*"}}));
}

// RFC 9114 §4.3.1: an https request needs :authority or Host.
RUVIA_TEST(http3_request_head_requires_authority_or_host_for_https) {
    RUVIA_CHECK(!parses({{":method", "GET"}, {":scheme", "https"}, {":path", "/"}}));

    std::pmr::unsynchronized_pool_resource resource;
    const auto head = ruvia::http3_request_head::parse(
        make_headers_event(
            &resource, {{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"host", "example.test"}}, true),
        &resource);
    RUVIA_CHECK(head.has_value());
    if (head) {
        const auto request = head->build_request(&resource);
        RUVIA_CHECK(request.authority() == "example.test");
        RUVIA_CHECK(request.header("host") == std::string_view("example.test"));
    }
}

// A HEADERS frame that ended the stream carries no content, so a nonzero
// Content-Length contradicts the framing.
RUVIA_TEST(http3_request_head_rejects_content_length_on_ended_stream) {
    const field_list fields{{":method", "POST"}, {":scheme", "https"}, {":authority", "example.test"},
        {":path", "/upload"}, {"content-length", "5"}};
    RUVIA_CHECK(!parses(fields, true));
    RUVIA_CHECK(parses(fields, false));
    RUVIA_CHECK(parses({{":method", "POST"}, {":scheme", "https"}, {":authority", "example.test"},
                           {":path", "/upload"}, {"content-length", "0"}},
        true));

    std::pmr::unsynchronized_pool_resource resource;
    const auto head = ruvia::http3_request_head::parse(make_headers_event(&resource, fields, false), &resource);
    RUVIA_CHECK(head.has_value());
    if (head) {
        RUVIA_CHECK(head->declared_content_length() == std::optional<std::uint64_t>(5));
        RUVIA_CHECK(head->content() == ruvia::http_request_content_indication::will_follow);
    }
}

RUVIA_TEST(http3_request_head_rejects_invalid_fields) {
    const auto with_field = [](std::string_view name, std::string_view value) {
        return parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
            {name, value}});
    };
    RUVIA_CHECK(with_field("x-trace", "valid value"));
    RUVIA_CHECK(!with_field("x-trace", std::string_view("bad\x01value")));
    RUVIA_CHECK(!with_field("x-trace", "split\r\nvalue"));
    RUVIA_CHECK(!with_field("x-trace", std::string_view("nul\0byte", 8)));
    // RFC 9114 §4.2: uppercase names and connection-specific fields are malformed.
    RUVIA_CHECK(!with_field("X-Trace", "value"));
    RUVIA_CHECK(!with_field("connection", "keep-alive"));
    RUVIA_CHECK(!with_field("transfer-encoding", "chunked"));
    // Pseudo-header fields after a regular field, or unknown ones, are malformed.
    RUVIA_CHECK(!parses({{":method", "GET"}, {":scheme", "https"}, {"accept", "*/*"}, {":authority", "example.test"},
        {":path", "/"}}));
    RUVIA_CHECK(!parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
        {":status", "200"}}));
}

// RFC 9110 §10.1.1: 100-continue is answered only when content follows; an
// unknown expectation is refused under the reject policy.
RUVIA_TEST(http3_request_head_plans_expectations) {
    const field_list continue_with_content{{":method", "PUT"}, {":scheme", "https"}, {":authority", "example.test"},
        {":path", "/object"}, {"content-length", "10"}, {"expect", "100-continue"}};
    RUVIA_CHECK(expectation_plan(continue_with_content, false, ruvia::http_unsupported_expectation_policy::reject) ==
                ruvia::http_server_expectation_plan::send_continue);

    const field_list continue_without_content{{":method", "PUT"}, {":scheme", "https"},
        {":authority", "example.test"}, {":path", "/object"}, {"expect", "100-continue"}};
    RUVIA_CHECK(expectation_plan(continue_without_content, true, ruvia::http_unsupported_expectation_policy::reject) ==
                ruvia::http_server_expectation_plan::no_action);

    const field_list unknown{{":method", "PUT"}, {":scheme", "https"}, {":authority", "example.test"},
        {":path", "/object"}, {"content-length", "10"}, {"expect", "x-unknown-expectation"}};
    RUVIA_CHECK(expectation_plan(unknown, false, ruvia::http_unsupported_expectation_policy::reject) ==
                ruvia::http_server_expectation_plan::reject_unsupported);
    RUVIA_CHECK(expectation_plan(unknown, false, ruvia::http_unsupported_expectation_policy::ignore) ==
                ruvia::http_server_expectation_plan::no_action);
}

// RFC 9110 §6.5.1: framing and routing fields are discarded from a trailer
// section; RFC 9114 §4.3: a pseudo-header field there is malformed.
RUVIA_TEST(http3_request_trailers_drop_forbidden_names_and_reject_pseudo_fields) {
    std::pmr::unsynchronized_pool_resource resource;
    std::pmr::vector<ruvia::http3_field> fields(&resource);
    fields.emplace_back("content-length", "5", &resource);
    fields.emplace_back("x-checksum", "sha-256=abc", &resource);
    fields.emplace_back("host", "example.test", &resource);
    fields.emplace_back("server-timing", "db;dur=5", &resource);

    ruvia::http_request_trailers trailers(&resource);
    RUVIA_CHECK(!ruvia::append_http3_request_trailers(trailers, fields).has_value());
    RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{2});
    RUVIA_CHECK(trailers.field("x-checksum") == std::string_view("sha-256=abc"));
    RUVIA_CHECK(trailers.field("server-timing") == std::string_view("db;dur=5"));
    RUVIA_CHECK(!trailers.field("content-length").has_value());
    RUVIA_CHECK(!trailers.field("host").has_value());

    std::pmr::vector<ruvia::http3_field> pseudo(&resource);
    pseudo.emplace_back("x-extra", "1", &resource);
    pseudo.emplace_back(":path", "/", &resource);
    RUVIA_CHECK(ruvia::append_http3_request_trailers(trailers, pseudo) ==
                std::optional<ruvia::http_request_trailer_error>(ruvia::http_request_trailer_error::invalid_field));
    RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{2});
    RUVIA_CHECK(!trailers.field("x-extra").has_value());

    std::pmr::vector<ruvia::http3_field> invalid_value(&resource);
    invalid_value.emplace_back("x-extra", "bad\r\nvalue", &resource);
    RUVIA_CHECK(ruvia::append_http3_request_trailers(trailers, invalid_value) ==
                std::optional<ruvia::http_request_trailer_error>(ruvia::http_request_trailer_error::invalid_field));
    RUVIA_CHECK_EQ(trailers.fields().size(), std::size_t{2});
}

RUVIA_TEST(http3_request_head_preserves_transport_early_data_provenance) {
    std::pmr::unsynchronized_pool_resource resource;
    auto event = make_headers_event(&resource,
        {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}}, true);
    event.early_data_ = true;
    auto head = ruvia::http3_request_head::parse(std::move(event), &resource);
    RUVIA_CHECK(head && head->early_data());
    auto ordinary = ruvia::http3_request_head::parse(make_headers_event(&resource,
                                                         {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}}, true),
        &resource);
    RUVIA_CHECK(ordinary && !ordinary->early_data());
}
