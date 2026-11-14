#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "http_cache.h"
#include "http_cors.h"
#include "http_header.h"
#include "http_proxy_forwarding.h"
#include "http_request.h"
#include "http_response.h"

TEST(RuviaHttpPublicApi, cors_policy_preflight_and_response_rewrite) {
    ruvia::http_cors_policy_config config;
    config.allowed_origins_.emplace_back("https://app.example.com");
    config.allowed_origin_suffixes_.emplace_back(".trusted.example");
    config.allowed_origin_suffixes_.emplace_back("https://partner.test:8443");
    config.allowed_methods_.emplace_back("GET");
    config.allowed_methods_.emplace_back("POST");
    config.allowed_headers_.emplace_back("X-Custom-Auth");
    config.allowed_headers_.emplace_back("Content-Type");
    config.expose_headers_.emplace_back("X-Request-Id");
    config.expose_headers_.emplace_back("X-RateLimit-Remaining");
    config.allow_credentials_ = true;
    config.max_age_ = std::chrono::seconds(600);

    const ruvia::http_cors_policy policy(config);
    EXPECT_TRUE(policy.allows_origin("https://app.example.com"));
    EXPECT_TRUE(policy.allows_origin("https://sub.trusted.example"));
    EXPECT_TRUE(policy.allows_origin("http://a.b.trusted.example:8080"));
    EXPECT_FALSE(policy.allows_origin("https://trusted.example"));
    EXPECT_FALSE(policy.allows_origin("https://evil-trusted.example"));
    EXPECT_TRUE(policy.allows_origin("https://partner.test:8443"));
    EXPECT_TRUE(policy.allows_origin("https://api.partner.test:8443"));
    EXPECT_FALSE(policy.allows_origin("http://partner.test:8443"));
    EXPECT_FALSE(policy.allows_origin("https://partner.test:9443"));

    const auto preflight_ok = policy.evaluate_preflight({
        .method_ = "OPTIONS",
        .origin_ = "https://sub.trusted.example",
        .access_control_request_method_ = "POST",
        .access_control_request_headers_ = "content-type, x-custom-auth",
    });
    EXPECT_TRUE(preflight_ok.accepted());

    const auto find_header = [](std::span<const ruvia::http_header> headers, std::string_view name) -> std::optional<std::string_view> {
        for (const auto& field : headers) {
            if (ruvia::http_ascii_equals_ignore_case(field.name(), name)) {
                return field.value();
            }
        }
        return std::nullopt;
    };

    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Origin"), std::optional<std::string_view>("https://sub.trusted.example"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Credentials"), std::optional<std::string_view>("true"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Methods"), std::optional<std::string_view>("GET, POST"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Allow-Headers"), std::optional<std::string_view>("X-Custom-Auth, Content-Type"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Access-Control-Max-Age"), std::optional<std::string_view>("600"));
    EXPECT_EQ(find_header(preflight_ok.headers(), "Vary"), std::optional<std::string_view>("Origin, Access-Control-Request-Method"));

    // Disallowed header or method in preflight is rejected.
    const auto preflight_bad_header = policy.evaluate_preflight({
        .method_ = "OPTIONS",
        .origin_ = "https://sub.trusted.example",
        .access_control_request_method_ = "POST",
        .access_control_request_headers_ = "X-Forbidden-Header",
    });
    EXPECT_FALSE(preflight_bad_header.accepted());
    EXPECT_TRUE(preflight_bad_header.headers().empty());

    // Actual response header rewrite: strips upstream Access-Control-* headers,
    // writes policy headers, and merges Origin into Vary without duplicates.
    std::vector<ruvia::http_header> response_headers;
    response_headers.emplace_back("Content-Type", "application/json");
    response_headers.emplace_back("Access-Control-Allow-Origin", "*");
    response_headers.emplace_back("Access-Control-Allow-Methods", "DELETE");
    response_headers.emplace_back("Vary", "Accept-Encoding, Origin");

    EXPECT_TRUE(policy.rewrite_response_headers("https://app.example.com", response_headers));
    EXPECT_EQ(find_header(response_headers, "Access-Control-Allow-Origin"), std::optional<std::string_view>("https://app.example.com"));
    EXPECT_EQ(find_header(response_headers, "Access-Control-Allow-Credentials"), std::optional<std::string_view>("true"));
    EXPECT_EQ(find_header(response_headers, "Access-Control-Expose-Headers"), std::optional<std::string_view>("X-Request-Id, X-RateLimit-Remaining"));
    EXPECT_FALSE(find_header(response_headers, "Access-Control-Allow-Methods").has_value());
    EXPECT_EQ(find_header(response_headers, "Vary"), std::optional<std::string_view>("Accept-Encoding, Origin"));

    // Server-side apply_response_headers emits Vary tokens before rejecting disallowed Origin/method/headers.
    {
        const std::array<ruvia::http_header_view, 1> bad_origin_headers{
            ruvia::http_header_view{"Origin", "https://untrusted.example"},
        };
        const auto get_bad_origin = ruvia::make_parsed_http_request("GET", "/resource", bad_origin_headers, {});
        ruvia::http_response get_resp;
        EXPECT_FALSE(policy.apply_response_headers(get_bad_origin, get_resp));
        EXPECT_FALSE(get_resp.header("Access-Control-Allow-Origin").has_value());
        EXPECT_EQ(get_resp.header("Vary"), std::optional<std::string_view>("Origin"));

        const std::array<ruvia::http_header_view, 3> bad_options_headers{
            ruvia::http_header_view{"Origin", "https://untrusted.example"},
            ruvia::http_header_view{"Access-Control-Request-Method", "DELETE"},
            ruvia::http_header_view{"Access-Control-Request-Headers", "X-Forbidden"},
        };
        const auto opts_bad = ruvia::make_parsed_http_request("OPTIONS", "/resource", bad_options_headers, {});
        ruvia::http_response opts_resp;
        EXPECT_FALSE(policy.apply_response_headers(opts_bad, opts_resp));
        EXPECT_FALSE(opts_resp.header("Access-Control-Allow-Origin").has_value());
        EXPECT_EQ(opts_resp.header("Vary"), std::optional<std::string_view>("Origin, Access-Control-Request-Method"));

        ruvia::http_cors_policy reflecting_policy(ruvia::http_cors_policy_config{
            .allowed_origins_ = {"https://app.example.com"},
            .reflect_request_headers_ = true,
        });
        ruvia::http_response reflect_opts_resp;
        EXPECT_FALSE(reflecting_policy.apply_response_headers(opts_bad, reflect_opts_resp));
        EXPECT_EQ(reflect_opts_resp.header("Vary"), std::optional<std::string_view>("Origin, Access-Control-Request-Method, Access-Control-Request-Headers"));
    }

    // Vary and Connection use non-quoted comma-token scanning (unclosed quote does not hide later tokens).
    {
        std::vector<ruvia::http_header> vary_list;
        vary_list.emplace_back("Vary", "\"unclosed, Origin");
        ruvia::merge_http_vary_token(vary_list, "Origin");
        EXPECT_EQ(vary_list.size(), 1U);
        EXPECT_EQ(vary_list[0].value(), "\"unclosed, Origin");

        const auto parsed_vary = ruvia::parse_http_vary("\"unclosed, Accept-Encoding, *");
        EXPECT_TRUE(parsed_vary.is_wildcard());

        const auto parsed_conn = ruvia::parse_http_connection_nominated_fields("\"unclosed, close, X-Custom-Hop");
        EXPECT_TRUE(parsed_conn.has_close());
        EXPECT_TRUE(parsed_conn.contains("x-custom-hop"));
    }
}

