#include "test_harness.h"

#include <chrono>
#include <concepts>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "ruvia/web/detail/http/HttpCors.h"
#include "ruvia/web/detail/http/HttpCorsConfigValidation.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/App.h"
#include "ruvia/http/HttpResponse.h"

namespace {

bool corsConfigThrows(const ruvia::CorsConfig& config) {
    try {
        ruvia::detail::validateCorsConfig(config);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(cors_origin_policy_has_explicit_legal_alternatives) {
    static_assert(!std::default_initializable<ruvia::CorsOriginPolicy>);
    static_assert(!std::is_aggregate_v<ruvia::CorsOriginPolicy>);

    const auto any = ruvia::CorsOriginPolicy::any();
    const auto exact = ruvia::CorsOriginPolicy::exact("https://app.example.com");
    const auto credentialed =
        ruvia::CorsOriginPolicy::credentialed("https://app.example.com");
    RUVIA_CHECK(any.kind() == ruvia::CorsOriginPolicy::Kind::kAny);
    RUVIA_CHECK(exact.kind() == ruvia::CorsOriginPolicy::Kind::kExact);
    RUVIA_CHECK(
        credentialed.kind() ==
        ruvia::CorsOriginPolicy::Kind::kCredentialedExact);
}

RUVIA_TEST(cors_exact_origin_rejects_invalid_value_at_construction) {
    bool emptyThrew = false;
    bool wildcardCredentialsThrew = false;
    bool injectionThrew = false;
    try {
        (void)ruvia::CorsOriginPolicy::exact("");
    } catch (const std::invalid_argument&) {
        emptyThrew = true;
    }
    try {
        (void)ruvia::CorsOriginPolicy::credentialed("*");
    } catch (const std::invalid_argument&) {
        wildcardCredentialsThrew = true;
    }
    try {
        (void)ruvia::CorsOriginPolicy::credentialed("https://a\r\nX: y");
    } catch (const std::invalid_argument&) {
        injectionThrew = true;
    }
    RUVIA_CHECK(emptyThrew);
    RUVIA_CHECK(wildcardCredentialsThrew);
    RUVIA_CHECK(injectionThrew);
}

RUVIA_TEST(cors_header_values_reject_crlf_injection) {
    ruvia::CorsConfig cors;
    cors.exposeHeaders = "X-Bar\r\nX: y";
    RUVIA_CHECK(corsConfigThrows(cors));

    cors.exposeHeaders = "X-Exposed";
    RUVIA_CHECK(!corsConfigThrows(cors));
}

RUVIA_TEST(cors_fixed_request_headers_reject_invalid_value_at_construction) {
    bool emptyThrew = false;
    bool injectionThrew = false;
    try {
        (void)ruvia::CorsRequestHeadersPolicy::fixed("");
    } catch (const std::invalid_argument&) {
        emptyThrew = true;
    }
    try {
        (void)ruvia::CorsRequestHeadersPolicy::fixed("X-Foo\r\nX: y");
    } catch (const std::invalid_argument&) {
        injectionThrew = true;
    }
    RUVIA_CHECK(emptyThrew);
    RUVIA_CHECK(injectionThrew);
}

RUVIA_TEST(cors_max_age_rejects_negative_value_at_construction) {
    bool threw = false;
    try {
        (void)ruvia::CorsMaxAge(std::chrono::seconds(-1));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK_EQ(
        ruvia::CorsMaxAge(std::chrono::seconds(3600)).value(),
        std::chrono::seconds(3600));
}

namespace {

using ruvia::HttpResponse;
using ruvia::CorsConfig;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::applyCorsHeaders;

CorsConfig corsOptions(std::string_view configuredOrigin, bool credentials) {
    CorsConfig cors;
    if (configuredOrigin == "*") {
        cors.origin = ruvia::CorsOriginPolicy::any();
        return cors;
    }
    cors.origin = credentials
        ? ruvia::CorsOriginPolicy::credentialed(configuredOrigin)
        : ruvia::CorsOriginPolicy::exact(configuredOrigin);
    return cors;
}

}  // namespace

RUVIA_TEST(cors_max_age_distinguishes_absence_from_zero) {
    static_assert(std::same_as<
                  decltype(ruvia::CorsConfig{}.maxAge),
                  std::optional<ruvia::CorsMaxAge>>);

    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n\r\n");

    auto absent = corsOptions("https://app.example", false);
    HttpResponse absentResponse(std::pmr::new_delete_resource());
    applyCorsHeaders(result.request, absentResponse, absent);
    RUVIA_CHECK(!absentResponse.header("Access-Control-Max-Age").has_value());

    auto zero = corsOptions("https://app.example", false);
    zero.maxAge.emplace(std::chrono::seconds(0));
    HttpResponse zeroResponse(std::pmr::new_delete_resource());
    applyCorsHeaders(result.request, zeroResponse, zero);
    RUVIA_CHECK_EQ(
        zeroResponse.header("Access-Control-Max-Age").value_or(""),
        std::string_view("0"));
}

RUVIA_TEST(cors_runtime_sets_configured_origin_and_vary) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
    HttpResponse response(std::pmr::new_delete_resource());
    applyCorsHeaders(result.request, response, corsOptions("https://app.example", false));

    // The configured origin is emitted verbatim -- the request Origin is never reflected.
    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Origin").value_or(""),
                   std::string_view("https://app.example"));
    // A specific (non-wildcard) origin varies the response by Origin.
    RUVIA_CHECK(response.header("Vary").value_or("").find("Origin") != std::string_view::npos);
    RUVIA_CHECK(!response.header("Access-Control-Allow-Credentials").has_value());
}

RUVIA_TEST(cors_runtime_wildcard_has_no_vary_origin) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://any.example\r\n\r\n");
    HttpResponse response(std::pmr::new_delete_resource());
    applyCorsHeaders(result.request, response, corsOptions("*", false));

    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Origin").value_or(""), std::string_view("*"));
    RUVIA_CHECK(response.header("Vary").value_or("").find("Origin") == std::string_view::npos);
}

RUVIA_TEST(cors_runtime_credentials_belong_to_specific_origin) {
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage(
            "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
        HttpResponse response(std::pmr::new_delete_resource());
        applyCorsHeaders(result.request, response, corsOptions("https://app.example", true));
        RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Credentials").value_or(""), std::string_view("true"));
    }
}

RUVIA_TEST(cors_runtime_skips_non_cors_requests) {
    // No Origin header -> not a CORS request -> no CORS headers emitted.
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        HttpResponse response(std::pmr::new_delete_resource());
        applyCorsHeaders(result.request, response, corsOptions("https://app.example", false));
        RUVIA_CHECK(!response.header("Access-Control-Allow-Origin").has_value());
    }
}

