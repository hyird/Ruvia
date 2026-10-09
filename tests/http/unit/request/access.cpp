#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/http1_request_parser.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_content_decoding.h"

#include "request/http_request_access.h"
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
using ruvia::detail::http_request_access;
using ruvia::detail::request_body_bytes;
using ruvia::detail::request_header_kind;
using ruvia::detail::request_known_header;

using ruvia::test::header_memory;

}  // namespace

RUVIA_TEST(request_header_blocks_reclaim_repeated_parses_and_preserve_retained_results) {
    header_memory resource;
    ruvia::http1_request_parser parser;
    {
        auto retained = parser.parse("GET /saved HTTP/1.1\r\nHost: example\r\nX-Data: saved\r\n\r\n", {.resource_ = &resource});
        RUVIA_CHECK(retained.parsed() != nullptr);
        const auto baseline = resource.live_bytes_;
        RUVIA_CHECK(baseline >= 2 * sizeof(http_header_view));
        RUVIA_CHECK(baseline < ruvia::max_http_header_fields * sizeof(http_header_view));
        for (int i = 0; i < 64; ++i) {
            {
                const auto before = resource.allocations_;
                auto result_value = parser.parse("GET / HTTP/1.1\r\nHost: second\r\n\r\n", {.resource_ = &resource});
                RUVIA_CHECK(result_value.parsed() != nullptr);
                RUVIA_CHECK_EQ(resource.allocations_, before + 1);
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

RUVIA_TEST(request_header_block_move_assignment_and_reset_release_storage) {
    header_memory first_resource;
    header_memory second_resource;
    auto first = http_request_access::make();
    auto second = http_request_access::make();
    http_request_access::set_resource(first, &first_resource);
    http_request_access::set_resource(second, &second_resource);
    http_request_access::reserve_headers(first, 1);
    http_request_access::reserve_headers(second, 1);
    const auto host = http_request_access::known_header_slot(request_header_kind::host);
    RUVIA_CHECK(http_request_access::add_header(first, {"Host", "first"}, host));
    RUVIA_CHECK(http_request_access::add_header(second, {"Host", "second"}, host));
    first = std::move(second);
    RUVIA_CHECK_EQ(first_resource.live_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(first.header("host").value(), std::string_view("second"));
    RUVIA_CHECK(!second.header("host").has_value());
    http_request_access::reset(first);
    RUVIA_CHECK_EQ(second_resource.live_bytes_, std::size_t{0});
}

RUVIA_TEST(request_access_reset_initializes_defaults) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    RUVIA_CHECK(request.method().empty());
    RUVIA_CHECK(request.known_method() == http_known_method::unknown);
    RUVIA_CHECK(request.target().empty());
    RUVIA_CHECK(request.scheme().empty());
    RUVIA_CHECK(request.authority().empty());
    RUVIA_CHECK(request.target_form() == http_request_target_form::origin);
    RUVIA_CHECK(request.protocol_version() == http_protocol_version::http11);
    RUVIA_CHECK(request.headers().empty());
    RUVIA_CHECK(request_body_bytes(request).empty());
}

RUVIA_TEST(request_public_reset_clears_borrowed_views_and_owned_headers) {
    header_memory resource;
    auto request = http_request_access::make();
    http_request_access::set_resource(request, &resource);
    http_request_access::set_method(request, "GET");
    http_request_access::set_target(request, "/path?key=value");
    http_request_access::set_scheme(request, "https");
    http_request_access::set_authority(request, "example.test");
    http_request_access::set_path(request, "/path");
    http_request_access::set_query_string(request, "key=value");
    http_request_access::set_body(request, "payload");
    http_request_access::reserve_headers(request, 1);
    RUVIA_CHECK(http_request_access::add_header(request, {"Host", "example.test"}));
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
    RUVIA_CHECK(request_body_bytes(request).empty());
    RUVIA_CHECK(request.protocol_version() == http_protocol_version::http11);
    RUVIA_CHECK(request.target_form() == http_request_target_form::origin);
}

RUVIA_TEST(request_access_preserves_target_components_and_form) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    http_request_access::set_target(request, "https://example.test/search?q=1");
    http_request_access::set_scheme(request, "https");
    http_request_access::set_authority(request, "example.test");
    http_request_access::set_target_form(request, http_request_target_form::absolute);

    RUVIA_CHECK_EQ(request.target(), std::string_view("https://example.test/search?q=1"));
    RUVIA_CHECK_EQ(request.scheme(), std::string_view("https"));
    RUVIA_CHECK_EQ(request.authority(), std::string_view("example.test"));
    RUVIA_CHECK(request.target_form() == http_request_target_form::absolute);
}

RUVIA_TEST(request_access_protocol_version_is_typed_control_data) {
    http_request request = http_request_access::make();
    http_request_access::set_protocol_version(request, http_protocol_version::http2);
    RUVIA_CHECK(request.protocol_version() == http_protocol_version::http2);
}

RUVIA_TEST(request_access_preserves_extension_method_token) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    http_request_access::set_method(request, "PROPFIND");
    RUVIA_CHECK_EQ(request.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(request.known_method() == http_known_method::unknown);
}

RUVIA_TEST(request_access_classified_and_unknown_header_lookup) {
    auto request = http_request_access::make();
    for (const auto field : {http_header_view{"aCcEpT", "text/plain"},
             http_header_view{"HOST", "example.com"},
             http_header_view{"User-Agent", "client"},
             http_header_view{"Sec-WebSocket-Extensions", "permessage-deflate"},
             http_header_view{"X-Request-Id", "request-id"}}) {
        RUVIA_CHECK(http_request_access::add_header(request, field));
        RUVIA_CHECK(request.header(field.name()) == field.value());
        const auto kind = ruvia::detail::classify_request_header(field.name());
        if (kind == request_header_kind::other) {
            RUVIA_CHECK(!http_request_access::has_known_header(request, kind));
            RUVIA_CHECK(http_request_access::known_header(request, kind).empty());
        } else {
            RUVIA_CHECK(http_request_access::has_known_header(request, kind));
            RUVIA_CHECK_EQ(http_request_access::known_header(request, kind), field.value());
        }
    }
    RUVIA_CHECK(request.header("accept") == "text/plain");
    RUVIA_CHECK(request.header("x-request-id") == "request-id");
}

RUVIA_TEST(request_access_known_header_last_write_wins) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    const auto slot = http_request_access::known_header_slot(request_header_kind::host);
    RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"Host", "first.example"}, slot));
    RUVIA_CHECK_EQ(
        request_known_header(request, request_header_kind::host), std::string_view("first.example"));
    RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"Host", "second.example"}, slot));
    RUVIA_CHECK_EQ(
        request_known_header(request, request_header_kind::host), std::string_view("second.example"));
    // An unpopulated known header reads back empty.
    RUVIA_CHECK(request_known_header(request, request_header_kind::user_agent).empty());
}

