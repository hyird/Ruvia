#include <chrono>
#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"

#include "http/http_cors.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::cors_config;
using ruvia::http1_server_request_parser;
using ruvia::http_response;
using ruvia::test::rejecting_memory_resource;

void apply_cors_headers(
    const ruvia::http_request& request, http_response& response, const cors_config& config) {
    const auto options = ruvia::detail::make_cors_options(config, std::pmr::new_delete_resource());
    ruvia::detail::apply_cors_headers(request, response, options);
}

cors_config cors_options(std::string_view configured_origin, bool credentials) {
    cors_config cors;
    if (configured_origin == "*") {
        return cors;
    }
    cors.origin_ = {
        .mode_ =
            credentials ? ruvia::cors_origin_mode::credentialed_exact : ruvia::cors_origin_mode::exact,
        .value_ = std::string(configured_origin),
    };
    return cors;
}

}  // namespace

RUVIA_TEST(cors_config_validates_when_consumed) {
    const auto rejects = [](const ruvia::cors_config& config) {
        try {
            (void)ruvia::detail::make_cors_options(config, std::pmr::new_delete_resource());
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    RUVIA_CHECK(rejects({.origin_ = {.mode_ = ruvia::cors_origin_mode::exact}}));
    RUVIA_CHECK(rejects(
        {.origin_ = {.mode_ = ruvia::cors_origin_mode::exact, .value_ = "https://APP.example"}}));
    RUVIA_CHECK(rejects({.request_headers_ = {.mode_ = ruvia::cors_request_headers_mode::fixed}}));
    RUVIA_CHECK(rejects({.expose_headers_ = {"X-Bad\r\nInjected: yes"}}));
    RUVIA_CHECK(rejects({.max_age_ = std::chrono::seconds(-1)}));
    RUVIA_CHECK(
        !rejects({.origin_ = {.mode_ = ruvia::cors_origin_mode::credentialed_exact, .value_ = "null"}}));
}

RUVIA_TEST(cors_rejects_the_entire_config_before_owner_allocation) {
    cors_config config{
        .origin_ =
            {
                .mode_ = ruvia::cors_origin_mode::exact,
                .value_ = std::string("https://") + std::string(50, 'a') + ".example",
            },
        .request_headers_ = {.mode_ = ruvia::cors_request_headers_mode::fixed},
    };
    rejecting_memory_resource resource;
    resource.reject_allocations();

    bool rejected_as_config = false;
    try {
        (void)ruvia::detail::make_cors_options(config, &resource);
    } catch (const std::invalid_argument& error) {
        rejected_as_config =
            std::string_view(error.what()) == "CORS fixed request headers must not be empty";
    }

    RUVIA_CHECK(rejected_as_config);
    RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
}

RUVIA_TEST(cors_max_age_distinguishes_absence_from_zero) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n\r\n");

    auto absent = cors_options("https://app.example", false);
    http_response absent_response({.resource_ = std::pmr::new_delete_resource()});
    apply_cors_headers(result_value.request_, absent_response, absent);
    RUVIA_CHECK(!absent_response.header("Access-Control-Max-Age").has_value());

    auto zero = cors_options("https://app.example", false);
    zero.max_age_.emplace(std::chrono::seconds(0));
    http_response zero_response({.resource_ = std::pmr::new_delete_resource()});
    apply_cors_headers(result_value.request_, zero_response, zero);
    RUVIA_CHECK_EQ(
        zero_response.header("Access-Control-Max-Age").value_or(""), std::string_view("0"));
}

RUVIA_TEST(cors_runtime_sets_static_configured_origin) {
    http1_server_request_parser parser;
    const auto result_value =
        parser.parse_message("GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    apply_cors_headers(result_value.request_, response, cors_options("https://app.example", false));

    // The configured origin is emitted verbatim -- the request Origin is never reflected.
    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Origin").value_or(""),
        std::string_view("https://app.example"));
    // A configured origin is static across requests, so it does not vary by
    // the presence or value of Origin.
    RUVIA_CHECK(!(response.header("Vary").value_or("").find("Origin") != std::string_view::npos));
    RUVIA_CHECK(!response.header("Access-Control-Allow-Credentials").has_value());
}

RUVIA_TEST(cors_runtime_wildcard_has_no_vary_origin) {
    http1_server_request_parser parser;
    const auto result_value =
        parser.parse_message("GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://any.example\r\n\r\n");
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    apply_cors_headers(result_value.request_, response, cors_options("*", false));

    RUVIA_CHECK_EQ(
        response.header("Access-Control-Allow-Origin").value_or(""), std::string_view("*"));
    RUVIA_CHECK(!(response.header("Vary").value_or("").find("Origin") != std::string_view::npos));
}

RUVIA_TEST(cors_runtime_credentials_belong_to_specific_origin) {
    {
        http1_server_request_parser parser;
        const auto result_value =
            parser.parse_message("GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
        http_response response({.resource_ = std::pmr::new_delete_resource()});
        apply_cors_headers(result_value.request_, response, cors_options("https://app.example", true));
        RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Credentials").value_or(""),
            std::string_view("true"));
    }
}

RUVIA_TEST(cors_static_response_metadata_is_cache_stable_without_origin) {
    // A shared cache can reuse this response for a later CORS request. Static
    // CORS metadata therefore cannot depend on whether Origin was present.
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        http_response response({.resource_ = std::pmr::new_delete_resource()});
        auto cors = cors_options("*", false);
        cors.expose_headers_ = {"X-Total-Count"};
        apply_cors_headers(result_value.request_, response, cors);
        RUVIA_CHECK_EQ(
            response.header("Access-Control-Allow-Origin").value_or(""), std::string_view("*"));
        RUVIA_CHECK_EQ(response.header("Access-Control-Expose-Headers").value_or(""),
            std::string_view("X-Total-Count"));
        RUVIA_CHECK(!response.header("Vary").has_value());
    }
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        http_response response({.resource_ = std::pmr::new_delete_resource()});
        apply_cors_headers(result_value.request_, response, cors_options("https://app.example", true));
        RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Origin").value_or(""),
            std::string_view("https://app.example"));
        RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Credentials").value_or(""),
            std::string_view("true"));
        RUVIA_CHECK(!response.header("Vary").has_value());
    }
}

