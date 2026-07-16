#include "test_harness.h"

#include <chrono>
#include <concepts>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "ruvia/web/detail/http/HttpCors.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/App.h"
#include "ruvia/http/HttpResponse.h"

RUVIA_TEST(cors_origin_policy_has_explicit_legal_alternatives) {
    static_assert(!std::default_initializable<ruvia::CorsOriginPolicy>);
    static_assert(!std::is_aggregate_v<ruvia::CorsOriginPolicy>);

    const auto any = ruvia::CorsOriginPolicy::any();
    const auto exact = ruvia::CorsOriginPolicy::exact(
        ruvia::CorsOrigin::serialized("https://app.example.com"));
    const auto credentialed =
        ruvia::CorsOriginPolicy::credentialed(
            ruvia::CorsOrigin::serialized("https://app.example.com"));
    RUVIA_CHECK(any.kind() == ruvia::CorsOriginPolicy::Kind::kAny);
    RUVIA_CHECK(exact.kind() == ruvia::CorsOriginPolicy::Kind::kExact);
    RUVIA_CHECK(
        credentialed.kind() ==
        ruvia::CorsOriginPolicy::Kind::kCredentialedExact);
}

RUVIA_TEST(cors_origin_requires_an_explicit_valid_wire_value) {
    const auto rejects = [](std::string_view value) {
        try {
            (void)ruvia::CorsOrigin::serialized(value);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    RUVIA_CHECK(rejects(""));
    RUVIA_CHECK(rejects("*"));
    RUVIA_CHECK(rejects("null"));
    RUVIA_CHECK(rejects("https://a\r\nX: y"));
    RUVIA_CHECK(rejects("https://app.example/"));
    RUVIA_CHECK(rejects("https://APP.example"));

    const auto opaque = ruvia::CorsOrigin::opaque();
    RUVIA_CHECK_EQ(opaque.value(), std::string_view("null"));
    const auto credentialedOpaque =
        ruvia::CorsOriginPolicy::credentialed(opaque);
    RUVIA_CHECK_EQ(
        credentialedOpaque.origin(),
        std::string_view("null"));
}

RUVIA_TEST(cors_header_names_validate_each_field_name_at_construction) {
    bool injectionThrew = false;
    bool invalidNameThrew = false;
    try {
        (void)ruvia::CorsHeaderNames::of({"X-Bar\r\nX: y"});
    } catch (const std::invalid_argument&) {
        injectionThrew = true;
    }
    try {
        (void)ruvia::CorsHeaderNames::of({"X Bad"});
    } catch (const std::invalid_argument&) {
        invalidNameThrew = true;
    }

    const auto valid = ruvia::CorsHeaderNames::of({"X-Exposed", "X-Other"});
    RUVIA_CHECK(injectionThrew);
    RUVIA_CHECK(invalidNameThrew);
    RUVIA_CHECK_EQ(valid.value(), std::string_view("X-Exposed, X-Other"));
}

RUVIA_TEST(cors_fixed_request_headers_reject_invalid_value_at_construction) {
    bool emptyThrew = false;
    bool injectionThrew = false;
    bool invalidNameThrew = false;
    try {
        (void)ruvia::CorsRequestHeadersPolicy::fixed(
            std::initializer_list<std::string_view>{});
    } catch (const std::invalid_argument&) {
        emptyThrew = true;
    }
    try {
        (void)ruvia::CorsRequestHeadersPolicy::fixed({"X-Foo\r\nX: y"});
    } catch (const std::invalid_argument&) {
        injectionThrew = true;
    }
    try {
        (void)ruvia::CorsRequestHeadersPolicy::fixed({"X Bad"});
    } catch (const std::invalid_argument&) {
        invalidNameThrew = true;
    }
    RUVIA_CHECK(emptyThrew);
    RUVIA_CHECK(injectionThrew);
    RUVIA_CHECK(invalidNameThrew);
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
        ? ruvia::CorsOriginPolicy::credentialed(
              ruvia::CorsOrigin::serialized(configuredOrigin))
        : ruvia::CorsOriginPolicy::exact(
              ruvia::CorsOrigin::serialized(configuredOrigin));
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

RUVIA_TEST(cors_runtime_sets_static_configured_origin) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
    HttpResponse response(std::pmr::new_delete_resource());
    applyCorsHeaders(result.request, response, corsOptions("https://app.example", false));

    // The configured origin is emitted verbatim -- the request Origin is never reflected.
    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Origin").value_or(""),
                   std::string_view("https://app.example"));
    // A configured origin is static across requests, so it does not vary by
    // the presence or value of Origin.
    RUVIA_CHECK(response.header("Vary").value_or("").find("Origin") == std::string_view::npos);
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

RUVIA_TEST(cors_static_response_metadata_is_cache_stable_without_origin) {
    // A shared cache can reuse this response for a later CORS request. Static
    // CORS metadata therefore cannot depend on whether Origin was present.
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        HttpResponse response(std::pmr::new_delete_resource());
        auto cors = corsOptions("*", false);
        cors.exposeHeaders = ruvia::CorsHeaderNames::of({"X-Total-Count"});
        applyCorsHeaders(result.request, response, cors);
        RUVIA_CHECK_EQ(
            response.header("Access-Control-Allow-Origin").value_or(""),
            std::string_view("*"));
        RUVIA_CHECK_EQ(
            response.header("Access-Control-Expose-Headers").value_or(""),
            std::string_view("X-Total-Count"));
        RUVIA_CHECK(!response.header("Vary").has_value());
    }
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        HttpResponse response(std::pmr::new_delete_resource());
        applyCorsHeaders(
            result.request,
            response,
            corsOptions("https://app.example", true));
        RUVIA_CHECK_EQ(
            response.header("Access-Control-Allow-Origin").value_or(""),
            std::string_view("https://app.example"));
        RUVIA_CHECK_EQ(
            response.header("Access-Control-Allow-Credentials").value_or(""),
            std::string_view("true"));
        RUVIA_CHECK(!response.header("Vary").has_value());
    }
}

RUVIA_TEST(cors_options_variants_declare_every_request_dependency) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\n"
        "Access-Control-Request-Method: POST\r\n\r\n");
    HttpResponse response(std::pmr::new_delete_resource());
    applyCorsHeaders(result.request, response, corsOptions("*", false));

    const auto vary = response.header("Vary").value_or("");
    RUVIA_CHECK(vary.find("Origin") != std::string_view::npos);
    RUVIA_CHECK(
        vary.find("Access-Control-Request-Method") != std::string_view::npos);
    RUVIA_CHECK(
        vary.find("Access-Control-Request-Headers") != std::string_view::npos);
    RUVIA_CHECK(!response.header("Access-Control-Allow-Methods").has_value());
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
        ruvia::CorsRequestHeadersPolicy::fixed(
            {"Authorization", "X-Configured"});
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
        cors.exposeHeaders =
            ruvia::CorsHeaderNames::of({"X-Total-Count", "X-Request-Id"});
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
        cors.exposeHeaders = ruvia::CorsHeaderNames::of({"X-Total-Count"});
        applyCorsHeaders(result.request, response, cors);
        RUVIA_CHECK(!response.header("Access-Control-Expose-Headers").has_value());
    }
}