RUVIA_TEST(request_access_add_header_appends_and_caches) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"host", "example.com"},
        http_request_access::known_header_slot(request_header_kind::host)));
    RUVIA_CHECK_EQ(request.headers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(request.headers()[0].name(), std::string_view("host"));
    RUVIA_CHECK_EQ(request.headers()[0].value(), std::string_view("example.com"));
    // The two-argument overload also caches the value for fast known-header access.
    RUVIA_CHECK_EQ(
        request_known_header(request, request_header_kind::host), std::string_view("example.com"));
}

RUVIA_TEST(request_access_unknown_header_lookup_uses_last_match) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"X-Trace", "first"}));
    RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"x-trace", "second"}));

    RUVIA_CHECK_EQ(request.header("X-Trace"), std::string_view("second"));
}

RUVIA_TEST(request_header_distinguishes_missing_from_present_empty) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);

    RUVIA_CHECK(!request.header("X-Empty").has_value());
    RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"X-Empty", ""}));
    const auto present_empty = request.header("x-empty");
    RUVIA_CHECK(present_empty.has_value());
    RUVIA_CHECK(present_empty.value_or("missing").empty());

    const auto host_slot = http_request_access::known_header_slot(request_header_kind::host);
    RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"Host", ""}, host_slot));
    const auto known_present_empty = request.header("HOST");
    RUVIA_CHECK(known_present_empty.has_value());
    RUVIA_CHECK(known_present_empty.value_or("missing").empty());
}

