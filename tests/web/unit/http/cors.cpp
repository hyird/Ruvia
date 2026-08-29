#include "test_harness.h"
#include "memory_resource_fixture.h"

#include <chrono>
#include <cstddef>
#include <concepts>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/http/HttpCors.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/App.h"
#include "ruvia/http/HttpResponse.h"

static_assert(std::is_aggregate_v<ruvia::CorsOriginConfig>);
static_assert(std::is_aggregate_v<ruvia::CorsRequestHeadersConfig>);
static_assert(std::is_aggregate_v<ruvia::CorsConfig>);

namespace {

using ruvia::CorsConfig;
using ruvia::HttpResponse;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::test::RejectingMemoryResource;

void applyCorsHeaders(
    const ruvia::HttpRequest& request, HttpResponse& response, const CorsConfig& config) {
    const auto options = ruvia::detail::makeCorsOptions(config, std::pmr::new_delete_resource());
    ruvia::detail::applyCorsHeaders(request, response, options);
}

CorsConfig corsOptions(std::string_view configuredOrigin, bool credentials) {
    CorsConfig cors;
    if (configuredOrigin == "*") {
        return cors;
    }
    cors.origin = {
        .mode =
            credentials ? ruvia::CorsOriginMode::kCredentialedExact : ruvia::CorsOriginMode::kExact,
        .value = std::string(configuredOrigin),
    };
    return cors;
}

}  // namespace

RUVIA_TEST(cors_config_validates_when_consumed) {
    const auto rejects = [](const ruvia::CorsConfig& config) {
        try {
            (void)ruvia::detail::makeCorsOptions(config, std::pmr::new_delete_resource());
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    RUVIA_CHECK(rejects({.origin = {.mode = ruvia::CorsOriginMode::kExact}}));
    RUVIA_CHECK(rejects(
        {.origin = {.mode = ruvia::CorsOriginMode::kExact, .value = "https://APP.example"}}));
    RUVIA_CHECK(rejects({.requestHeaders = {.mode = ruvia::CorsRequestHeadersMode::kFixed}}));
    RUVIA_CHECK(rejects({.exposeHeaders = {"X-Bad\r\nInjected: yes"}}));
    RUVIA_CHECK(rejects({.maxAge = std::chrono::seconds(-1)}));
    RUVIA_CHECK(
        !rejects({.origin = {.mode = ruvia::CorsOriginMode::kCredentialedExact, .value = "null"}}));
}

RUVIA_TEST(cors_rejects_the_entire_config_before_owner_allocation) {
    CorsConfig config{
        .origin =
            {
                .mode = ruvia::CorsOriginMode::kExact,
                .value = std::string("https://") + std::string(50, 'a') + ".example",
            },
        .requestHeaders = {.mode = ruvia::CorsRequestHeadersMode::kFixed},
    };
    RejectingMemoryResource resource;
    resource.rejectAllocations();

    bool rejectedAsConfig = false;
    try {
        (void)ruvia::detail::makeCorsOptions(config, &resource);
    } catch (const std::invalid_argument& error) {
        rejectedAsConfig =
            std::string_view(error.what()) == "CORS fixed request headers must not be empty";
    }

    RUVIA_CHECK(rejectedAsConfig);
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});
}

RUVIA_TEST(cors_max_age_distinguishes_absence_from_zero) {
    static_assert(
        std::same_as<decltype(ruvia::CorsConfig{}.maxAge), std::optional<std::chrono::seconds>>);

    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n\r\n");

    auto absent = corsOptions("https://app.example", false);
    HttpResponse absentResponse({.resource = std::pmr::new_delete_resource()});
    applyCorsHeaders(result.request, absentResponse, absent);
    RUVIA_CHECK(!absentResponse.header("Access-Control-Max-Age").has_value());

    auto zero = corsOptions("https://app.example", false);
    zero.maxAge.emplace(std::chrono::seconds(0));
    HttpResponse zeroResponse({.resource = std::pmr::new_delete_resource()});
    applyCorsHeaders(result.request, zeroResponse, zero);
    RUVIA_CHECK_EQ(
        zeroResponse.header("Access-Control-Max-Age").value_or(""), std::string_view("0"));
}

RUVIA_TEST(cors_runtime_sets_static_configured_origin) {
    Http1ServerRequestParser parser;
    const auto result =
        parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
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
    const auto result =
        parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://any.example\r\n\r\n");
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    applyCorsHeaders(result.request, response, corsOptions("*", false));

    RUVIA_CHECK_EQ(
        response.header("Access-Control-Allow-Origin").value_or(""), std::string_view("*"));
    RUVIA_CHECK(response.header("Vary").value_or("").find("Origin") == std::string_view::npos);
}

RUVIA_TEST(cors_runtime_credentials_belong_to_specific_origin) {
    {
        Http1ServerRequestParser parser;
        const auto result =
            parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
        HttpResponse response({.resource = std::pmr::new_delete_resource()});
        applyCorsHeaders(result.request, response, corsOptions("https://app.example", true));
        RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Credentials").value_or(""),
            std::string_view("true"));
    }
}

