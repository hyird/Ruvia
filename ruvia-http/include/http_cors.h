#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "http_header.h"
#include "http_response.h"

namespace ruvia {

class http_request;

struct http_cors_policy_options final {
    bool allow_any_origin_{false};
    // Exact WHATWG serialized origins ("https://example.com", "null"), "*", or
    // suffix domain patterns starting with "." or "*." (e.g. ".example.com").
    std::vector<std::string> allowed_origins_;
    // Domain suffixes matched on a label boundary (".example.com", "example.com",
    // "*.example.com", or scheme-qualified "https://.example.com").
    std::vector<std::string> allowed_origin_suffixes_;
    // Allowed preflight methods (e.g. "GET", "POST", "OPTIONS"). A "*" entry
    // selects any valid method token instead of a fixed response list.
    std::vector<std::string> allowed_methods_;
    // Allowed request header names, or "*" to reflect Access-Control-Request-Headers.
    std::vector<std::string> allowed_headers_;
    bool reflect_request_headers_{false};
    std::vector<std::string> expose_headers_;
    // Requires explicit origins or suffixes; with any origin the policy
    // constructor throws std::invalid_argument.
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
    http_cors_preflight_result(const http_cors_preflight_result&) = default;
    http_cors_preflight_result& operator=(const http_cors_preflight_result&) = delete;
    http_cors_preflight_result(http_cors_preflight_result&&) noexcept = default;
    http_cors_preflight_result& operator=(http_cors_preflight_result&&) = delete;

    http_cors_preflight_result(bool accepted, std::vector<http_header> headers) noexcept :
        headers_(std::move(headers)),
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
    explicit http_cors_policy(const http_cors_policy_options& config);
    explicit http_cors_policy(http_cors_policy_options&& config);

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

    // Counts every requested field-name across all field lines, including
    // repeats. A wildcard request name is rejected, never reflected.
    [[nodiscard]] http_cors_preflight_result evaluate_preflight(http_cors_preflight_input input, std::size_t max_requested_headers = (std::numeric_limits<std::size_t>::max)()) const;
    [[nodiscard]] http_cors_preflight_result evaluate_preflight(borrowed_text method, const http_header_range_view& headers, std::size_t max_requested_headers = (std::numeric_limits<std::size_t>::max)()) const;
    [[nodiscard]] http_cors_preflight_result evaluate_preflight(const http_request& request, std::size_t max_requested_headers = (std::numeric_limits<std::size_t>::max)()) const;

    // Removes controlled Access-Control-* headers from the existing response,
    // writes computed CORS headers when the origin is permitted, and merges
    // Vary tokens (such as Origin) without duplicates.
    bool rewrite_response_headers(borrowed_text request_origin, editable_http_headers response_headers) const;
    // Applies server-side CORS headers (including OPTIONS preflight and Allow
    // promotion) without overwriting headers already set by the route handler.
    bool apply_response_headers(const http_request& request, http_response& response) const;

private:
    template <typename config_type>
    void initialize(config_type&& config);
    [[nodiscard]] bool collect_requested_headers(const http_header_range_view& fields, std::size_t maximum, std::string* reflected) const;

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

// Adds the Vary tokens the block does not list yet, leaving one combined Vary
// line; "*" replaces every Vary line.
void merge_http_vary_tokens(editable_http_headers headers, std::span<const std::string_view> tokens);
void merge_http_vary_token(editable_http_headers headers, std::string_view token);
}  // namespace ruvia
