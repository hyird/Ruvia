#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_client.h"
#include "ruvia/http/http_client_redirect.h"

#include "http_client_response_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::classify_http_client_origin_authority;
using ruvia::http_client_origin_authority_status;
using ruvia::http_client_redirect_content_disposition;
using ruvia::http_client_redirect_resolution_error;
using ruvia::http_client_request_view;
using ruvia::http_origin_view;
using ruvia::http_scheme;
using ruvia::is_http_client_redirect_status;
using ruvia::lookup_unique_http_client_response_header;
using ruvia::plan_http_client_redirect_request;
using ruvia::resolve_http_client_redirect_target;

http_origin_view origin_for(
    std::string_view host, std::uint16_t port, http_scheme scheme = http_scheme::http) {
    return scheme == http_scheme::https ? http_origin_view::https({.host_ = host, .port_ = port})
                                        : http_origin_view::http({.host_ = host, .port_ = port});
}

// Same-origin resolution is a followable redirect whose destination stays on
// the request origin; the cross-origin-capable resolver reports that as a
// resolved destination with cross_origin() == false.
void check_resolved_target(ruvia::testing::test_context& ruvia_ctx, const http_origin_view& origin,
    std::string_view current_target, std::string_view location, std::string_view expected) {
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = current_target,
                                                        .location_ = location,
                                                        .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(result_value.failure() == nullptr);
    RUVIA_CHECK(result_value.resolved() != nullptr);
    if (const auto* resolved = result_value.resolved()) {
        RUVIA_CHECK(!resolved->cross_origin());
        RUVIA_CHECK_EQ(resolved->target(), expected);
    }
}

void check_cross_origin_redirect(ruvia::testing::test_context& ruvia_ctx, const http_origin_view& origin,
    std::string_view current_target, std::string_view location) {
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = current_target,
                                                        .location_ = location,
                                                        .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(result_value.failure() == nullptr);
    RUVIA_CHECK(result_value.resolved() != nullptr);
    if (const auto* resolved = result_value.resolved()) {
        RUVIA_CHECK(resolved->cross_origin());
    }
}

void check_redirect_target_failure(ruvia::testing::test_context& ruvia_ctx,
    const http_origin_view& origin, std::string_view current_target, std::string_view location,
    http_client_redirect_resolution_error expected) {
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = current_target,
                                                        .location_ = location,
                                                        .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(result_value.resolved() == nullptr);
    RUVIA_CHECK(result_value.failure() != nullptr);
    if (const auto* failure = result_value.failure()) {
        RUVIA_CHECK(failure->error() == expected);
    }
}

}  // namespace

RUVIA_TEST(http_client_redirect_status_set) {
    for (const ruvia::http_status_code status : {ruvia::http_status::moved_permanently,
             ruvia::http_status::found, ruvia::http_status::see_other,
             ruvia::http_status::temporary_redirect, ruvia::http_status::permanent_redirect}) {
        RUVIA_CHECK(is_http_client_redirect_status(status));
    }
    for (const ruvia::http_status_code status :
        {ruvia::http_status::ok, ruvia::http_status::no_content,
            ruvia::http_status::multiple_choices, ruvia::http_status::not_modified,
            ruvia::http_status::use_proxy, ruvia::http_status_code::from_value(306),
            ruvia::http_status_code::from_value(399), ruvia::http_status::not_found}) {
        RUVIA_CHECK(!is_http_client_redirect_status(status));
    }
}

