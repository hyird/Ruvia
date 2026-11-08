#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_request_head.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_trailers.h"

#include "failing_memory_resource.h"
#include "http3_relay.h"
#include "stl_allocation_fixture.h"

namespace {

static_assert(std::is_nothrow_move_constructible_v<ruvia::http3_request_head>);
static_assert(!std::is_move_assignable_v<ruvia::http3_request_head>);

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
TEST(Ruvia, http3_request_head_parses_get_received_over_nghttp3) {
    ruvia::testing::http3_peers peers;
    EXPECT_TRUE(peers.bind());
    constexpr std::array request_fields{
        ruvia::http3_field_view{":method", "GET"},
        ruvia::http3_field_view{":scheme", "https"},
        ruvia::http3_field_view{":authority", "example.test"},
        ruvia::http3_field_view{":path", "/search?q=h3&page=2"},
        ruvia::http3_field_view{"cookie", "session=abc"},
        ruvia::http3_field_view{"accept", "text/html"},
        ruvia::http3_field_view{"cookie", "theme=dark"},
    };
    EXPECT_TRUE(peers.client_.submit_request(0, request_fields, true) == ruvia::http3_status::accepted);

    std::pmr::unsynchronized_pool_resource resource;
    std::optional<ruvia::http3_headers_event> received;
    EXPECT_TRUE(peers.to_server([&](ruvia::http3_event& event) {
        if (auto* headers = std::get_if<ruvia::http3_headers_event>(&event)) {
            received.emplace(std::move(*headers));
        }
    }));
    EXPECT_TRUE(received.has_value());
    if (!received) {
        return;
    }

    const auto head = ruvia::http3_request_head::parse(std::move(*received), &resource);
    EXPECT_TRUE(head.has_value());
    if (!head) {
        return;
    }
    EXPECT_EQ(head->stream_id(), std::int64_t{0});
    EXPECT_TRUE(head->method() == "GET");
    EXPECT_TRUE(head->scheme() == "https");
    EXPECT_TRUE(head->authority() == "example.test");
    EXPECT_TRUE(head->path() == "/search?q=h3&page=2");
    EXPECT_TRUE(!head->connect());
    EXPECT_TRUE(head->stream_ended());
    EXPECT_TRUE(head->content() == ruvia::http_request_content_indication::no_content);

    const auto request = head->build_request(&resource);
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
        failing_memory_resource field_resource;
        failing_memory_resource projection_resource;
        {
            std::optional<ruvia::http3_request_head> head;
            {
                auto event = make_headers_event(&field_resource,
                    {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}}, true);
                for (const auto crumb : test_case.crumbs_) {
                    event.fields_.emplace_back("cookie", crumb, &field_resource);
                }
                auto source = ruvia::http3_request_head::parse(std::move(event), &projection_resource);
                ASSERT_TRUE(source.has_value());
                head.emplace(std::move(*source));
            }
            const auto request = head->build_request(&projection_resource);
            if (test_case.expected_.empty()) {
                EXPECT_TRUE(!request.header("cookie").has_value());
            } else {
                EXPECT_TRUE(request.header("cookie") == test_case.expected_);
            }
        }
        EXPECT_EQ(field_resource.live_allocations(), std::size_t{0});
        EXPECT_EQ(projection_resource.live_allocations(), std::size_t{0});
    }
}

TEST(Ruvia, http3_request_head_bounds_single_and_joined_cookie_values) {
    for (const bool split : {false, true}) {
        for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
            std::pmr::unsynchronized_pool_resource resource;
            const std::string first = "a=" + std::string(ruvia::max_http_header_bytes - 2 - (split ? 5 : 0) + excess, 'x');
            auto event = make_headers_event(&resource,
                {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
                    {"cookie", first}},
                true);
            if (split) {
                event.fields_.emplace_back("cookie", "b=2", &resource);
            }
            const auto head = ruvia::http3_request_head::parse(std::move(event), &resource);
            if (excess != 0) {
                EXPECT_TRUE(!head.has_value());
            } else {
                ASSERT_TRUE(head.has_value());
                const auto request = head->build_request(&resource);
                EXPECT_TRUE(request.header("cookie") == first + (split ? "; b=2" : ""));
            }
        }
    }
}

TEST(Ruvia, http3_request_head_cookie_merge_releases_resources_after_allocation_failure) {
    if (!ruvia::test::stl_allocation_failure_injection_enabled()) {
        GTEST_SKIP() << "requires STL without noexcept debug metadata allocation";
    }
    failing_memory_resource field_resource;
    failing_memory_resource projection_resource;
    {
        const std::string first = "a=" + std::string(128, 'x');
        auto event = make_headers_event(&field_resource,
            {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
                {"cookie", first}, {"cookie", "b=2"}},
            true);
        projection_resource.fail_after_stl_operation(0);
        EXPECT_THROW((void)ruvia::http3_request_head::parse(std::move(event), &projection_resource), std::bad_alloc);
    }
    EXPECT_EQ(field_resource.live_allocations(), std::size_t{0});
    EXPECT_EQ(projection_resource.live_allocations(), std::size_t{0});
}