RUVIA_TEST(cors_preflight_reflects_methods_and_requested_headers) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Headers: X-Custom\r\n\r\n");
    HttpResponse response(std::pmr::new_delete_resource());
    response.header("Allow", "GET, POST, OPTIONS");  // the route-advertised methods

    auto cors = corsOptions("https://app.example", false);
    cors.maxAge.emplace(std::chrono::seconds(600));
    // Reflect policy forwards the request's Access-Control-Request-Headers value.
    applyCorsHeaders(result.request, response, cors);

    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Methods").value_or(""),
                   std::string_view("GET, POST, OPTIONS"));
    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Headers").value_or(""), std::string_view("X-Custom"));
    RUVIA_CHECK_EQ(response.header("Access-Control-Max-Age").value_or(""), std::string_view("600"));
    RUVIA_CHECK(response.header("Vary").value_or("").find("Access-Control-Request-Method") != std::string_view::npos);
    RUVIA_CHECK(response.header("Vary").value_or("").find("Access-Control-Request-Headers") != std::string_view::npos);
}

RUVIA_TEST(cors_preflight_prefers_configured_allow_headers) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Headers: X-Requested\r\n\r\n");
    HttpResponse response(std::pmr::new_delete_resource());

    auto cors = corsOptions("https://app.example", false);
    cors.requestHeaders =
        ruvia::CorsRequestHeadersPolicy::fixed("Authorization, X-Configured");
    applyCorsHeaders(result.request, response, cors);

    // The configured allow-list wins over reflecting the requested headers.
    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Headers").value_or(""),
                   std::string_view("Authorization, X-Configured"));
}

RUVIA_TEST(cors_runtime_exposes_configured_headers_on_simple_response) {
    // A simple (non-preflight) response advertises which response headers
    // cross-origin script may read, via Access-Control-Expose-Headers. This
    // runtime branch had no coverage -- only the config validation of
    // exposeHeaders did -- so a regression dropping it would silently break
    // cross-origin header access.
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage(
            "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
        HttpResponse response(std::pmr::new_delete_resource());
        auto cors = corsOptions("https://app.example", false);
        cors.exposeHeaders.assign("X-Total-Count, X-Request-Id");
        applyCorsHeaders(result.request, response, cors);
        RUVIA_CHECK_EQ(response.header("Access-Control-Expose-Headers").value_or(""),
                       std::string_view("X-Total-Count, X-Request-Id"));
    }
    // Expose-Headers is meaningless on a preflight response and must NOT be
    // emitted there: the preflight path returns before the expose-headers branch.
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage(
            "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
            "Access-Control-Request-Method: POST\r\n\r\n");
        HttpResponse response(std::pmr::new_delete_resource());
        auto cors = corsOptions("https://app.example", false);
        cors.exposeHeaders.assign("X-Total-Count");
        applyCorsHeaders(result.request, response, cors);
        RUVIA_CHECK(!response.header("Access-Control-Expose-Headers").has_value());
    }
}