RUVIA_TEST(request_access_known_header_lookup_uses_last_match) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    const auto slot = http_request_access::known_header_slot(request_header_kind::host);
    RUVIA_CHECK(
        http_request_access::add_header(request, http_header_view{"Host", "first.example"}, slot));
    RUVIA_CHECK(
        http_request_access::add_header(request, http_header_view{"host", "second.example"}, slot));

    RUVIA_CHECK_EQ(request.header("Host"), std::string_view("second.example"));
    RUVIA_CHECK_EQ(
        request_known_header(request, request_header_kind::host), std::string_view("second.example"));
}

RUVIA_TEST(request_content_coding_accumulates_repeated_header_fields_in_order) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    const auto slot = http_request_access::known_header_slot(request_header_kind::content_encoding);
    RUVIA_CHECK(
        http_request_access::add_header(request, http_header_view{"Content-Encoding", "br"}, slot));
    RUVIA_CHECK(
        http_request_access::add_header(request, http_header_view{"Content-Encoding", "gzip"}, slot));

    RUVIA_CHECK_EQ(request_known_header(request, request_header_kind::content_encoding),
        std::string_view("gzip"));
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
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    const auto slot = http_request_access::known_header_slot(request_header_kind::content_encoding);
    RUVIA_CHECK(
        http_request_access::add_header(request, http_header_view{"Content-Encoding", ","}, slot));
    RUVIA_CHECK(
        http_request_access::add_header(request, http_header_view{"Content-Encoding", "gzip"}, slot));

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
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    http_request_access::set_query_string(
        request, "a=first&b=2&a=second+value&encoded%20key=raw%2Fvalue");

    const auto value = request.last_raw_query_value("a");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(*value, std::string_view("second+value"));
    RUVIA_CHECK(!request.last_raw_query_value("encoded key").has_value());
    RUVIA_CHECK_EQ(*request.last_raw_query_value("encoded%20key"), std::string_view("raw%2Fvalue"));
}

RUVIA_TEST(request_access_cookie_lookup_uses_last_match) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    RUVIA_CHECK(http_request_access::add_header(request,
        http_header_view{"Cookie", "sid=first; theme=dark; sid=second"},
        http_request_access::known_header_slot(request_header_kind::cookie)));

    const auto value = request.cookie("sid");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(*value, std::string_view("second"));
}

RUVIA_TEST(request_access_cookie_lookup_scans_repeated_cookie_fields) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    const auto slot = http_request_access::known_header_slot(request_header_kind::cookie);
    RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"Cookie", "a=1"}, slot));
    RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"Cookie", "b=2"}, slot));

    const auto first = request.cookie("a");
    RUVIA_CHECK(first.has_value());
    RUVIA_CHECK_EQ(*first, std::string_view("1"));
    const auto second = request.cookie("b");
    RUVIA_CHECK(second.has_value());
    RUVIA_CHECK_EQ(*second, std::string_view("2"));
}

RUVIA_TEST(request_access_add_header_rejects_when_full) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    for (int i = 0; i < 64; ++i) {  // max_http_header_fields == 64
        RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"x", "y"}));
    }
    RUVIA_CHECK_EQ(request.headers().size(), std::size_t{64});
    RUVIA_CHECK(!http_request_access::add_header(request, http_header_view{"over", "flow"}));
    RUVIA_CHECK_EQ(request.headers().size(), std::size_t{64});
}

RUVIA_TEST(request_access_reset_clears_cached_headers) {
    http_request request = http_request_access::make();
    http_request_access::reset(request);
    RUVIA_CHECK(http_request_access::add_header(request, http_header_view{"Host", "h"},
        http_request_access::known_header_slot(request_header_kind::host)));
    // reset wipes cached known headers and appended headers.
    http_request_access::reset(request);
    RUVIA_CHECK(request_known_header(request, request_header_kind::host).empty());
    RUVIA_CHECK(request.headers().empty());
}