RUVIA_TEST(cors_static_response_metadata_is_cache_stable_without_origin) {
    // A shared cache can reuse this response for a later CORS request. Static
    // CORS metadata therefore cannot depend on whether Origin was present.
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        HttpResponse response({.resource = std::pmr::new_delete_resource()});
        auto cors = corsOptions("*", false);
        cors.exposeHeaders = {"X-Total-Count"};
        applyCorsHeaders(result.request, response, cors);
        RUVIA_CHECK_EQ(
            response.header("Access-Control-Allow-Origin").value_or(""), std::string_view("*"));
        RUVIA_CHECK_EQ(response.header("Access-Control-Expose-Headers").value_or(""),
            std::string_view("X-Total-Count"));
        RUVIA_CHECK(!response.header("Vary").has_value());
    }
    {
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        HttpResponse response({.resource = std::pmr::new_delete_resource()});
        applyCorsHeaders(result.request, response, corsOptions("https://app.example", true));
        RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Origin").value_or(""),
            std::string_view("https://app.example"));
        RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Credentials").value_or(""),
            std::string_view("true"));
        RUVIA_CHECK(!response.header("Vary").has_value());
    }
}

RUVIA_TEST(cors_options_variants_declare_every_request_dependency) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\n"
        "Access-Control-Request-Method: POST\r\n\r\n");
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    applyCorsHeaders(result.request, response, corsOptions("*", false));

    const auto vary = response.header("Vary").value_or("");
    RUVIA_CHECK(vary.find("Origin") != std::string_view::npos);
    RUVIA_CHECK(vary.find("Access-Control-Request-Method") != std::string_view::npos);
    RUVIA_CHECK(vary.find("Access-Control-Request-Headers") != std::string_view::npos);
    RUVIA_CHECK(!response.header("Access-Control-Allow-Methods").has_value());
}

RUVIA_TEST(cors_preflight_reflects_methods_and_requested_headers) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Headers: X-Custom\r\n\r\n");
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.header("Allow", "GET, POST, OPTIONS");  // the route-advertised methods

    auto cors = corsOptions("https://app.example", false);
    cors.maxAge.emplace(std::chrono::seconds(600));
    // Reflect policy forwards the request's Access-Control-Request-Headers value.
    applyCorsHeaders(result.request, response, cors);

    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Methods").value_or(""),
        std::string_view("GET, POST, OPTIONS"));
    RUVIA_CHECK_EQ(
        response.header("Access-Control-Allow-Headers").value_or(""), std::string_view("X-Custom"));
    RUVIA_CHECK_EQ(response.header("Access-Control-Max-Age").value_or(""), std::string_view("600"));
    RUVIA_CHECK(response.header("Vary").value_or("").find("Access-Control-Request-Method") !=
                std::string_view::npos);
    RUVIA_CHECK(response.header("Vary").value_or("").find("Access-Control-Request-Headers") !=
                std::string_view::npos);
}

RUVIA_TEST(cors_preflight_reflects_every_request_header_field_line) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Headers: , X-One,\r\n"
        "Access-Control-Request-Headers: X-Two, X-Three\r\n\r\n");
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    applyCorsHeaders(result.request, response, corsOptions("https://app.example", false));

    std::size_t reflectedLines = 0;
    for (const auto& header : response.headers()) {
        if (ruvia::detail::httpAsciiEqualsIgnoreCase(
                header.name(), "Access-Control-Allow-Headers")) {
            if (reflectedLines == 0) {
                RUVIA_CHECK_EQ(header.value(), std::string_view("X-One"));
            } else if (reflectedLines == 1) {
                RUVIA_CHECK_EQ(header.value(), std::string_view("X-Two"));
            } else if (reflectedLines == 2) {
                RUVIA_CHECK_EQ(header.value(), std::string_view("X-Three"));
            }
            ++reflectedLines;
        }
    }
    RUVIA_CHECK_EQ(reflectedLines, std::size_t{3});
}

RUVIA_TEST(cors_preflight_prefers_configured_allow_headers) {
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Headers: X-Requested\r\n\r\n");
    HttpResponse response({.resource = std::pmr::new_delete_resource()});

    auto cors = corsOptions("https://app.example", false);
    cors.requestHeaders = {
        .mode = ruvia::CorsRequestHeadersMode::kFixed,
        .names = {"Authorization", "X-Configured"},
    };
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
        const auto result =
            parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
        HttpResponse response({.resource = std::pmr::new_delete_resource()});
        auto cors = corsOptions("https://app.example", false);
        cors.exposeHeaders = {"X-Total-Count", "X-Request-Id"};
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
        HttpResponse response({.resource = std::pmr::new_delete_resource()});
        auto cors = corsOptions("https://app.example", false);
        cors.exposeHeaders = {"X-Total-Count"};
        applyCorsHeaders(result.request, response, cors);
        RUVIA_CHECK(!response.header("Access-Control-Expose-Headers").has_value());
    }
}
