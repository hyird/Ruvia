#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

namespace ruvia {

struct http_cors_policy_config final {
    explicit http_cors_policy_config(std::pmr::memory_resource* resource = nullptr)
        : allowed_origins_(detail::http_pmr_resource_or_default(resource)),
          allowed_origin_suffixes_(detail::http_pmr_resource_or_default(resource)),
          allowed_methods_(detail::http_pmr_resource_or_default(resource)),
          allowed_headers_(detail::http_pmr_resource_or_default(resource)),
          expose_headers_(detail::http_pmr_resource_or_default(resource)) {}

    bool allow_any_origin_{false};
    // Exact WHATWG serialized origins ("https://example.com", "null"), "*", or
    // suffix domain patterns starting with "." or "*." (e.g. ".example.com").
    std::pmr::vector<std::pmr::string> allowed_origins_;
    // Domain suffixes matched on a label boundary (".example.com", "example.com",
    // "*.example.com", or scheme-qualified "https://.example.com").
    std::pmr::vector<std::pmr::string> allowed_origin_suffixes_;
    // Allowed preflight methods (e.g. "GET", "POST", "OPTIONS"), or "*" for any
    // valid method token.
    std::pmr::vector<std::pmr::string> allowed_methods_;
    // Allowed request header names, or "*" to reflect Access-Control-Request-Headers.
    std::pmr::vector<std::pmr::string> allowed_headers_;
    bool reflect_request_headers_{false};
    std::pmr::vector<std::pmr::string> expose_headers_;
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
    explicit http_cors_preflight_result(std::pmr::memory_resource* resource = nullptr)
        : headers_(detail::http_pmr_resource_or_default(resource)) {}

    http_cors_preflight_result(bool accepted, std::pmr::vector<http_header> headers) noexcept
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

    [[nodiscard]] std::pmr::vector<http_header> take_headers() && noexcept {
        accepted_ = false;
        return std::move(headers_);
    }

private:
    std::pmr::vector<http_header> headers_;
    bool accepted_{false};
};

// Stateless, pre-validated CORS policy primitive for per-request preflight
// evaluation and actual response header rewriting.
class http_cors_policy final {
public:
    explicit http_cors_policy(const http_cors_policy_config& config,
        std::pmr::memory_resource* resource = nullptr);

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
        http_cors_preflight_input input, std::pmr::memory_resource* resource = nullptr) const;
    [[nodiscard]] http_cors_preflight_result evaluate_preflight(
        const http_request& request, std::pmr::memory_resource* resource = nullptr) const;

    // Removes controlled Access-Control-* headers from the existing response,
    // writes computed CORS headers when the origin is permitted, and merges
    // Vary tokens (such as Origin) without duplicates.
    bool rewrite_response_headers(
        borrowed_text request_origin, std::pmr::vector<http_header>& response_headers) const;
    bool rewrite_response_headers(
        borrowed_text request_origin, http_response_headers& response_headers) const;
    bool rewrite_response_headers(
        borrowed_text request_origin, http_response& response) const;

private:
    struct suffix_rule final {
        explicit suffix_rule(std::pmr::memory_resource* resource)
            : scheme_(resource),
              host_suffix_(resource) {}

        std::pmr::string scheme_;
        std::pmr::string host_suffix_;
        std::optional<std::uint16_t> port_{};
        bool subdomain_only_{false};
    };

    std::pmr::memory_resource* resource_;
    std::pmr::vector<std::pmr::string> exact_origins_;
    std::pmr::vector<suffix_rule> suffix_rules_;
    std::pmr::vector<std::pmr::string> allowed_methods_;
    std::pmr::string joined_allowed_methods_;
    std::pmr::vector<std::pmr::string> allowed_headers_;
    std::pmr::string joined_allowed_headers_;
    std::pmr::string joined_expose_headers_;
    std::pmr::string max_age_;
    bool allow_any_origin_{false};
    bool allow_any_method_{false};
    bool reflect_request_headers_{false};
    bool allow_credentials_{false};
};

[[nodiscard]] bool is_http_cors_controlled_response_header(std::string_view name) noexcept;

void merge_http_vary_tokens(
    std::pmr::vector<http_header>& headers, std::span<const std::string_view> tokens);
void merge_http_vary_token(std::pmr::vector<http_header>& headers, std::string_view token);

[[nodiscard]] inline http_cors_preflight_result evaluate_http_cors_preflight(
    const http_cors_policy& policy, http_cors_preflight_input input,
    std::pmr::memory_resource* resource = nullptr) {
    return policy.evaluate_preflight(input, resource);
}

[[nodiscard]] inline http_cors_preflight_result evaluate_http_cors_preflight(
    const http_cors_policy& policy, const http_request& request,
    std::pmr::memory_resource* resource = nullptr) {
    return policy.evaluate_preflight(request, resource);
}

inline bool rewrite_http_cors_response_headers(const http_cors_policy& policy,
    borrowed_text request_origin, std::pmr::vector<http_header>& response_headers) {
    return policy.rewrite_response_headers(request_origin, response_headers);
}

inline bool rewrite_http_cors_response_headers(const http_cors_policy& policy,
    borrowed_text request_origin, http_response_headers& response_headers) {
    return policy.rewrite_response_headers(request_origin, response_headers);
}

inline bool rewrite_http_cors_response_headers(const http_cors_policy& policy,
    borrowed_text request_origin, http_response& response) {
    return policy.rewrite_response_headers(request_origin, response);
}

}  // namespace ruvia