RUVIA_TEST(cors_options_variants_declare_every_request_dependency) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\n"
        "Access-Control-Request-Method: POST\r\n\r\n");
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    apply_cors_headers(result_value.request_, response, cors_options("*", false));

    const auto vary = response.header("Vary").value_or("");
    RUVIA_CHECK((vary.find("Origin") != std::string_view::npos));
    RUVIA_CHECK((vary.find("Access-Control-Request-Method") != std::string_view::npos));
    RUVIA_CHECK((vary.find("Access-Control-Request-Headers") != std::string_view::npos));
    RUVIA_CHECK(!response.header("Access-Control-Allow-Methods").has_value());
}

RUVIA_TEST(cors_preflight_reflects_methods_and_requested_headers) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Headers: X-Custom\r\n\r\n");
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    response.header("Allow", "GET, POST, OPTIONS");  // the route-advertised methods

    auto cors = cors_options("https://app.example", false);
    cors.max_age_.emplace(std::chrono::seconds(600));
    // Reflect policy forwards the request's Access-Control-Request-Headers value.
    apply_cors_headers(result_value.request_, response, cors);

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
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Headers: , X-One,\r\n"
        "Access-Control-Request-Headers: X-Two, X-Three\r\n\r\n");
    http_response response({.resource_ = std::pmr::new_delete_resource()});
    apply_cors_headers(result_value.request_, response, cors_options("https://app.example", false));

    std::size_t reflected_lines = 0;
    for (const auto& header : response.headers()) {
        if (ruvia::detail::http_ascii_equals_ignore_case(
                header.name(), "Access-Control-Allow-Headers")) {
            if (reflected_lines == 0) {
                RUVIA_CHECK_EQ(header.value(), std::string_view("X-One"));
            } else if (reflected_lines == 1) {
                RUVIA_CHECK_EQ(header.value(), std::string_view("X-Two"));
            } else if (reflected_lines == 2) {
                RUVIA_CHECK_EQ(header.value(), std::string_view("X-Three"));
            }
            ++reflected_lines;
        }
    }
    RUVIA_CHECK_EQ(reflected_lines, std::size_t{3});
}

RUVIA_TEST(cors_preflight_prefers_configured_allow_headers) {
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(
        "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Headers: X-Requested\r\n\r\n");
    http_response response({.resource_ = std::pmr::new_delete_resource()});

    auto cors = cors_options("https://app.example", false);
    cors.request_headers_ = {
        .mode_ = ruvia::cors_request_headers_mode::fixed,
        .names_ = {"Authorization", "X-Configured"},
    };
    apply_cors_headers(result_value.request_, response, cors);

    // The configured allow-list wins over reflecting the requested headers.
    RUVIA_CHECK_EQ(response.header("Access-Control-Allow-Headers").value_or(""),
        std::string_view("Authorization, X-Configured"));
}

RUVIA_TEST(cors_runtime_exposes_configured_headers_on_simple_response) {
    // A simple (non-preflight) response advertises which response headers
    // cross-origin script may read, via Access-Control-Expose-Headers. This
    // runtime branch had no coverage -- only the config validation of
    // expose_headers_ did -- so a regression dropping it would silently break
    // cross-origin header access.
    {
        http1_server_request_parser parser;
        const auto result_value =
            parser.parse_message("GET / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
        http_response response({.resource_ = std::pmr::new_delete_resource()});
        auto cors = cors_options("https://app.example", false);
        cors.expose_headers_ = {"X-Total-Count", "X-Request-Id"};
        apply_cors_headers(result_value.request_, response, cors);
        RUVIA_CHECK_EQ(response.header("Access-Control-Expose-Headers").value_or(""),
            std::string_view("X-Total-Count, X-Request-Id"));
    }
    // Expose-Headers is meaningless on a preflight response and must NOT be
    // emitted there: the preflight path returns before the expose-headers branch.
    {
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(
            "OPTIONS / HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
            "Access-Control-Request-Method: POST\r\n\r\n");
        http_response response({.resource_ = std::pmr::new_delete_resource()});
        auto cors = cors_options("https://app.example", false);
        cors.expose_headers_ = {"X-Total-Count"};
        apply_cors_headers(result_value.request_, response, cors);
        RUVIA_CHECK(!response.header("Access-Control-Expose-Headers").has_value());
    }
}