// RFC 9114 §4.4: CONNECT carries only :method and :authority, and its target
// is the authority form.
TEST(Ruvia, http3_request_head_parses_connect_authority_target) {
    std::pmr::unsynchronized_pool_resource resource;
    const auto head = ruvia::http3_request_head::parse(
        make_headers_event(&resource, {{":method", "CONNECT"}, {":authority", "proxy.test:443"}}, false),
        &resource);
    EXPECT_TRUE(head.has_value());
    if (!head) {
        return;
    }
    EXPECT_TRUE(head->connect());
    EXPECT_TRUE(head->authority() == "proxy.test:443");
    EXPECT_TRUE(head->scheme().empty());
    EXPECT_TRUE(head->path().empty());

    const auto request = head->build_request(&resource, "tunnel");
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

// RFC 9114 §4.3.1: an https request needs :authority or Host.
TEST(Ruvia, http3_request_head_requires_authority_or_host_for_https) {
    EXPECT_TRUE(!parses({{":method", "GET"}, {":scheme", "https"}, {":path", "/"}}));

    std::pmr::unsynchronized_pool_resource resource;
    const auto head = ruvia::http3_request_head::parse(
        make_headers_event(
            &resource, {{":method", "GET"}, {":scheme", "https"}, {":path", "/"}, {"host", "example.test"}}, true),
        &resource);
    EXPECT_TRUE(head.has_value());
    if (head) {
        const auto request = head->build_request(&resource);
        EXPECT_TRUE(request.authority() == "example.test");
        EXPECT_TRUE(request.header("host") == std::string_view("example.test"));
    }
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

    std::pmr::unsynchronized_pool_resource resource;
    const auto head = ruvia::http3_request_head::parse(make_headers_event(&resource, fields, false), &resource);
    EXPECT_TRUE(head.has_value());
    if (head) {
        EXPECT_TRUE(head->declared_content_length() == std::optional<std::uint64_t>(5));
        EXPECT_TRUE(head->content() == ruvia::http_request_content_indication::will_follow);
    }
}

TEST(Ruvia, http3_request_head_rejects_invalid_fields) {
    const auto with_field = [](std::string_view name, std::string_view value) {
        return parses({{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"},
            {name, value}});
    };
    EXPECT_TRUE(with_field("x-trace", "valid value"));
    EXPECT_TRUE(!with_field("x-trace", std::string_view("bad\x01value")));
    EXPECT_TRUE(!with_field("x-trace", "split\r\nvalue"));
    EXPECT_TRUE(!with_field("x-trace", std::string_view("nul\0byte", 8)));
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
    std::pmr::unsynchronized_pool_resource resource;
    std::pmr::vector<ruvia::http3_field> fields(&resource);
    fields.emplace_back("content-length", "5", &resource);
    fields.emplace_back("x-checksum", "sha-256=abc", &resource);
    fields.emplace_back("host", "example.test", &resource);
    fields.emplace_back("server-timing", "db;dur=5", &resource);

    ruvia::http_request_trailers trailers(&resource);
    EXPECT_TRUE(!ruvia::append_http3_request_trailers(trailers, fields).has_value());
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
    EXPECT_TRUE(trailers.field("x-checksum") == std::string_view("sha-256=abc"));
    EXPECT_TRUE(trailers.field("server-timing") == std::string_view("db;dur=5"));
    EXPECT_TRUE(!trailers.field("content-length").has_value());
    EXPECT_TRUE(!trailers.field("host").has_value());

    std::pmr::vector<ruvia::http3_field> pseudo(&resource);
    pseudo.emplace_back("x-extra", "1", &resource);
    pseudo.emplace_back(":path", "/", &resource);
    EXPECT_TRUE(ruvia::append_http3_request_trailers(trailers, pseudo) ==
                std::optional<ruvia::http_request_trailer_error>(ruvia::http_request_trailer_error::invalid_field));
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
    EXPECT_TRUE(!trailers.field("x-extra").has_value());

    std::pmr::vector<ruvia::http3_field> invalid_value(&resource);
    invalid_value.emplace_back("x-extra", "bad\r\nvalue", &resource);
    EXPECT_TRUE(ruvia::append_http3_request_trailers(trailers, invalid_value) ==
                std::optional<ruvia::http_request_trailer_error>(ruvia::http_request_trailer_error::invalid_field));
    EXPECT_EQ(trailers.fields().size(), std::size_t{2});
}

TEST(Ruvia, http3_request_head_preserves_transport_early_data_provenance) {
    std::pmr::unsynchronized_pool_resource resource;
    auto event = make_headers_event(&resource,
        {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}}, true);
    event.early_data_ = true;
    auto head = ruvia::http3_request_head::parse(std::move(event), &resource);
    EXPECT_TRUE(head && head->early_data());
    auto ordinary = ruvia::http3_request_head::parse(make_headers_event(&resource,
                                                         {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}}, true),
        &resource);
    EXPECT_TRUE(ordinary && !ordinary->early_data());
}

TEST(Ruvia, http3_request_head_enforces_materialized_field_count_limit) {
    std::pmr::unsynchronized_pool_resource resource;
    // Host synthesis and a nonempty Cookie each consume one materialized field.
    for (const bool with_cookie : {false, true}) {
        const auto regular_limit = ruvia::max_http_header_fields - 1 - static_cast<std::size_t>(with_cookie);
        for (const std::size_t regular_count : {regular_limit, regular_limit + 1}) {
            auto event = make_headers_event(&resource,
                {{":method", "GET"}, {":scheme", "https"}, {":authority", "example.test"}, {":path", "/"}},
                true);
            for (std::size_t index = 0; index < regular_count; ++index) {
                event.fields_.emplace_back("x-field", "v", &resource);
            }
            if (with_cookie) {
                event.fields_.emplace_back("cookie", "a=1", &resource);
            }
            auto parsed = ruvia::http3_request_head::parse(std::move(event), &resource);
            if (regular_count == regular_limit) {
                ASSERT_TRUE(parsed.has_value());
                const auto request = parsed->build_request(&resource);
                EXPECT_EQ(request.headers().size(), ruvia::max_http_header_fields);
            } else {
                EXPECT_TRUE(!parsed.has_value());
            }
        }
    }
}
