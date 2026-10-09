#include "ruvia/web/security_headers.h"

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ruvia/web/context.h"

#include "context_request_fixture.h"
#include "test_harness.h"

namespace security_header_test {

using ruvia::apply_security_headers;
using ruvia::default_security_header_policy;
using ruvia::security_header_conflict_policy;
using ruvia::security_headers_config;
using ruvia::xss_protection_header_policy;

[[nodiscard]] ruvia::test_response security_response(
    const security_headers_config& options = {}, std::string_view existing_frame_options = {}) {
    static_cast<void>(ruvia::security_headers_middleware(options));
    return context_request_test::with_context(
        ruvia::test_request::get("/"),
        [&](ruvia::context& ctx) -> ruvia::task<void> {
            if (!existing_frame_options.empty()) {
                ctx.header("X-Frame-Options", existing_frame_options);
            }
            apply_security_headers(ctx, options);
            ctx.respond(ctx.text("ok"));
            co_return;
        });
}

}  // namespace security_header_test

using ruvia::default_security_header_policy;
using ruvia::security_header_conflict_policy;
using ruvia::security_headers_config;
using ruvia::xss_protection_header_policy;
using security_header_test::security_response;

RUVIA_TEST(security_headers_default_set) {
    const auto response = security_response();

    RUVIA_CHECK_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
    RUVIA_CHECK_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
    RUVIA_CHECK(!response.header("Strict-Transport-Security").has_value());
    // Modern guidance disables the legacy XSS auditor rather than enabling it.
    RUVIA_CHECK_EQ(response.header("X-XSS-Protection"), std::string_view("0"));
    // Secure-by-default policy values ship out of the box.
    RUVIA_CHECK_EQ(
        response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    RUVIA_CHECK_EQ(
        response.header("Referrer-Policy"), std::string_view("strict-origin-when-cross-origin"));
    RUVIA_CHECK_EQ(response.header("Permissions-Policy"),
        std::string_view("geolocation=(), microphone=(), camera=()"));
}

RUVIA_TEST(security_headers_xss_protection_header_policy_is_explicit) {
    const security_headers_config options{
        .xss_protection_header_ = xss_protection_header_policy::omit,
    };
    const auto response = security_response(options);
    RUVIA_CHECK(!response.header("X-XSS-Protection").has_value());
}

RUVIA_TEST(security_headers_reject_invalid_xss_protection_header_policy) {
    const security_headers_config options{
        .xss_protection_header_ = std::bit_cast<xss_protection_header_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        static_cast<void>(security_response(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(security_headers_reject_invalid_default_header_policy) {
    const security_headers_config options{
        .frame_options_header_ = std::bit_cast<default_security_header_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        static_cast<void>(security_response(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(security_headers_empty_policy_is_not_emitted) {
    // An explicitly-cleared policy string produces no header (rather than an
    // empty-valued one).
    security_headers_config options;
    options.content_security_policy_ = "";
    options.referrer_policy_ = "";
    const auto response = security_response(options);
    RUVIA_CHECK(!response.header("Content-Security-Policy").has_value());
    RUVIA_CHECK(!response.header("Referrer-Policy").has_value());
}

RUVIA_TEST(security_headers_disabled_options_omit_headers) {
    security_headers_config options;
    options.frame_options_header_ = default_security_header_policy::omit;
    options.strict_transport_security_header_ = default_security_header_policy::omit;
    const auto response = security_response(options);

    RUVIA_CHECK(!response.header("X-Frame-Options").has_value());
    RUVIA_CHECK(!response.header("Strict-Transport-Security").has_value());
    RUVIA_CHECK_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
}

RUVIA_TEST(security_headers_emit_configured_policies) {
    security_headers_config options;
    options.content_security_policy_ = "default-src 'self'";
    options.referrer_policy_ = "no-referrer";
    const auto response = security_response(options);

    RUVIA_CHECK_EQ(
        response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    RUVIA_CHECK_EQ(response.header("Referrer-Policy"), std::string_view("no-referrer"));
}

RUVIA_TEST(security_headers_apply_custom_headers) {
    const std::vector<ruvia::security_header> custom_value = {
        {"X-Custom-Security", "value-1"},
        {"X-Report-To", "endpoint"},
    };
    security_headers_config options;
    options.custom_headers_ = custom_value;
    const auto response = security_response(options);

    RUVIA_CHECK_EQ(response.header("X-Custom-Security"), std::string_view("value-1"));
    RUVIA_CHECK_EQ(response.header("X-Report-To"), std::string_view("endpoint"));
    // Built-in defaults are still applied alongside custom headers.
    RUVIA_CHECK_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
}

RUVIA_TEST(security_headers_reject_invalid_custom_header_at_construction) {
    bool invalid_name_rejected = false;
    try {
        static_cast<void>(ruvia::security_headers_middleware(security_headers_config{
            .custom_headers_ = {{"bad name", "value"}},
        }));
    } catch (const std::invalid_argument&) {
        invalid_name_rejected = true;
    }
    RUVIA_CHECK(invalid_name_rejected);

    bool invalid_value_rejected = false;
    try {
        static_cast<void>(ruvia::security_headers_middleware(security_headers_config{
            .custom_headers_ = {{"X-Security", "bad\r\nvalue"}},
        }));
    } catch (const std::invalid_argument&) {
        invalid_value_rejected = true;
    }
    RUVIA_CHECK(invalid_value_rejected);
}

RUVIA_TEST(security_headers_respect_existing_header_conflict_policy) {
    // The default preserves a header a handler already set -- the middleware
    // only supplies defaults.
    const auto keep = security_response({}, "SAMEORIGIN");
    RUVIA_CHECK_EQ(keep.header("X-Frame-Options"), std::string_view("SAMEORIGIN"));

    const auto replace = security_response(
        security_headers_config{
            .existing_headers_ = security_header_conflict_policy::replace_existing,
        },
        "SAMEORIGIN");
    RUVIA_CHECK_EQ(replace.header("X-Frame-Options"), std::string_view("DENY"));
}

RUVIA_TEST(security_headers_reject_invalid_conflict_policy) {
    const security_headers_config options{
        .existing_headers_ = std::bit_cast<security_header_conflict_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        static_cast<void>(security_response(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
