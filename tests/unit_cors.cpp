#include "test_harness.h"

#include <chrono>
#include <memory_resource>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/detail/http/HttpCors.h"
#include "ruvia/web/detail/http/HttpCorsConfigValidation.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/App.h"
#include "ruvia/http/HttpResponse.h"

namespace {

bool corsThrows(std::string_view allowOrigin, bool allowCredentials) {
    try {
        ruvia::detail::validateCorsFields(
            allowOrigin,
            /*allowHeaders=*/"",
            /*exposeHeaders=*/"",
            std::chrono::seconds(0),
            allowCredentials);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(cors_wildcard_with_credentials_rejected) {
    // "*" + credentials would force reflecting arbitrary origins with credentials.
    RUVIA_CHECK(corsThrows("*", /*allowCredentials=*/true));
}

RUVIA_TEST(cors_wildcard_without_credentials_allowed) {
    RUVIA_CHECK(!corsThrows("*", /*allowCredentials=*/false));
}

RUVIA_TEST(cors_explicit_origin_with_credentials_allowed) {
    RUVIA_CHECK(!corsThrows("https://app.example.com", /*allowCredentials=*/true));
}

namespace {

bool corsFieldsThrow(
    std::string_view allowOrigin,
    std::string_view allowHeaders,
    std::string_view exposeHeaders,
    std::chrono::seconds maxAge,
    bool allowCredentials) {
    try {
        ruvia::detail::validateCorsFields(
            allowOrigin, allowHeaders, exposeHeaders, maxAge, allowCredentials);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(cors_config_rejects_empty_origin) {
    RUVIA_CHECK(corsFieldsThrow("", "", "", std::chrono::seconds(0), false));
}

RUVIA_TEST(cors_header_values_reject_crlf_injection) {
    const std::string_view origin = "https://app.example.com";
    // A CRLF in any emitted CORS header value would enable response header injection.
    RUVIA_CHECK(corsFieldsThrow("https://a\r\nX: y", "", "", std::chrono::seconds(0), false));
    RUVIA_CHECK(corsFieldsThrow(origin, "X-Foo\r\nX: y", "", std::chrono::seconds(0), false));
    RUVIA_CHECK(corsFieldsThrow(origin, "", "X-Bar\r\nX: y", std::chrono::seconds(0), false));
    // Clean header values are accepted.
    RUVIA_CHECK(!corsFieldsThrow(origin, "X-Foo, X-Bar", "X-Exposed", std::chrono::seconds(0), false));
}

RUVIA_TEST(cors_negative_max_age_rejected) {
    const std::string_view origin = "https://app.example.com";
    RUVIA_CHECK(corsFieldsThrow(origin, "", "", std::chrono::seconds(-1), false));
    RUVIA_CHECK(!corsFieldsThrow(origin, "", "", std::chrono::seconds(0), false));
    RUVIA_CHECK(!corsFieldsThrow(origin, "", "", std::chrono::seconds(3600), false));
}

namespace {

using ruvia::HttpResponse;
using ruvia::CorsConfig;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::applyCorsHeaders;

CorsConfig corsOptions(std::string_view allowOrigin, bool credentials) {
    CorsConfig cors;
    cors.allowOrigin.assign(allowOrigin.data(), allowOrigin.size());
    cors.allowCredentials = credentials;
    return cors;
}

}  // namespace

RUVIA_TEST(cors_runtime_sets_configured_origin_and_vary) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
    HttpResponse response(std::pmr::new_delete_resource());
    applyCorsHeaders(result.request, response, corsOptions("https://app.example", false));

    // The configured origin is emitted verbatim -- the request Origin is never reflected.
    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Origin"),
                   std::string_view("https://app.example"));
    // A specific (non-wildcard) origin varies the response by Origin.
    RUVIA_CHECK(response.header("Vary").find("Origin") != std::string_view::npos);
    RUVIA_CHECK(response.header("Access-Control-Allow-Credentials").empty());
}

RUVIA_TEST(cors_runtime_wildcard_has_no_vary_origin) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://any.example\r\n\r\n");
    HttpResponse response(std::pmr::new_delete_resource());
    applyCorsHeaders(result.request, response, corsOptions("*", false));

    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Origin"), std::string_view("*"));
    RUVIA_CHECK(response.header("Vary").find("Origin") == std::string_view::npos);
}

RUVIA_TEST(cors_runtime_credentials_require_specific_origin) {
    // Specific origin + credentials -> Access-Control-Allow-Credentials: true.
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage(
            "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
        HttpResponse response(std::pmr::new_delete_resource());
        applyCorsHeaders(result.request, response, corsOptions("https://app.example", true));
        RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Credentials"), std::string_view("true"));
    }
    // Defense in depth: even if a wildcard+credentials config slipped past validation,
    // the runtime must never emit credentials alongside a wildcard origin.
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage(
            "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
        HttpResponse response(std::pmr::new_delete_resource());
        applyCorsHeaders(result.request, response, corsOptions("*", true));
        RUVIA_CHECK(response.header("Access-Control-Allow-Credentials").empty());
    }
}

RUVIA_TEST(cors_runtime_skips_non_cors_requests) {
    // No Origin header -> not a CORS request -> no CORS headers emitted.
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        HttpResponse response(std::pmr::new_delete_resource());
        applyCorsHeaders(result.request, response, corsOptions("https://app.example", false));
        RUVIA_CHECK(response.header("Access-Control-Allow-Origin").empty());
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
    cors.maxAge = std::chrono::seconds(600);
    // No configured allowHeaders -> the requested headers are reflected.
    applyCorsHeaders(result.request, response, cors);

    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Methods"),
                   std::string_view("GET, POST, OPTIONS"));
    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Headers"), std::string_view("X-Custom"));
    RUVIA_CHECK_EQ(response.header("Access-Control-Max-Age"), std::string_view("600"));
    RUVIA_CHECK(response.header("Vary").find("Access-Control-Request-Method") != std::string_view::npos);
    RUVIA_CHECK(response.header("Vary").find("Access-Control-Request-Headers") != std::string_view::npos);
}

RUVIA_TEST(cors_preflight_prefers_configured_allow_headers) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Headers: X-Requested\r\n\r\n");
    HttpResponse response(std::pmr::new_delete_resource());

    auto cors = corsOptions("https://app.example", false);
    cors.allowHeaders.assign("Authorization, X-Configured");
    applyCorsHeaders(result.request, response, cors);

    // The configured allow-list wins over reflecting the requested headers.
    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Headers"),
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
        RUVIA_CHECK_EQ(response.header("Access-Control-Expose-Headers"),
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
        RUVIA_CHECK(response.header("Access-Control-Expose-Headers").empty());
    }
}