// Fetch forbids credentials with a wildcard origin, so a policy that would
// answer any origin cannot also allow credentials.
TEST(RuviaHttpPublicApi, cors_policy_rejects_credentials_with_any_origin) {
    EXPECT_THROW((void)ruvia::http_cors_policy(ruvia::http_cors_policy_config{.allow_credentials_ = true}), std::invalid_argument);
    EXPECT_THROW((void)ruvia::http_cors_policy(ruvia::http_cors_policy_config{.allowed_origins_ = {"*"}, .allow_credentials_ = true}), std::invalid_argument);

    const ruvia::http_cors_policy policy(ruvia::http_cors_policy_config{.allowed_origins_ = {"https://example.com"}, .allow_credentials_ = true});
    const std::array<ruvia::http_header_view, 1> headers{
        ruvia::http_header_view{"Origin", "https://example.com"},
    };
    const auto request = ruvia::make_parsed_http_request("GET", "/resource", headers, {});
    ruvia::http_response response;
    EXPECT_TRUE(policy.apply_response_headers(request, response));
    EXPECT_EQ(response.header("Access-Control-Allow-Origin"), std::optional<std::string_view>("https://example.com"));
    EXPECT_EQ(response.header("Access-Control-Allow-Credentials"), std::optional<std::string_view>("true"));
}
