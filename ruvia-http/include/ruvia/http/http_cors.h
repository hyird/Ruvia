#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

namespace ruvia {

struct http_cors_policy_config final {
    bool allow_any_origin_{false};
    // Exact WHATWG serialized origins ("https://example.com", "null"), "*", or
    // suffix domain patterns starting with "." or "*." (e.g. ".example.com").
    std::vector<std::string> allowed_origins_;
    // Domain suffixes matched on a label boundary (".example.com", "example.com",
    // "*.example.com", or scheme-qualified "https://.example.com").
    std::vector<std::string> allowed_origin_suffixes_;
    // Allowed preflight methods (e.g. "GET", "POST", "OPTIONS"), or "*" for any
    // valid method token.
    std::vector<std::string> allowed_methods_;
    // Allowed request header names, or "*" to reflect Access-Control-Request-Headers.
    std::vector<std::string> allowed_headers_;
    bool reflect_request_headers_{false};
    std::vector<std::string> expose_headers_;
    bool allow_credentials_{false};
    std::optional<std::chrono::seconds> max_age_{};
};

struct http_cors_preflight_input final {
    borrowed_text method_{"OPTIONS"};
    borrowed_text origin_{};
    borrowed_text access_control_request_method_{};
    borrowed_text access_control_request_headers_{};
};

class http_cors_preflight_result final {
public:
    http_cors_preflight_result() noexcept = default;

    http_cors_preflight_result(bool accepted, std::vector<http_header> headers) noexcept
        : headers_(std::move(headers)),
          accepted_(accepted) {}

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return accepted_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return accepted_;
    }

    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_;
    }
    std::span<const http_header> headers() const&& = delete;

    [[nodiscard]] std::vector<http_header> take_headers() && noexcept {
        accepted_ = false;
        return std::move(headers_);
    }

private:
    std::vector<http_header> headers_;
    bool accepted_{false};
};

// Stateless, pre-validated CORS policy primitive for per-request preflight
// evaluation and actual response header rewriting.
class http_cors_policy final {
public:
    explicit http_cors_policy(const http_cors_policy_config& config);

    [[nodiscard]] bool allow_any_origin() const noexcept {
        return allow_any_origin_;
    }
    [[nodiscard]] bool allow_credentials() const noexcept {
        return allow_credentials_;
    }
    [[nodiscard]] bool reflect_request_headers() const noexcept {
        return reflect_request_headers_;
    }
    [[nodiscard]] bool allows_origin(std::string_view origin) const noexcept;
    [[nodiscard]] bool allows_method(std::string_view method) const noexcept;
    [[nodiscard]] bool allows_request_header(std::string_view name) const noexcept;

    [[nodiscard]] http_cors_preflight_result evaluate_preflight(
        http_cors_preflight_input input) const;
    [[nodiscard]] http_cors_preflight_result evaluate_preflight(
        const http_request& request) const;

    // Removes controlled Access-Control-* headers from the existing response,
    // writes computed CORS headers when the origin is permitted, and merges
    // Vary tokens (such as Origin) without duplicates.
    bool rewrite_response_headers(
        borrowed_text request_origin, std::vector<http_header>& response_headers) const;
    bool rewrite_response_headers(
        borrowed_text request_origin, http_response_headers& response_headers) const;
    bool rewrite_response_headers(
        borrowed_text request_origin, http_response& response) const;
    // Applies server-side CORS headers (including OPTIONS preflight and Allow
    // promotion) without overwriting headers already set by the route handler.
    bool apply_response_headers(const http_request& request, http_response& response) const;

private:
    struct suffix_rule final {
        std::string scheme_;
        std::string host_suffix_;
        std::optional<std::uint16_t> port_{};
        bool subdomain_only_{false};
    };

    std::vector<std::string> exact_origins_;
    std::vector<suffix_rule> suffix_rules_;
    std::vector<std::string> allowed_methods_;
    std::string joined_allowed_methods_;
    std::vector<std::string> allowed_headers_;
    std::string joined_allowed_headers_;
    std::string joined_expose_headers_;
    std::string max_age_;
    bool allow_any_origin_{false};
    bool allow_any_method_{false};
    bool reflect_request_headers_{false};
    bool allow_credentials_{false};
};

[[nodiscard]] bool is_http_cors_controlled_response_header(std::string_view name) noexcept;

void merge_http_vary_tokens(
    std::vector<http_header>& headers, std::span<const std::string_view> tokens);
void merge_http_vary_token(std::vector<http_header>& headers, std::string_view token);

}  // namespace ruvia