RUVIA_TEST(http_client_redirect_request_plan_follows_rfc) {
    // 303 selects a retrieval request. HEAD remains HEAD; every other method
    // becomes GET. The representation and content-specific fields are dropped.
    {
        http_client_request_view request;
        request.method_ = "PUT";
        request.content_ = ruvia::http_client_request_content_view::bytes("payload");
        const auto plan =
            plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::see_other});
        RUVIA_CHECK_EQ(plan.method(), std::string_view("GET"));
        RUVIA_CHECK(plan.content_disposition() == http_client_redirect_content_disposition::drop);
    }
    {
        http_client_request_view request;
        request.method_ = "HEAD";
        const auto plan =
            plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::see_other});
        RUVIA_CHECK_EQ(plan.method(), std::string_view("HEAD"));
        RUVIA_CHECK(plan.content_disposition() == http_client_redirect_content_disposition::drop);
    }

    // RFC 9110 permits the historical POST-to-GET rewrite for 301/302. Other
    // methods are not aliases for POST and retain both method and content.
    {
        http_client_request_view request;
        request.method_ = "POST";
        request.content_ = ruvia::http_client_request_content_view::bytes("payload");
        const auto plan =
            plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::found});
        RUVIA_CHECK_EQ(plan.method(), std::string_view("GET"));
        RUVIA_CHECK(plan.content_disposition() == http_client_redirect_content_disposition::drop);
    }
    {
        http_client_request_view request;
        request.method_ = "PUT";
        request.content_ = ruvia::http_client_request_content_view::bytes("payload");
        const auto plan = plan_http_client_redirect_request(
            request, {.status_ = ruvia::http_status::moved_permanently});
        RUVIA_CHECK_EQ(plan.method(), std::string_view("PUT"));
        RUVIA_CHECK(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
    }

    // Method tokens are case-sensitive: lowercase "post" is a distinct method.
    {
        http_client_request_view request;
        request.method_ = "post";
        const auto plan = plan_http_client_redirect_request(
            request, {.status_ = ruvia::http_status::moved_permanently});
        RUVIA_CHECK_EQ(plan.method(), std::string_view("post"));
        RUVIA_CHECK(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
    }

    // 307/308 never change method or content.
    for (const ruvia::http_status_code status :
        {ruvia::http_status::temporary_redirect, ruvia::http_status::permanent_redirect}) {
        http_client_request_view request;
        request.method_ = "POST";
        request.content_ = ruvia::http_client_request_content_view::bytes("payload");
        const auto plan = plan_http_client_redirect_request(request, {.status_ = status});
        RUVIA_CHECK_EQ(plan.method(), std::string_view("POST"));
        RUVIA_CHECK(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
    }
}

RUVIA_TEST(http_client_redirect_request_plan_owns_preserved_method) {
    std::string method = "PROPFIND";
    http_client_request_view request;
    request.method_ = method;

    const auto plan =
        plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::temporary_redirect});
    for (char& ch : method) {
        ch = 'X';
    }

    RUVIA_CHECK_EQ(plan.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
}

RUVIA_TEST(http_client_response_header_lookup_distinguishes_empty_and_repeated) {
    auto parsed = http_client_response_test::parse_response(
        "GET", "HTTP/1.1 302 Found\r\nLocation:\r\nContent-Length: 0");
    const auto& head = parsed.head_;

    const auto empty = lookup_unique_http_client_response_header(head, "location");
    RUVIA_CHECK(empty.absent() == nullptr);
    RUVIA_CHECK(empty.found() != nullptr);
    RUVIA_CHECK(empty.repeated() == nullptr);
    if (const auto* found = empty.found()) {
        RUVIA_CHECK(found->value().empty());
    }
    const auto missing = lookup_unique_http_client_response_header(head, "missing");
    RUVIA_CHECK(missing.absent() != nullptr);
    RUVIA_CHECK(missing.found() == nullptr);
    RUVIA_CHECK(missing.repeated() == nullptr);

    auto repeated_parsed = http_client_response_test::parse_response(
        "GET", "HTTP/1.1 302 Found\r\nLocation:\r\nLOCATION: /second\r\nContent-Length: 0");
    const auto repeated = lookup_unique_http_client_response_header(repeated_parsed.head_, "Location");
    RUVIA_CHECK(repeated.absent() == nullptr);
    RUVIA_CHECK(repeated.found() == nullptr);
    RUVIA_CHECK(repeated.repeated() != nullptr);
}

RUVIA_TEST(http_client_authority_matches_typed_origin) {
    const auto non_default = origin_for("example.com", 8080);
    const auto is = [](const http_origin_view& origin, std::string_view authority) {
        return classify_http_client_origin_authority(origin, authority);
    };
    RUVIA_CHECK(is(non_default, "example.com:8080") == http_client_origin_authority_status::same_origin);
    for (const std::string_view different :
        {"example.com", "example.com:9090", "other.com:8080", "example.com:0", "example.com:"}) {
        RUVIA_CHECK(is(non_default, different) == http_client_origin_authority_status::different_origin);
    }
    RUVIA_CHECK(is(non_default, "user@example.com:8080") ==
                http_client_origin_authority_status::invalid_authority);
    RUVIA_CHECK(
        is(non_default, "example.com:99999") == http_client_origin_authority_status::invalid_authority);

    RUVIA_CHECK(is(http_origin_view::http({.host_ = "example.com"}), "example.com") ==
                http_client_origin_authority_status::same_origin);
    RUVIA_CHECK(is(http_origin_view::http({.host_ = "example.com"}), "EXAMPLE.com:") ==
                http_client_origin_authority_status::same_origin);
    RUVIA_CHECK(is(http_origin_view::http({.host_ = "example.com"}), "exa%6dple.com") ==
                http_client_origin_authority_status::same_origin);
    RUVIA_CHECK(is(http_origin_view::http({.host_ = "!example"}), "%21example") ==
                http_client_origin_authority_status::different_origin);
    RUVIA_CHECK(is(http_origin_view::https({.host_ = "example.com"}), "example.com") ==
                http_client_origin_authority_status::same_origin);
    RUVIA_CHECK(is(http_origin_view::http({.host_ = "example.com", .port_ = 0}), "example.com:0") ==
                http_client_origin_authority_status::same_origin);
    RUVIA_CHECK(is(http_origin_view::http({.host_ = "example.com", .port_ = 0}), "example.com") ==
                http_client_origin_authority_status::different_origin);

    const auto v6 = origin_for("[::1]", 8080);
    RUVIA_CHECK(is(v6, "[::1]:8080") == http_client_origin_authority_status::same_origin);
    RUVIA_CHECK(is(v6, "[::2]:8080") == http_client_origin_authority_status::different_origin);
    RUVIA_CHECK(is(v6, "[::1]:") == http_client_origin_authority_status::different_origin);

    const auto future = http_origin_view::http({.host_ = "[v1.future]"});
    RUVIA_CHECK(is(future, "[V1.FUTURE]:") == http_client_origin_authority_status::same_origin);
}

RUVIA_TEST(http_client_same_origin_redirect_resolves_uri_references) {
    const auto origin = http_origin_view::http({.host_ = "example.com"});
    constexpr std::string_view current = "/base/dir/page?old=1";

    check_resolved_target(ruvia_ctx, origin, current, "/new/path", "/new/path");
    check_resolved_target(ruvia_ctx, origin, current, "http://example.com/next", "/next");
    check_resolved_target(ruvia_ctx, origin, current, "//example.com/rel", "/rel");
    check_resolved_target(
        ruvia_ctx, origin, current, "http://EXA%6dPLE.com:/normalized", "/normalized");
    check_resolved_target(ruvia_ctx, origin, current, "next", "/base/dir/next");
    check_resolved_target(ruvia_ctx, origin, current, "../other/./item", "/base/other/item");
    check_resolved_target(ruvia_ctx, origin, current, "?new=2", "/base/dir/page?new=2");
    check_resolved_target(ruvia_ctx, origin, current, "#fragment", current);
    check_resolved_target(ruvia_ctx, origin, current, "/next#part/one?x=%2F:@!$&'()*+,;=", "/next");
    check_resolved_target(ruvia_ctx, origin, current, "", current);
    check_resolved_target(ruvia_ctx, origin, current, "/a/../b#section", "/b");
}

RUVIA_TEST(http_client_same_origin_redirect_reports_rejection_reason) {
    const auto origin = http_origin_view::http({.host_ = "example.com"});

    check_cross_origin_redirect(ruvia_ctx, origin, "/current", "http://evil.com/next");
    check_cross_origin_redirect(ruvia_ctx, origin, "/current", "https://example.com/next");
    for (const std::string_view invalid :
        {"https://user@example.com/next", "https://example.com:99999/next",
            "http://user@example.com/next", "http://example.com:99999/next", "http:/broken",
            "/next#bad fragment", "/next#%zz", "/next#[bad]", "/next#first#second"}) {
        check_redirect_target_failure(ruvia_ctx, origin, "/current", invalid,
            http_client_redirect_resolution_error::invalid_location);
    }
    check_redirect_target_failure(
        ruvia_ctx, origin, "*", "/next", http_client_redirect_resolution_error::invalid_current_target);
}

RUVIA_TEST(http_client_redirect_validates_path_before_dot_segment_removal) {
    const auto origin = http_origin_view::http({.host_ = "example.com"});
    constexpr std::string_view invalid_locations[] = {
        "/bad space/../next", "/%/../next", "/%zz/../next", "/[bad]/../next",
        "/bad\\path/../next", "/bad\tpath/../next", "/bad\r\npath/../next",
        std::string_view("/bad\0path/../next", 17), "/bad\x7f/../next", "/caf\xc3\xa9/../next",
        "bad space/../next", "%zz/../next", "[bad]/../next",
        "http://example.com/%zz/../next", "//example.com/[bad]/../next"};
    for (const auto invalid : invalid_locations) {
        check_redirect_target_failure(ruvia_ctx, origin, "/base/page", invalid,
            http_client_redirect_resolution_error::invalid_location);
    }
    for (const std::string_view valid : {
             "/bad%20space/../next", "/%25/../next", "/%5Bbad%5D/../next",
             "http://example.com/%25/../next", "//example.com/%25/../next"}) {
        check_resolved_target(ruvia_ctx, origin, "/base/page", valid, "/next");
    }
}

RUVIA_TEST(http_client_redirect_empty_reference_path_preserves_base_path) {
    const auto origin = http_origin_view::http({.host_ = "example.com"});
    for (const std::string_view path : {"/a/./page", "/a/../page", "/a//.", "/a/.."}) {
        const std::string current = std::string(path) + "?old=1";
        for (const std::string_view location : {"", "#fragment"}) {
            check_resolved_target(ruvia_ctx, origin, current, location, current);
        }
        check_resolved_target(ruvia_ctx, origin, current, "?new=2", std::string(path) + "?new=2");
        check_resolved_target(ruvia_ctx, origin, current, "?", std::string(path) + "?");
    }
    check_resolved_target(ruvia_ctx, origin, "/a/../page?old=1", "//example.com", "/");
    check_resolved_target(ruvia_ctx, origin, "/a/../page?old=1", "next", "/next");
}

RUVIA_TEST(http_client_same_origin_redirect_supports_ipvfuture) {
    const auto origin = http_origin_view::http({.host_ = "[v1.future]"});
    check_resolved_target(ruvia_ctx, origin, "/current", "http://[V1.FUTURE]:/next", "/next");
}

RUVIA_TEST(http_client_redirect_relative_resolution_matches_rfc3986_examples) {
    const auto origin = http_origin_view::http({.host_ = "a"});
    constexpr std::string_view current = "/b/c/d;p?q";

    struct example final {
        std::string_view reference_;
        std::string_view target_;
    };
    constexpr example examples[] = {
        {"g", "/b/c/g"},
        {"./g", "/b/c/g"},
        {"g/", "/b/c/g/"},
        {"/g", "/g"},
        {"?y", "/b/c/d;p?y"},
        {"g?y", "/b/c/g?y"},
        {"#s", "/b/c/d;p?q"},
        {"g#s", "/b/c/g"},
        {";p", "/b/c/;p"},
        {"", "/b/c/d;p?q"},
        {".", "/b/c/"},
        {"./", "/b/c/"},
        {"..", "/b/"},
        {"../", "/b/"},
        {"../g", "/b/g"},
        {"../..", "/"},
        {"../../g", "/g"},
        {"../../../g", "/g"},
        {"/./g", "/g"},
        {"/../g", "/g"},
        {"g/./h", "/b/c/g/h"},
        {"g/../h", "/b/c/h"},
        {"g;x=1/../y", "/b/c/y"},
        {"/a//.", "/a//"},
        {"g//.", "/b/c/g//"},
        {"g?y/./x", "/b/c/g?y/./x"},
    };

    for (const auto& example : examples) {
        check_resolved_target(ruvia_ctx, origin, current, example.reference_, example.target_);
    }
}

namespace {

using ruvia::http_client_redirect_resolution_error;
using ruvia::resolve_http_client_redirect_target;

struct expected_resolved_redirect final {
    http_scheme scheme_;
    std::string_view host_;
    std::uint16_t port_;
    std::string_view target_;
    bool cross_origin_;
};

void check_resolved_redirect(ruvia::testing::test_context& ruvia_ctx, const http_origin_view& origin,
    std::string_view current_target, std::string_view location,
    const expected_resolved_redirect& expected) {
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = current_target,
                                                        .location_ = location,
                                                        .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(result_value.failure() == nullptr);
    RUVIA_CHECK(result_value.resolved() != nullptr);
    if (const auto* resolved = result_value.resolved()) {
        RUVIA_CHECK(resolved->scheme() == expected.scheme_);
        RUVIA_CHECK_EQ(resolved->host(), expected.host_);
        RUVIA_CHECK_EQ(resolved->port(), expected.port_);
        RUVIA_CHECK_EQ(resolved->target(), expected.target_);
        RUVIA_CHECK_EQ(resolved->cross_origin(), expected.cross_origin_);
    }
}

void check_redirect_resolution_failure(ruvia::testing::test_context& ruvia_ctx,
    const http_origin_view& origin, std::string_view current_target, std::string_view location,
    http_client_redirect_resolution_error expected) {
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = current_target,
                                                        .location_ = location,
                                                        .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(result_value.resolved() == nullptr);
    RUVIA_CHECK(result_value.failure() != nullptr);
    if (const auto* failure = result_value.failure()) {
        RUVIA_CHECK(failure->error() == expected);
    }
}

}  // namespace

RUVIA_TEST(http_client_redirect_resolution_same_origin_stays_relative) {
    const auto origin = origin_for("example.com", 80);
    check_resolved_redirect(ruvia_ctx, origin, "/a/b?old=1", "c?x=1",
        {http_scheme::http, "example.com", 80, "/a/c?x=1", false});
    check_resolved_redirect(ruvia_ctx, origin, "/a/b", "http://example.com/x",
        {http_scheme::http, "example.com", 80, "/x", false});
    // Explicit default port and a case-different host are the same origin.
    check_resolved_redirect(ruvia_ctx, origin, "/a/b", "HTTP://EXAMPLE.COM:80/x",
        {http_scheme::http, "EXAMPLE.COM", 80, "/x", false});
}

RUVIA_TEST(http_client_redirect_resolution_classifies_cross_origin) {
    const auto origin = origin_for("example.com", 80);
    // Different host, default port for the located scheme.
    check_resolved_redirect(ruvia_ctx, origin, "/a/b", "https://other.example/path?q=1",
        {http_scheme::https, "other.example", 443, "/path?q=1", true});
    // Same host, different port.
    check_resolved_redirect(ruvia_ctx, origin, "/a/b", "http://example.com:8080/x",
        {http_scheme::http, "example.com", 8080, "/x", true});
    // Scheme change alone crosses the origin even on the same host.
    check_resolved_redirect(ruvia_ctx, origin, "/a/b", "https://example.com/x",
        {http_scheme::https, "example.com", 443, "/x", true});
    // A protocol-relative reference keeps the scheme but moves authority.
    check_resolved_redirect(ruvia_ctx, origin, "/a/b", "//other.example/p",
        {http_scheme::http, "other.example", 80, "/p", true});
    // Path normalization and fragment stripping apply across origins too.
    check_resolved_redirect(ruvia_ctx, origin, "/a/b", "https://other.example/a/../b#frag",
        {http_scheme::https, "other.example", 443, "/b", true});
    // IPv6 literals keep their brackets, matching the http_origin_view contract.
    check_resolved_redirect(ruvia_ctx, origin, "/a/b", "http://[::1]:8080/x",
        {http_scheme::http, "[::1]", 8080, "/x", true});
}

RUVIA_TEST(http_client_redirect_resolution_builds_borrowing_origin) {
    const auto origin = origin_for("example.com", 80);
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = "/a/b",
                                                        .location_ = "https://other.example:8443/x",
                                                        .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(result_value.resolved() != nullptr);
    if (const auto* resolved = result_value.resolved()) {
        const auto next_origin = resolved->origin();
        RUVIA_CHECK(next_origin.scheme() == http_scheme::https);
        RUVIA_CHECK_EQ(next_origin.host(), std::string_view("other.example"));
        RUVIA_CHECK_EQ(next_origin.port(), std::uint16_t{8443});
    }
}

RUVIA_TEST(http_client_redirect_resolution_reports_typed_failures) {
    const auto origin = origin_for("example.com", 80);
    check_redirect_resolution_failure(ruvia_ctx, origin, "/a/b", "ftp://example.com/file",
        http_client_redirect_resolution_error::unsupported_scheme);
    check_redirect_resolution_failure(ruvia_ctx, origin, "/a/b", "mailto:someone@example.com",
        http_client_redirect_resolution_error::unsupported_scheme);
    // Userinfo remains rejected: RFC 9110 deprecates it and clients must not
    // leak credentials embedded by the peer.
    check_redirect_resolution_failure(ruvia_ctx, origin, "/a/b", "https://user@other.example/",
        http_client_redirect_resolution_error::invalid_location);
    check_redirect_resolution_failure(ruvia_ctx, origin, "/a/b", "http:opaque-without-authority",
        http_client_redirect_resolution_error::invalid_location);
    check_redirect_resolution_failure(ruvia_ctx, origin, "not-a-target", "/x",
        http_client_redirect_resolution_error::invalid_current_target);
}
