#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_status.h"

#include "http1_request_message_fixture.h"
#include "request_header_memory_fixture.h"
#include "stl_allocation_fixture.h"

namespace {

using ruvia::http_content_coding;
using ruvia::http_header_view;
using ruvia::http_known_method;
using ruvia::http_protocol_version;
using ruvia::http_request;
using ruvia::http_request_target_form;
using ruvia::request_content_coding;
using ruvia::testing::parse_http1_request_message;

using ruvia::test::header_memory;

}  // namespace

TEST(Ruvia, request_header_blocks_reclaim_repeated_parses_and_preserve_retained_results) {
    header_memory resource;
    {
        auto retained = parse_http1_request_message("GET /saved HTTP/1.1\r\nHost: example\r\nX-Data: saved\r\n\r\n", &resource);
        EXPECT_TRUE(retained.complete());
        const auto baseline = resource.live_bytes_;
        for (int i = 0; i < 64; ++i) {
            {
                auto result_value = parse_http1_request_message("GET / HTTP/1.1\r\nHost: second\r\n\r\n", &resource);
                EXPECT_TRUE(result_value.complete());
                auto moved = std::move(result_value);
                EXPECT_EQ(moved.state_.request_.header("Host").value(), std::string_view("second"));
                EXPECT_EQ(retained.state_.request_.header("X-Data").value(), std::string_view("saved"));
            }
            EXPECT_EQ(resource.live_bytes_, baseline);
            {
                const auto incomplete = parse_http1_request_message("POST / HTTP/1.1\r\nHost: example\r\nContent-Length: 3\r\n\r\nx", &resource);
                EXPECT_TRUE(incomplete.need_body());
            }
            EXPECT_EQ(resource.live_bytes_, baseline);
        }
        resource.reject_ = true;
        bool failed = false;
        try {
            (void)parse_http1_request_message("GET / HTTP/1.1\r\nHost: example\r\n\r\n", &resource);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        EXPECT_EQ(resource.live_bytes_, baseline);
        EXPECT_EQ(retained.state_.request_.path(), std::string_view("/saved"));
    }
    EXPECT_EQ(resource.live_bytes_, std::size_t{0});
}

TEST(Ruvia, request_public_reset_clears_borrowed_views_and_owned_headers) {
    header_memory resource;
    const http_header_view headers[]{{"Host", "example.test"}};
    auto request = ruvia::make_parsed_http_request(
        "GET", "https://example.test/path?key=value", headers, {}, &resource);
    EXPECT_TRUE(resource.live_bytes_ > 0);
    {
        const http_header_view conflicting_host[]{{"Host", "conflicting.example"}};
        auto rebound_req = ruvia::make_parsed_http_request(
            "GET", "https://example.test/path", conflicting_host, {}, &resource);
        EXPECT_EQ(rebound_req.header("Host").value_or(""), std::string_view("example.test"));
    }
    request.reset();
    EXPECT_EQ(resource.live_bytes_, std::size_t{0});
    EXPECT_TRUE(request.method().empty());
    EXPECT_TRUE(request.target().empty());
    EXPECT_TRUE(request.scheme().empty());
    EXPECT_TRUE(request.authority().empty());
    EXPECT_TRUE(request.path().empty());
    EXPECT_TRUE(request.query_string().empty());
    EXPECT_TRUE(request.headers().empty());
    EXPECT_TRUE(!request.header("Host").has_value());
    EXPECT_TRUE(request.body_bytes().empty());
    EXPECT_TRUE(request.body().empty());
    EXPECT_TRUE(request.protocol_version() == http_protocol_version::http11);
    EXPECT_TRUE(request.target_form() == http_request_target_form::origin);
}

TEST(Ruvia, request_header_distinguishes_missing_from_present_empty) {
    const http_header_view headers[]{{"X-Empty", ""}, {"Host", ""}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", headers, {}, std::pmr::get_default_resource());
    EXPECT_TRUE(!request.header("X-Missing").has_value());
    const auto present_empty = request.header("x-empty");
    EXPECT_TRUE(present_empty.has_value());
    EXPECT_TRUE(present_empty.value_or("missing").empty());

    const auto known_present_empty = request.header("HOST");
    EXPECT_TRUE(known_present_empty.has_value());
    EXPECT_TRUE(known_present_empty.value_or("missing").empty());
}

TEST(Ruvia, parsed_request_applies_recipient_field_rules_like_wire_parsers) {
    // RFC 9110 section 5.3: only list fields may repeat; joined Range or
    // If-Range lines would otherwise resolve to their last line.
    const http_header_view duplicate_range[]{{"Range", "bytes=0-0"}, {"Range", "bytes=5-5"}};
    const http_header_view duplicate_if_range[]{{"If-Range", "\"y\""}, {"If-Range", "\"x\""}};
    const http_header_view duplicate_host[]{{"Host", "a.example"}, {"Host", "b.example"}};
    for (const auto headers : {std::span<const http_header_view>(duplicate_range),
             std::span<const http_header_view>(duplicate_if_range), std::span<const http_header_view>(duplicate_host)}) {
        bool rejected = false;
        try {
            (void)ruvia::make_parsed_http_request("GET", "/", headers, {}, std::pmr::get_default_resource());
        } catch (const ruvia::http_protocol_error& error) {
            rejected = error.status() == ruvia::http_status::bad_request;
        }
        EXPECT_TRUE(rejected);
    }

    const http_header_view repeated_list[]{{"Accept", "text/plain"}, {"Accept", "text/html"},
        {"User-Agent", "first/1"}, {"User-Agent", "second/2"}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", repeated_list, {}, std::pmr::get_default_resource());
    EXPECT_EQ(request.headers().size(), std::size_t{4});
}

TEST(Ruvia, request_content_coding_accumulates_repeated_header_fields_in_order) {
    const http_header_view headers[]{{"Content-Encoding", "br"}, {"Content-Encoding", "gzip"}};
    const auto request = ruvia::make_parsed_http_request(
        "POST", "/", headers, {}, std::pmr::get_default_resource());
    const auto coding = request_content_coding(request);
    EXPECT_TRUE(coding.invalid() == nullptr);
    EXPECT_TRUE(coding.unsupported() == nullptr);
    EXPECT_EQ(coding.codings().size(), 2U);
    if (coding.codings().size() == 2) {
        EXPECT_TRUE(coding.codings()[0] == http_content_coding::brotli);
        EXPECT_TRUE(coding.codings()[1] == http_content_coding::gzip);
    }
}

TEST(Ruvia, request_content_coding_combines_field_lines_with_list_semantics) {
    const http_header_view headers[]{{"Content-Encoding", ","}, {"Content-Encoding", "gzip"}};
    const auto request = ruvia::make_parsed_http_request(
        "POST", "/", headers, {}, std::pmr::get_default_resource());

    const auto coding = request_content_coding(request);
    EXPECT_TRUE(coding.invalid() == nullptr);
    EXPECT_TRUE(coding.unsupported() == nullptr);
    EXPECT_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        EXPECT_TRUE(coding.codings().front() == http_content_coding::gzip);
    }
}

TEST(Ruvia, request_access_raw_query_lookup_preserves_encoding_and_uses_last_match) {
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/?a=first&b=2&a=second+value&encoded%20key=raw%2Fvalue",
        {}, {}, std::pmr::get_default_resource());

    const auto value = request.last_raw_query_value("a");
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(*value, std::string_view("second+value"));
    EXPECT_TRUE(!request.last_raw_query_value("encoded key").has_value());
    EXPECT_EQ(*request.last_raw_query_value("encoded%20key"), std::string_view("raw%2Fvalue"));
}

TEST(Ruvia, request_access_cookie_lookup_uses_last_match) {
    const http_header_view headers[]{{"Cookie", "sid=first; theme=dark; sid=second"}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", headers, {}, std::pmr::get_default_resource());

    const auto value = request.cookie("sid");
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(*value, std::string_view("second"));
}

TEST(Ruvia, request_access_cookie_lookup_scans_repeated_cookie_fields) {
    const http_header_view headers[]{{"Cookie", "a=1"}, {"Cookie", "b=2"}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", headers, {}, std::pmr::get_default_resource());

    const auto first = request.cookie("a");
    EXPECT_TRUE(first.has_value());
    EXPECT_EQ(*first, std::string_view("1"));
    const auto second = request.cookie("b");
    EXPECT_TRUE(second.has_value());
    EXPECT_EQ(*second, std::string_view("2"));
}

TEST(Ruvia, request_access_cookie_lookup_preserves_distinct_fields_with_shared_storage) {
    constexpr std::string_view shared = "sid=old; sid=new; token=present";
    const http_header_view headers[]{{"Cookie", shared}, {"Cookie", shared.substr(0, 7)}};
    const auto request = ruvia::make_parsed_http_request(
        "GET", "/", headers, {}, std::pmr::get_default_resource());
    EXPECT_EQ(request.headers().size(), std::size_t{2});
    EXPECT_EQ(request.header("Cookie").value_or("missing"), std::string_view("sid=old"));
    EXPECT_EQ(request.cookie("sid").value_or("missing"), std::string_view("old"));
    EXPECT_EQ(request.cookie("token").value_or("missing"), std::string_view("present"));
    EXPECT_TRUE(!request.cookie("absent").has_value());
}

TEST(Ruvia, owned_header_copies_stay_in_the_source_resource) {
    header_memory memory;
    {
        std::pmr::vector<ruvia::http_header> headers(&memory);
        headers.emplace_back("x-name", "value", &memory);
        const auto before = memory.allocations_;
        const std::pmr::vector<ruvia::http_header> copied(headers, &memory);
        const ruvia::http_header single(headers.front());
        EXPECT_EQ(copied.front().name(), std::string_view("x-name"));
        EXPECT_EQ(single.value(), std::string_view("value"));
        // Vector storage plus both field blocks come from the owning resource.
        if constexpr (ruvia::test::stl_moves_allocate_debug_metadata) {
            EXPECT_TRUE(memory.allocations_ >= before + 3);
        } else {
            EXPECT_EQ(memory.allocations_, before + 3);
        }
    }
    EXPECT_EQ(memory.live_bytes_, std::size_t{0});
}

TEST(Ruvia, owned_header_lowercase_copy_normalizes_only_the_name_in_one_allocation) {
    header_memory memory;
    {
        const auto before = memory.allocations_;
        const auto header = ruvia::http_header::copy_with_lowercase_name("X-Trace-ID", "AbC", &memory);
        EXPECT_EQ(header.name(), std::string_view("x-trace-id"));
        EXPECT_EQ(header.value(), std::string_view("AbC"));
        EXPECT_EQ(memory.allocations_, before + 1);
    }
    EXPECT_EQ(memory.live_bytes_, std::size_t{0});
}

TEST(Ruvia, request_body_string_view_and_known_method_bits_match_contracts) {
    const std::array<std::byte, 4> payload{
        std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}};
    const auto request = ruvia::make_parsed_http_request(
        "POST", "/", {}, payload, std::pmr::get_default_resource());
    EXPECT_EQ(request.body(), std::string_view("data"));
    EXPECT_EQ(request.body_bytes().size(), std::size_t{4});
    static_assert(ruvia::http_known_method_bit(http_known_method::get) == 1U);
    static_assert(ruvia::http_known_method_bit(http_known_method::unknown) == 0U);
}

namespace {

template <typename method_type, typename target_type, typename body_type>
concept make_parsed_request_accepts = requires(method_type&& method, target_type&& target, body_type&& body) {
    ruvia::make_parsed_http_request(std::forward<method_type>(method), std::forward<target_type>(target),
        std::span<const ruvia::http_header_view>{}, std::forward<body_type>(body),
        std::pmr::get_default_resource());
};

static_assert(make_parsed_request_accepts<const std::string&, const std::string&, const std::vector<std::byte>&>);
static_assert(!make_parsed_request_accepts<std::string, const std::string&, const std::vector<std::byte>&>);
static_assert(!make_parsed_request_accepts<const std::string&, std::string, const std::vector<std::byte>&>);
static_assert(!make_parsed_request_accepts<const std::string&, const std::string&, std::vector<std::byte>>);
static_assert(!make_parsed_request_accepts<const std::string&, const std::string&, std::array<std::byte, 1>>);

template <typename request_type>
concept request_allows_body = requires(request_type&& req) {
    std::forward<request_type>(req).body();
};
static_assert(request_allows_body<const http_request&>);
static_assert(!request_allows_body<http_request>);

}  // namespace
