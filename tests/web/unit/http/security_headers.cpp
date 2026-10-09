#include "ruvia/web/security_headers.h"

#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"

#include "context/context_access.h"
#include "context/context_services.h"
#include "context_services_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::apply_security_headers;
using ruvia::context;
using ruvia::default_security_header_policy;
using ruvia::http_response;
using ruvia::request_memory;
using ruvia::security_header;
using ruvia::security_header_conflict_policy;
using ruvia::security_headers_config;
using ruvia::worker_memory;
using ruvia::xss_protection_header_policy;
using ruvia::detail::context_access;
using ruvia::detail::context_services;

class security_context_fixture final {
public:
    security_context_fixture()
        : security_context_fixture(ruvia::test::test_context_services()) {}

    explicit security_context_fixture(context_services services)
        : memory_(worker_),
          request_(make_request(memory_)),
          context_(context_access::make(memory_, request_, services)) {}

    [[nodiscard]] ruvia::context& context() noexcept {
        return context_;
    }

    [[nodiscard]] http_response& response() {
        return context_access::response_storage(context_);
    }

private:
    [[nodiscard]] static ruvia::http_request make_request(request_memory& memory) {
        auto [request, error] = ruvia::make_parsed_http_request("GET", "/", {}, {}, memory.resource());
        if (error) {
            throw std::runtime_error("invalid test request");
        }
        return std::move(request);
    }

    worker_memory worker_;
    request_memory memory_;
    ruvia::http_request request_;
    ruvia::context context_;
};

}  // namespace

RUVIA_TEST(security_headers_default_set) {
    security_context_fixture fixture_value(
        ruvia::test::test_context_services().with_tls_transport("192.0.2.1"));
    apply_security_headers(fixture_value.context(), security_headers_config{});
    const auto& response = fixture_value.response();

    RUVIA_CHECK_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
    RUVIA_CHECK_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
    RUVIA_CHECK_EQ(response.header("Strict-Transport-Security"),
        std::string_view("max-age=31536000; includeSubDomains"));
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

RUVIA_TEST(security_headers_emit_hsts_only_for_tls_contexts) {
    security_context_fixture plain(
        ruvia::test::test_context_services().with_plain_transport("192.0.2.1"));
    apply_security_headers(plain.context());
    RUVIA_CHECK(!plain.response().header("Strict-Transport-Security").has_value());

    security_context_fixture tls(ruvia::test::test_context_services().with_tls_transport("192.0.2.2"));
    apply_security_headers(tls.context());
    RUVIA_CHECK_EQ(tls.response().header("Strict-Transport-Security"),
        std::string_view("max-age=31536000; includeSubDomains"));
}

RUVIA_TEST(security_headers_xss_protection_header_policy_is_explicit) {
    security_context_fixture fixture;
    const security_headers_config options{
        .xss_protection_header_ = xss_protection_header_policy::omit,
    };
    apply_security_headers(fixture.context(), options);

    RUVIA_CHECK(!fixture.response().header("X-XSS-Protection").has_value());
}

RUVIA_TEST(security_headers_reject_invalid_xss_protection_header_policy) {
    security_context_fixture fixture;
    const security_headers_config options{
        .xss_protection_header_ = std::bit_cast<xss_protection_header_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        apply_security_headers(fixture.context(), options);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(security_headers_reject_invalid_default_header_policy) {
    security_context_fixture fixture;
    const security_headers_config options{
        .frame_options_header_ = std::bit_cast<default_security_header_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        apply_security_headers(fixture.context(), options);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(security_headers_empty_policy_is_not_emitted) {
    // An explicitly-cleared policy string produces no header (rather than an
    // empty-valued one).
    security_context_fixture fixture;
    security_headers_config options;
    options.content_security_policy_ = "";
    options.referrer_policy_ = "";
    apply_security_headers(fixture.context(), options);
    const auto& response = fixture.response();
    RUVIA_CHECK(!response.header("Content-Security-Policy").has_value());
    RUVIA_CHECK(!response.header("Referrer-Policy").has_value());
}

RUVIA_TEST(security_headers_disabled_options_omit_headers) {
    security_context_fixture fixture;
    security_headers_config options;
    options.frame_options_header_ = default_security_header_policy::omit;
    options.strict_transport_security_header_ = default_security_header_policy::omit;
    apply_security_headers(fixture.context(), options);
    const auto& response = fixture.response();

    RUVIA_CHECK(!response.header("X-Frame-Options").has_value());
    RUVIA_CHECK(!response.header("Strict-Transport-Security").has_value());
    RUVIA_CHECK_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
}

RUVIA_TEST(security_headers_emit_configured_policies) {
    security_context_fixture fixture;
    security_headers_config options;
    options.content_security_policy_ = "default-src 'self'";
    options.referrer_policy_ = "no-referrer";
    apply_security_headers(fixture.context(), options);
    const auto& response = fixture.response();

    RUVIA_CHECK_EQ(
        response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    RUVIA_CHECK_EQ(response.header("Referrer-Policy"), std::string_view("no-referrer"));
}

RUVIA_TEST(security_headers_apply_custom_headers) {
    security_context_fixture fixture;
    const std::vector<ruvia::security_header> custom_value = {
        {"X-Custom-Security", "value-1"},
        {"X-Report-To", "endpoint"},
    };
    security_headers_config options;
    options.custom_headers_ = custom_value;
    apply_security_headers(fixture.context(), options);
    const auto& response = fixture.response();

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

RUVIA_TEST(security_headers_custom_hsts_is_still_tls_only) {
    const std::vector<ruvia::security_header> custom_value = {
        {"Strict-Transport-Security", "max-age=1"},
    };
    security_headers_config options;
    options.strict_transport_security_header_ = default_security_header_policy::omit;
    options.custom_headers_ = custom_value;

    security_context_fixture plain(
        ruvia::test::test_context_services().with_plain_transport("192.0.2.1"));
    apply_security_headers(plain.context(), options);
    RUVIA_CHECK(!plain.response().header("Strict-Transport-Security").has_value());

    security_context_fixture tls(ruvia::test::test_context_services().with_tls_transport("192.0.2.2"));
    apply_security_headers(tls.context(), options);
    RUVIA_CHECK_EQ(
        tls.response().header("Strict-Transport-Security"), std::string_view("max-age=1"));
}

RUVIA_TEST(security_headers_respect_existing_header_conflict_policy) {
    // The default preserves a header a handler already set -- the middleware
    // only supplies defaults.
    security_context_fixture keep;
    keep.context().header("X-Frame-Options", "SAMEORIGIN");
    apply_security_headers(keep.context(), security_headers_config{});
    RUVIA_CHECK_EQ(keep.response().header("X-Frame-Options"), std::string_view("SAMEORIGIN"));

    security_context_fixture replace;
    replace.context().header("X-Frame-Options", "SAMEORIGIN");
    apply_security_headers(
        replace.context(), security_headers_config{
                               .existing_headers_ = security_header_conflict_policy::replace_existing,
                           });
    RUVIA_CHECK_EQ(replace.response().header("X-Frame-Options"), std::string_view("DENY"));
}

RUVIA_TEST(security_headers_reject_invalid_conflict_policy) {
    security_context_fixture fixture;
    const security_headers_config options{
        .existing_headers_ = std::bit_cast<security_header_conflict_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        apply_security_headers(fixture.context(), options);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
