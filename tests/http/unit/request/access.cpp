#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http1_request_parser.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_content_decoding.h"

#include "request_header_memory_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::http_content_coding;
using ruvia::http_header_view;
using ruvia::http_known_method;
using ruvia::http_protocol_version;
using ruvia::http_request;
using ruvia::http_request_target_form;
using ruvia::request_content_coding;

using ruvia::test::header_memory;

}  // namespace

RUVIA_TEST(request_header_blocks_reclaim_repeated_parses_and_preserve_retained_results) {
    header_memory resource;
    ruvia::http1_request_parser parser;
    {
        auto retained = parser.parse("GET /saved HTTP/1.1\r\nHost: example\r\nX-Data: saved\r\n\r\n", {.resource_ = &resource});
        RUVIA_CHECK(retained.parsed() != nullptr);
        const auto baseline = resource.live_bytes_;
        for (int i = 0; i < 64; ++i) {
            {
                auto result_value = parser.parse("GET / HTTP/1.1\r\nHost: second\r\n\r\n", {.resource_ = &resource});
                RUVIA_CHECK(result_value.parsed() != nullptr);
                auto moved = std::move(result_value);
                RUVIA_CHECK_EQ(moved.parsed()->request().header("Host").value(), std::string_view("second"));
                RUVIA_CHECK_EQ(retained.parsed()->request().header("X-Data").value(), std::string_view("saved"));
            }
            RUVIA_CHECK_EQ(resource.live_bytes_, baseline);
            auto incomplete = parser.parse("POST / HTTP/1.1\r\nHost: example\r\nContent-Length: 3\r\n\r\nx", {.resource_ = &resource});
            RUVIA_CHECK(incomplete.need_more() != nullptr);
            RUVIA_CHECK_EQ(resource.live_bytes_, baseline);
        }
        resource.reject_ = true;
        bool failed = false;
        try {
            (void)parser.parse("GET / HTTP/1.1\r\nHost: example\r\n\r\n", {.resource_ = &resource});
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.live_bytes_, baseline);
        RUVIA_CHECK_EQ(retained.parsed()->request().path(), std::string_view("/saved"));
    }
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}

RUVIA_TEST(request_public_reset_clears_borrowed_views_and_owned_headers) {
    header_memory resource;
    const http_header_view headers[]{{"Host", "example.test"}};
    auto [request, error] = ruvia::make_parsed_http_request(
        "GET", "https://example.test/path?key=value", headers, {}, &resource);
    RUVIA_CHECK(!error.has_value());
    RUVIA_CHECK(resource.live_bytes_ > 0);
    request.reset();
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
    RUVIA_CHECK(request.method().empty());
    RUVIA_CHECK(request.target().empty());
    RUVIA_CHECK(request.scheme().empty());
    RUVIA_CHECK(request.authority().empty());
    RUVIA_CHECK(request.path().empty());
    RUVIA_CHECK(request.query_string().empty());
    RUVIA_CHECK(request.headers().empty());
    RUVIA_CHECK(!request.header("Host").has_value());
    RUVIA_CHECK(request.body_bytes().empty());
    RUVIA_CHECK(request.protocol_version() == http_protocol_version::http11);
    RUVIA_CHECK(request.target_form() == http_request_target_form::origin);
}

RUVIA_TEST(request_header_distinguishes_missing_from_present_empty) {
    const http_header_view headers[]{{"X-Empty", ""}, {"Host", ""}};
    auto [request, error] = ruvia::make_parsed_http_request(
        "GET", "/", headers, {}, std::pmr::get_default_resource());
    RUVIA_CHECK(!error.has_value());
    RUVIA_CHECK(!request.header("X-Missing").has_value());
    const auto present_empty = request.header("x-empty");
    RUVIA_CHECK(present_empty.has_value());
    RUVIA_CHECK(present_empty.value_or("missing").empty());

    const auto known_present_empty = request.header("HOST");
    RUVIA_CHECK(known_present_empty.has_value());
    RUVIA_CHECK(known_present_empty.value_or("missing").empty());
}

RUVIA_TEST(request_content_coding_accumulates_repeated_header_fields_in_order) {
    const http_header_view headers[]{{"Content-Encoding", "br"}, {"Content-Encoding", "gzip"}};
    auto [request, error] = ruvia::make_parsed_http_request(
        "POST", "/", headers, {}, std::pmr::get_default_resource());
    RUVIA_CHECK(!error.has_value());
    std::pmr::monotonic_buffer_resource resource;
    const auto coding = request_content_coding(request, &resource);
    RUVIA_CHECK(coding.invalid() == nullptr);
    RUVIA_CHECK(coding.unsupported() == nullptr);
    RUVIA_CHECK_EQ(coding.codings().size(), 2U);
    if (coding.codings().size() == 2) {
        RUVIA_CHECK(coding.codings()[0] == http_content_coding::brotli);
        RUVIA_CHECK(coding.codings()[1] == http_content_coding::gzip);
    }
}

RUVIA_TEST(request_content_coding_combines_field_lines_with_list_semantics) {
    const http_header_view headers[]{{"Content-Encoding", ","}, {"Content-Encoding", "gzip"}};
    auto [request, error] = ruvia::make_parsed_http_request(
        "POST", "/", headers, {}, std::pmr::get_default_resource());
    RUVIA_CHECK(!error.has_value());

    std::pmr::monotonic_buffer_resource resource;
    const auto coding = request_content_coding(request, &resource);
    RUVIA_CHECK(coding.invalid() == nullptr);
    RUVIA_CHECK(coding.unsupported() == nullptr);
    RUVIA_CHECK_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        RUVIA_CHECK(coding.codings().front() == http_content_coding::gzip);
    }
}

RUVIA_TEST(request_access_raw_query_lookup_preserves_encoding_and_uses_last_match) {
    auto [request, error] = ruvia::make_parsed_http_request(
        "GET", "/?a=first&b=2&a=second+value&encoded%20key=raw%2Fvalue",
        {}, {}, std::pmr::get_default_resource());
    RUVIA_CHECK(!error.has_value());

    const auto value = request.last_raw_query_value("a");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(*value, std::string_view("second+value"));
    RUVIA_CHECK(!request.last_raw_query_value("encoded key").has_value());
    RUVIA_CHECK_EQ(*request.last_raw_query_value("encoded%20key"), std::string_view("raw%2Fvalue"));
}

RUVIA_TEST(request_access_cookie_lookup_uses_last_match) {
    const http_header_view headers[]{{"Cookie", "sid=first; theme=dark; sid=second"}};
    auto [request, error] = ruvia::make_parsed_http_request(
        "GET", "/", headers, {}, std::pmr::get_default_resource());
    RUVIA_CHECK(!error.has_value());

    const auto value = request.cookie("sid");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(*value, std::string_view("second"));
}

RUVIA_TEST(request_access_cookie_lookup_scans_repeated_cookie_fields) {
    const http_header_view headers[]{{"Cookie", "a=1"}, {"Cookie", "b=2"}};
    auto [request, error] = ruvia::make_parsed_http_request(
        "GET", "/", headers, {}, std::pmr::get_default_resource());
    RUVIA_CHECK(!error.has_value());

    const auto first = request.cookie("a");
    RUVIA_CHECK(first.has_value());
    RUVIA_CHECK_EQ(*first, std::string_view("1"));
    const auto second = request.cookie("b");
    RUVIA_CHECK(second.has_value());
    RUVIA_CHECK_EQ(*second, std::string_view("2"));
}
