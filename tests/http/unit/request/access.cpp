#include <gtest/gtest.h>

#include <array>
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
#include "memory_resource_fixture.h"
#include "stl_allocation_fixture.h"

namespace {

using ruvia::http_content_coding;
using ruvia::http_header_view;
using ruvia::http_known_method;
using ruvia::http_protocol_version;
using ruvia::http_request;
using ruvia::http_request_target_form;
using ruvia::request_content_coding;
using ruvia::test::parse_http1_request_message;

using ruvia::test::counting_memory_resource;
using ruvia::test::reentrant_memory_resource;

struct request_retirement_observation final {
    http_request* request_;
    http_request* replacement_;
    bool committed_{};
};

}  // namespace

TEST(Ruvia, request_header_blocks_reclaim_repeated_parses_and_preserve_retained_results) {
    counting_memory_resource resource;
    {
        auto retained = parse_http1_request_message("GET /saved HTTP/1.1\r\nHost: example\r\nX-Data: saved\r\n\r\n", &resource);
        EXPECT_TRUE(retained.complete());
        const auto baseline = resource.live_bytes();
        for (int i = 0; i < 64; ++i) {
            {
                auto result_value = parse_http1_request_message("GET / HTTP/1.1\r\nHost: second\r\n\r\n", &resource);
                EXPECT_TRUE(result_value.complete());
                auto moved = std::move(result_value);
                EXPECT_EQ(moved.state_.request().header("Host").value(), std::string_view("second"));
                EXPECT_EQ(retained.state_.request().header("X-Data").value(), std::string_view("saved"));
            }
            EXPECT_EQ(resource.live_bytes(), baseline);
            {
                const auto incomplete = parse_http1_request_message("POST / HTTP/1.1\r\nHost: example\r\nContent-Length: 3\r\n\r\nx", &resource);
                EXPECT_TRUE(incomplete.need_body());
            }
            EXPECT_EQ(resource.live_bytes(), baseline);
        }
        resource.reject_allocations(true);
        bool failed = false;
        try {
            (void)parse_http1_request_message("GET / HTTP/1.1\r\nHost: example\r\n\r\n", &resource);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        EXPECT_EQ(resource.live_bytes(), baseline);
        EXPECT_EQ(retained.state_.request().path(), std::string_view("/saved"));
    }
    EXPECT_EQ(resource.live_bytes(), std::size_t{0});
}

TEST(Ruvia, request_public_reset_clears_borrowed_views_and_owned_headers) {
    counting_memory_resource resource;
    const http_header_view headers[]{{"Host", "example.test"}};
    auto request = ruvia::make_parsed_http_request(
        "GET", "https://example.test/path?key=value", headers, {}, &resource);
    EXPECT_TRUE(resource.live_bytes() > 0);
    {
        const http_header_view conflicting_host[]{{"Host", "conflicting.example"}};
        auto rebound_req = ruvia::make_parsed_http_request(
            "GET", "https://example.test/path", conflicting_host, {}, &resource);
        EXPECT_EQ(rebound_req.header("Host").value_or(""), std::string_view("example.test"));
    }
    request.reset();
    EXPECT_EQ(resource.live_bytes(), std::size_t{0});
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

TEST(Ruvia, request_reset_commits_before_retirement_and_preserves_reentrant_replacement) {
    reentrant_memory_resource original_resource;
    counting_memory_resource replacement_resource;
    const http_header_view original_headers[]{{"Host", "old.test"}, {"Cookie", "sid=old"}};
    const http_header_view replacement_headers[]{{"Cookie", "sid=new"}, {"Host", "new.test"}};
    const std::array original_body{std::byte{'o'}, std::byte{'l'}, std::byte{'d'}};
    const std::array replacement_body{std::byte{'n'}, std::byte{'e'}, std::byte{'w'}};
    auto request = ruvia::make_parsed_http_request(
        "POST", "https://old.test/old?key=old", original_headers, original_body, &original_resource);
    auto replacement = ruvia::make_parsed_http_request(
        "PUT", "https://new.test/new?key=new", replacement_headers, replacement_body, &replacement_resource);
    request_retirement_observation observation{&request, &replacement};
    original_resource.deallocation_context_ = &observation;
    original_resource.before_deallocate_ = [](void* context) noexcept {
        auto& state = *static_cast<request_retirement_observation*>(context);
        const auto& value = *state.request_;
        state.committed_ = value.method().empty() && value.known_method() == http_known_method::unknown &&
                           value.target().empty() && value.scheme().empty() && value.authority().empty() &&
                           value.path().empty() && value.query_string().empty() && value.headers().empty() &&
                           !value.header("Host") && !value.cookie("sid") && value.body().empty();
        if (state.committed_) {
            *state.request_ = std::move(*state.replacement_);
        }
    };
    original_resource.reject_allocations(true);
    replacement_resource.reject_allocations(true);
    request.reset();
    EXPECT_TRUE(observation.committed_);
    EXPECT_EQ(request.method(), std::string_view("PUT"));
    EXPECT_EQ(request.target(), std::string_view("https://new.test/new?key=new"));
    EXPECT_EQ(request.header("Host").value_or(""), std::string_view("new.test"));
    EXPECT_EQ(request.cookie("sid").value_or(""), std::string_view("new"));
    EXPECT_EQ(request.body(), std::string_view("new"));
    EXPECT_TRUE(replacement.headers().empty());
    EXPECT_EQ(original_resource.live_bytes(), std::size_t{0});
    request.reset();
    EXPECT_EQ(replacement_resource.live_bytes(), std::size_t{0});
}

TEST(Ruvia, request_move_commits_all_views_before_retirement_and_preserves_reentrant_reset) {
    reentrant_memory_resource original_resource;
    counting_memory_resource replacement_resource;
    const http_header_view original_headers[]{{"Host", "old.test"}, {"Cookie", "sid=old"}};
    const http_header_view replacement_headers[]{{"Cookie", "sid=new"}, {"Host", "new.test"}};
    const std::array original_body{std::byte{'o'}, std::byte{'l'}, std::byte{'d'}};
    const std::array replacement_body{std::byte{'n'}, std::byte{'e'}, std::byte{'w'}};
    auto request = ruvia::make_parsed_http_request(
        "GET", "/old", original_headers, original_body, &original_resource);
    auto replacement = ruvia::make_parsed_http_request(
        "POST", "https://new.test/new?key=new", replacement_headers, replacement_body, &replacement_resource);
    request_retirement_observation observation{&request, &replacement};
    original_resource.deallocation_context_ = &observation;
    original_resource.before_deallocate_ = [](void* context) noexcept {
        auto& state = *static_cast<request_retirement_observation*>(context);
        const auto& value = *state.request_;
        state.committed_ = value.method() == "POST" && value.known_method() == http_known_method::post &&
                           value.target() == "https://new.test/new?key=new" && value.scheme() == "https" &&
                           value.authority() == "new.test" && value.path() == "/new" &&
                           value.query_string() == "key=new" && value.header("Host") == "new.test" &&
                           value.cookie("sid") == "new" && value.body() == "new" &&
                           state.replacement_->headers().empty();
        if (state.committed_) {
            state.request_->reset();
        }
    };
    original_resource.reject_allocations(true);
    replacement_resource.reject_allocations(true);
    request = std::move(replacement);
    EXPECT_TRUE(observation.committed_);
    EXPECT_TRUE(request.method().empty());
    EXPECT_TRUE(request.headers().empty());
    EXPECT_TRUE(request.body().empty());
    EXPECT_EQ(original_resource.live_bytes(), std::size_t{0});
    request.reset();
    EXPECT_EQ(replacement_resource.live_bytes(), std::size_t{0});
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
    counting_memory_resource memory;
    {
        std::pmr::vector<ruvia::http_header> headers(&memory);
        headers.emplace_back("x-name", "value", &memory);
        const auto before = memory.allocation_count();
        const std::pmr::vector<ruvia::http_header> copied(headers, &memory);
        const ruvia::http_header single(headers.front());
        EXPECT_EQ(copied.front().name(), std::string_view("x-name"));
        EXPECT_EQ(single.value(), std::string_view("value"));
        // Vector storage plus both field blocks come from the owning resource.
        if constexpr (ruvia::test::stl_moves_allocate_debug_metadata) {
            EXPECT_TRUE(memory.allocation_count() >= before + 3);
        } else {
            EXPECT_EQ(memory.allocation_count(), before + 3);
        }
    }
    EXPECT_EQ(memory.live_bytes(), std::size_t{0});
}

TEST(Ruvia, owned_header_lowercase_copy_normalizes_only_the_name_in_one_allocation) {
    counting_memory_resource memory;
    {
        const auto before = memory.allocation_count();
        const auto header = ruvia::http_header::copy_with_lowercase_name("X-Trace-ID", "AbC", &memory);
        EXPECT_EQ(header.name(), std::string_view("x-trace-id"));
        EXPECT_EQ(header.value(), std::string_view("AbC"));
        EXPECT_EQ(memory.allocation_count(), before + 1);
    }
    EXPECT_EQ(memory.live_bytes(), std::size_t{0});
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
