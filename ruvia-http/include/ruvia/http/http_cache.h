#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_date.h"
#include "ruvia/http/http_header.h"

namespace ruvia {

class http_request;
class http_response;
class http_response_headers;

enum class cache_control_directive : std::uint16_t {
    no_store = 1U << 0,
    no_cache = 1U << 1,
    no_transform = 1U << 2,
    must_revalidate = 1U << 3,
    proxy_revalidate = 1U << 4,
    private_value = 1U << 5,
    public_value = 1U << 6,
    immutable = 1U << 7,
    only_if_cached = 1U << 8,
    max_stale_any = 1U << 9,
};

// Parsed HTTP Cache-Control directives (RFC 9111 section 5.2). Unknown directives are ignored.
// Boolean directives are queried by directive enum; delta-seconds directives are optional (absent = not present).
// This type reports wire directives only; cache freshness and reuse policy belong to the caller.
class cache_control final {
public:
    [[nodiscard]] constexpr bool has(cache_control_directive directive) const noexcept {
        const auto mask = static_cast<std::uint16_t>(directive);
        return mask != 0U && (mask & (mask - 1U)) == 0U && (directives_ & mask) != 0U;
    }

    [[nodiscard]] constexpr bool no_store() const noexcept {
        return has(cache_control_directive::no_store);
    }
    [[nodiscard]] constexpr bool no_cache() const noexcept {
        return has(cache_control_directive::no_cache);
    }
    [[nodiscard]] constexpr bool no_transform() const noexcept {
        return has(cache_control_directive::no_transform);
    }
    [[nodiscard]] constexpr bool must_revalidate() const noexcept {
        return has(cache_control_directive::must_revalidate);
    }
    [[nodiscard]] constexpr bool proxy_revalidate() const noexcept {
        return has(cache_control_directive::proxy_revalidate);
    }
    [[nodiscard]] constexpr bool is_private() const noexcept {
        return has(cache_control_directive::private_value);
    }
    [[nodiscard]] constexpr bool is_public() const noexcept {
        return has(cache_control_directive::public_value);
    }
    [[nodiscard]] constexpr bool immutable() const noexcept {
        return has(cache_control_directive::immutable);
    }
    [[nodiscard]] constexpr bool only_if_cached() const noexcept {
        return has(cache_control_directive::only_if_cached);
    }
    [[nodiscard]] constexpr bool max_stale_any() const noexcept {
        return has(cache_control_directive::max_stale_any);
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> max_age() const noexcept {
        return max_age_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> max_stale() const noexcept {
        return max_stale_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> min_fresh() const noexcept {
        return min_fresh_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> s_max_age() const noexcept {
        return s_max_age_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> s_maxage() const noexcept {
        return s_max_age_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_while_revalidate() const noexcept {
        return stale_while_revalidate_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_if_error() const noexcept {
        return stale_if_error_;
    }

private:
    friend class cache_control_field_parser;

    constexpr void set(cache_control_directive directive) noexcept {
        directives_ |= static_cast<std::uint16_t>(directive);
    }

    std::uint16_t directives_{0};
    std::optional<std::uint64_t> max_age_;
    std::optional<std::uint64_t> max_stale_;
    std::optional<std::uint64_t> min_fresh_;
    std::optional<std::uint64_t> s_max_age_;
    std::optional<std::uint64_t> stale_while_revalidate_;
    std::optional<std::uint64_t> stale_if_error_;
};

// Incrementally parses every Cache-Control field line as one logical directive
// list (RFC 9110 section 5.2). State spans updates so duplicate freshness
// directives keep the first occurrence even when they appear on different lines.
class cache_control_field_parser final {
public:
    void update(std::string_view field_value) noexcept;

    [[nodiscard]] cache_control finish() const noexcept {
        return value_;
    }

private:
    cache_control value_;
    bool max_age_seen_{false};
    bool max_stale_seen_{false};
    bool min_fresh_seen_{false};
    bool s_max_age_seen_{false};
    bool stale_while_revalidate_seen_{false};
    bool stale_if_error_seen_{false};
};

// Parse a Cache-Control field value (a single line, or several joined by commas).
[[nodiscard]] cache_control parse_cache_control(std::string_view value) noexcept;
[[nodiscard]] cache_control parse_cache_control(std::span<const http_header> headers) noexcept;
[[nodiscard]] cache_control parse_cache_control(std::span<const http_header_view> headers) noexcept;
[[nodiscard]] cache_control parse_cache_control(const http_request& request) noexcept;
[[nodiscard]] cache_control parse_cache_control(const http_response_headers& headers) noexcept;
[[nodiscard]] cache_control parse_cache_control(const http_response& response) noexcept;

[[nodiscard]] bool http_cache_control_has_no_transform(std::string_view field_value) noexcept;
[[nodiscard]] bool http_cache_control_has_no_transform(std::span<const http_header> headers) noexcept;
[[nodiscard]] bool http_cache_control_has_no_transform(
    std::span<const http_header_view> headers) noexcept;
[[nodiscard]] bool http_cache_control_has_no_transform(const http_request& request) noexcept;
[[nodiscard]] bool http_cache_control_has_no_transform(
    const http_response_headers& headers) noexcept;
[[nodiscard]] bool http_cache_control_has_no_transform(const http_response& response) noexcept;

struct http_cache_control_directive_view final {
    std::string_view raw_{};
    std::string_view name_{};
    std::optional<std::string_view> argument_{};
};

// Rewrites Cache-Control to preserve all directives except max-age and s-maxage
// while setting client max-age=<client_max_age>. Header-block overloads also
// clear Expires so the rewritten max-age governs client freshness.
[[nodiscard]] std::pmr::string rewrite_http_cache_control_max_age(
    std::string_view field_value, std::uint64_t client_max_age,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] std::pmr::string rewrite_http_cache_control_max_age(
    std::span<const http_header> headers, std::uint64_t client_max_age,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] std::pmr::string rewrite_http_cache_control_max_age(
    std::span<const http_header_view> headers, std::uint64_t client_max_age,
    std::pmr::memory_resource* resource = nullptr);
void rewrite_http_response_cache_max_age(
    std::pmr::vector<http_header>& response_headers, std::uint64_t client_max_age);
void rewrite_http_response_cache_max_age(
    http_response_headers& response_headers, std::uint64_t client_max_age);
void rewrite_http_response_cache_max_age(
    http_response& response, std::uint64_t client_max_age);

// Parsed multi-line Vary specification (RFC 9111 section 4.1). Field names are
// normalized to lowercase, deduplicated, and sorted in fixed ASCII order.
class http_vary_spec final {
public:
    explicit http_vary_spec(std::pmr::memory_resource* resource = nullptr)
        : field_names_(detail::http_pmr_resource_or_default(resource)) {}

    void update(std::string_view field_value);
    void update_headers(std::span<const http_header> headers);
    void update_headers(std::span<const http_header_view> headers);
    void update_headers(const http_response_headers& headers);

    [[nodiscard]] constexpr bool is_wildcard() const noexcept {
        return wildcard_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return !wildcard_ && field_names_.empty();
    }
    [[nodiscard]] std::span<const std::pmr::string> field_names() const& noexcept RUVIA_LIFETIMEBOUND {
        return field_names_;
    }
    std::span<const std::pmr::string> field_names() const&& = delete;

private:
    std::pmr::vector<std::pmr::string> field_names_;
    bool wildcard_{false};
};

[[nodiscard]] http_vary_spec parse_http_vary(
    std::string_view field_value, std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] http_vary_spec parse_http_vary(
    std::span<const http_header> headers, std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] http_vary_spec parse_http_vary(
    std::span<const http_header_view> headers, std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] http_vary_spec parse_http_vary(
    const http_response_headers& headers, std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] http_vary_spec parse_http_vary(
    const http_response& response, std::pmr::memory_resource* resource = nullptr);

// Normalizes the request header values nominated by `vary` in canonical field
// order for cache-key generation. Returns std::nullopt when vary.is_wildcard().
[[nodiscard]] std::optional<std::pmr::string> format_http_vary_cache_key(
    const http_vary_spec& vary, std::span<const http_header> request_headers,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] std::optional<std::pmr::string> format_http_vary_cache_key(
    const http_vary_spec& vary, std::span<const http_header_view> request_headers,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] std::optional<std::pmr::string> format_http_vary_cache_key(
    const http_vary_spec& vary, const http_request& request,
    std::pmr::memory_resource* resource = nullptr);

[[nodiscard]] bool http_vary_requests_match(const http_vary_spec& vary,
    std::span<const http_header> first_request, std::span<const http_header> second_request,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] bool http_vary_requests_match(const http_vary_spec& vary,
    std::span<const http_header_view> first_request,
    std::span<const http_header_view> second_request,
    std::pmr::memory_resource* resource = nullptr);
[[nodiscard]] bool http_vary_requests_match(const http_vary_spec& vary,
    const http_request& first_request, const http_request& second_request,
    std::pmr::memory_resource* resource = nullptr);

// RFC 9111 section 4.2.3 Age calculation.
struct http_cache_age_input final {
    std::optional<std::time_t> date_value_{};
    std::optional<std::uint64_t> age_value_{};
    std::time_t request_time_{0};
    std::time_t response_time_{0};
    std::time_t now_{0};
};

[[nodiscard]] std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept;

[[nodiscard]] constexpr std::uint64_t compute_http_cache_age(
    const http_cache_age_input& input) noexcept {
    const auto add_saturating = [](std::uint64_t left, std::uint64_t right) constexpr noexcept {
        constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        return left > maximum - right ? maximum : left + right;
    };
    const std::uint64_t apparent_age =
        (input.date_value_.has_value() && input.response_time_ > *input.date_value_)
            ? static_cast<std::uint64_t>(input.response_time_) -
                  static_cast<std::uint64_t>(*input.date_value_)
            : 0U;
    const std::uint64_t response_delay =
        input.response_time_ > input.request_time_
            ? static_cast<std::uint64_t>(input.response_time_) -
                  static_cast<std::uint64_t>(input.request_time_)
            : 0U;
    const std::uint64_t corrected_age_value =
        add_saturating(input.age_value_.value_or(0U), response_delay);
    const std::uint64_t corrected_initial_age = (std::max)(apparent_age, corrected_age_value);
    const std::uint64_t resident_time =
        input.now_ > input.response_time_
            ? static_cast<std::uint64_t>(input.now_) -
                  static_cast<std::uint64_t>(input.response_time_)
            : 0U;
    return add_saturating(corrected_initial_age, resident_time);
}

[[nodiscard]] constexpr std::uint64_t compute_http_cache_age(
    std::optional<std::time_t> date_value, std::optional<std::uint64_t> age_value,
    std::time_t request_time, std::time_t response_time, std::time_t now) noexcept {
    return compute_http_cache_age(http_cache_age_input{
        .date_value_ = date_value,
        .age_value_ = age_value,
        .request_time_ = request_time,
        .response_time_ = response_time,
        .now_ = now,
    });
}

[[nodiscard]] std::uint64_t compute_http_cache_age(std::span<const http_header> response_headers,
    std::time_t request_time, std::time_t response_time, std::time_t now) noexcept;

// RFC 9110 section 15.4.5 and RFC 9111 section 4.3.4: representation metadata
// and framing fields that must not be emitted in a 304 Not Modified response or
// overwrite stored representation headers during 304 cache freshening.
[[nodiscard]] bool is_http_304_forbidden_header(std::string_view field_name) noexcept;
[[nodiscard]] inline bool is_http_not_modified_representation_metadata(
    std::string_view field_name) noexcept {
    return is_http_304_forbidden_header(field_name);
}

void sanitize_http_304_response_headers(std::pmr::vector<http_header>& headers);
void sanitize_http_304_response_headers(http_response_headers& headers);
void sanitize_http_304_response_headers(http_response& response);

void merge_http_304_response_headers(std::pmr::vector<http_header>& cached_headers,
    std::span<const http_header> not_modified_headers);
void merge_http_304_response_headers(std::pmr::vector<http_header>& cached_headers,
    std::span<const http_header_view> not_modified_headers);
void merge_http_304_response_headers(
    http_response_headers& cached_headers, const http_response_headers& not_modified_headers);
void merge_http_304_response_headers(
    http_response& cached_response, const http_response& not_modified_response);

}  // namespace ruvia
