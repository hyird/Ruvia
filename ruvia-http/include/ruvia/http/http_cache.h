#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
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

// Wire width of an RFC 9110 IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
inline constexpr std::size_t http_imf_fixdate_size = 29;

// RFC 9110 IMF-fixdate, or no value if the timestamp cannot be represented.
// The fixed-size result contains the 29 wire bytes without a terminator.
[[nodiscard]] std::optional<std::array<char, http_imf_fixdate_size>> format_http_date(std::time_t value) noexcept;

// Parse an HTTP-date (RFC 7231 section 7.1.1.1: IMF-fixdate / RFC 850 / asctime) as used by Date,
// Expires, Last-Modified, If-Modified-Since. Returns std::nullopt if malformed.
[[nodiscard]] std::optional<std::time_t> parse_http_date(std::string_view value) noexcept;

}  // namespace ruvia

namespace ruvia {

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
    private_unqualified = 1U << 10,
    no_cache_unqualified = 1U << 11,
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
    [[nodiscard]] constexpr bool is_unqualified_private() const noexcept {
        return has(cache_control_directive::private_unqualified);
    }
    [[nodiscard]] constexpr bool is_field_qualified_private() const noexcept {
        return is_private() && !is_unqualified_private();
    }
    [[nodiscard]] constexpr bool is_unqualified_no_cache() const noexcept {
        return has(cache_control_directive::no_cache_unqualified);
    }
    [[nodiscard]] constexpr bool is_field_qualified_no_cache() const noexcept {
        return no_cache() && !is_unqualified_no_cache();
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

    [[nodiscard]] constexpr std::optional<std::uint64_t> s_maxage() const noexcept {
        return s_max_age_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_while_revalidate() const noexcept {
        return stale_while_revalidate_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_if_error() const noexcept {
        return stale_if_error_;
    }

    [[nodiscard]] constexpr std::size_t private_field_count() const noexcept {
        return is_unqualified_private() ? 0U : private_fields_.count_;
    }
    [[nodiscard]] constexpr std::string_view private_field(
        std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return is_unqualified_private() ? std::string_view{} : private_fields_.at(index);
    }
    std::string_view private_field(std::size_t) const&& = delete;
    [[nodiscard]] bool has_private_field(std::string_view field_name) const noexcept;

    [[nodiscard]] constexpr std::size_t no_cache_field_count() const noexcept {
        return is_unqualified_no_cache() ? 0U : no_cache_fields_.count_;
    }
    [[nodiscard]] constexpr std::string_view no_cache_field(
        std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return is_unqualified_no_cache() ? std::string_view{} : no_cache_fields_.at(index);
    }
    std::string_view no_cache_field(std::size_t) const&& = delete;
    [[nodiscard]] bool has_no_cache_field(std::string_view field_name) const noexcept;

private:
    friend class cache_control_field_parser;
    friend std::optional<cache_control> parse_targeted_cache_control(
        std::string_view value) noexcept;
    template <typename input_type>
    friend std::optional<cache_control> parse_targeted_cache_control_from_input(
        input_type input) noexcept;

    static constexpr std::size_t max_nominated_fields = 16;
    static constexpr std::size_t max_nominated_field_bytes = 256;

    struct nominated_field_set final {
        std::array<char, max_nominated_field_bytes> storage_{};
        std::array<std::uint16_t, max_nominated_fields> offsets_{};
        std::array<std::uint8_t, max_nominated_fields> lengths_{};
        std::uint16_t storage_used_{0};
        std::uint8_t count_{0};

        constexpr void clear() noexcept {
            storage_used_ = 0;
            count_ = 0;
        }

        [[nodiscard]] constexpr std::string_view at(std::size_t index) const noexcept {
            if (index >= count_) {
                return {};
            }
            return std::string_view(storage_.data() + offsets_[index], lengths_[index]);
        }

        [[nodiscard]] bool contains(std::string_view field_name) const noexcept;
        [[nodiscard]] bool add_lowercase(std::string_view field_name) noexcept;
    };

    constexpr void set(cache_control_directive directive) noexcept {
        directives_ |= static_cast<std::uint16_t>(directive);
    }
    constexpr void clear_directive(cache_control_directive directive) noexcept {
        directives_ &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(directive));
    }

    std::uint16_t directives_{0};
    std::optional<std::uint64_t> max_age_;
    std::optional<std::uint64_t> max_stale_;
    std::optional<std::uint64_t> min_fresh_;
    std::optional<std::uint64_t> s_max_age_;
    std::optional<std::uint64_t> stale_while_revalidate_;
    std::optional<std::uint64_t> stale_if_error_;
    nominated_field_set private_fields_{};
    nominated_field_set no_cache_fields_{};
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

struct targeted_cache_control_selection final {
    cache_control control_{};
    std::string_view selected_header_{};
    bool targeted_{false};
};

class http_vary_spec;

namespace detail {
[[nodiscard]] cache_control parse_cache_control_headers(http_header_range_view headers) noexcept;
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control_headers(
    http_header_range_view headers, std::string_view header_name) noexcept;
[[nodiscard]] targeted_cache_control_selection resolve_targeted_cache_control_headers(
    http_header_range_view headers, std::span<const std::string_view> target_headers) noexcept;
[[nodiscard]] std::string rewrite_http_cache_control_max_age_headers(
    http_header_range_view headers, std::uint64_t client_max_age);
[[nodiscard]] std::optional<std::string> format_http_vary_cache_key_headers(
    const http_vary_spec& vary, http_header_range_view request_headers);
[[nodiscard]] std::uint64_t compute_http_cache_age_headers(
    http_header_range_view response_headers, std::time_t request_time, std::time_t response_time,
    std::time_t now) noexcept;
void merge_http_304_response_headers_into(
    std::vector<http_header>& cached_headers, http_header_range_view not_modified_headers);
void merge_http_304_response_headers_into(
    http_response_headers& cached_headers, http_header_range_view not_modified_headers);
}  // namespace detail

// Parse a Cache-Control field value (a single line, or several joined by commas).
[[nodiscard]] cache_control parse_cache_control(std::string_view value) noexcept;
template <http_header_field_source headers_type>
[[nodiscard]] cache_control parse_cache_control(const headers_type& headers) noexcept {
    return detail::parse_cache_control_headers(headers);
}

// RFC 9213 Targeted HTTP Cache Control (e.g. CDN-Cache-Control).
// Parses an RFC 8941 Structured Field Dictionary of cache directives; returns
// std::nullopt when absent, empty, or syntactically invalid per RFC 9213 §2.1.
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control(
    std::string_view value) noexcept;
template <http_header_field_source headers_type>
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control(
    const headers_type& headers,
    std::string_view header_name = "CDN-Cache-Control") noexcept {
    return detail::parse_targeted_cache_control_headers(headers, header_name);
}

// RFC 9213 §2.2 target-list selection: evaluates `target_headers` in priority
// order and returns the first present, valid non-empty targeted policy; falls
// back to standard Cache-Control when no targeted header matches.
template <http_header_field_source headers_type>
[[nodiscard]] targeted_cache_control_selection resolve_targeted_cache_control(
    const headers_type& headers,
    std::span<const std::string_view> target_headers) noexcept {
    return detail::resolve_targeted_cache_control_headers(headers, target_headers);
}

// RFC 9111 §5.2.2.7 & §5.2.2.3 field-qualified header stripping for shared caches.
// Returns false if unqualified `private` or `no-store` forbids shared-cache
// storage; returns true when storable (after stripping any field-qualified
// `private="field1, field2"` headers in place).
bool strip_http_cache_private_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept;
bool strip_http_cache_private_headers(
    http_response_headers& headers, const cache_control& control);
bool strip_http_cache_private_headers(
    http_response& response, const cache_control& control);
bool strip_http_cache_private_headers(std::vector<http_header>& headers) noexcept;
bool strip_http_cache_private_headers(http_response_headers& headers);
bool strip_http_cache_private_headers(http_response& response);

bool strip_http_cache_no_cache_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept;
bool strip_http_cache_no_cache_headers(
    http_response_headers& headers, const cache_control& control);
bool strip_http_cache_no_cache_headers(
    http_response& response, const cache_control& control);
[[nodiscard]] bool http_cache_control_has_no_transform(std::string_view field_value) noexcept;
template <http_header_field_source headers_type>
[[nodiscard]] bool http_cache_control_has_no_transform(const headers_type& headers) noexcept {
    return parse_cache_control(headers).no_transform();
}

struct http_cache_control_directive_view final {
    std::string_view raw_{};
    std::string_view name_{};
    std::optional<std::string_view> argument_{};
};

// Rewrites Cache-Control to preserve all directives except max-age and s-maxage
// while setting client max-age=<client_max_age>. Header-block overloads also
// clear Expires so the rewritten max-age governs client freshness.
[[nodiscard]] std::string rewrite_http_cache_control_max_age(
    std::string_view field_value, std::uint64_t client_max_age);
template <http_header_field_source headers_type>
[[nodiscard]] std::string rewrite_http_cache_control_max_age(
    const headers_type& headers, std::uint64_t client_max_age) {
    return detail::rewrite_http_cache_control_max_age_headers(headers, client_max_age);
}
void rewrite_http_response_cache_max_age(
    std::vector<http_header>& response_headers, std::uint64_t client_max_age);
void rewrite_http_response_cache_max_age(
    http_response_headers& response_headers, std::uint64_t client_max_age);
void rewrite_http_response_cache_max_age(
    http_response& response, std::uint64_t client_max_age);

// Parsed multi-line Vary specification (RFC 9111 section 4.1). Field names are
// normalized to lowercase, deduplicated, and sorted in fixed ASCII order.
class http_vary_spec final {
public:
    http_vary_spec() noexcept = default;

    void update(std::string_view field_value);
    template <http_header_field_source headers_type>
    void update_headers(const headers_type& headers) {
        update_header_range(headers);
    }

    [[nodiscard]] constexpr bool is_wildcard() const noexcept {
        return wildcard_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return !wildcard_ && field_names_.empty();
    }
    [[nodiscard]] std::span<const std::string> field_names() const& noexcept RUVIA_LIFETIMEBOUND {
        return field_names_;
    }
    std::span<const std::string> field_names() const&& = delete;

private:
    void update_header_range(detail::http_header_range_view headers);

    std::vector<std::string> field_names_;
    bool wildcard_{false};
};

[[nodiscard]] http_vary_spec parse_http_vary(std::string_view field_value);
template <http_header_field_source headers_type>
[[nodiscard]] http_vary_spec parse_http_vary(const headers_type& headers) {
    http_vary_spec spec;
    spec.update_headers(headers);
    return spec;
}

// Normalizes the request header values nominated by `vary` in canonical field
// order for cache-key generation. Returns std::nullopt when vary.is_wildcard().
template <http_header_field_source headers_type>
[[nodiscard]] std::optional<std::string> format_http_vary_cache_key(
    const http_vary_spec& vary, const headers_type& request_headers) {
    return detail::format_http_vary_cache_key_headers(vary, request_headers);
}

template <http_header_field_source first_type, http_header_field_source second_type>
[[nodiscard]] bool http_vary_requests_match(const http_vary_spec& vary,
    const first_type& first_request, const second_type& second_request) {
    const auto first_key = format_http_vary_cache_key(vary, first_request);
    const auto second_key = format_http_vary_cache_key(vary, second_request);
    return first_key.has_value() && second_key.has_value() && *first_key == *second_key;
}
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

template <http_header_field_source headers_type>
[[nodiscard]] std::uint64_t compute_http_cache_age(const headers_type& response_headers,
    std::time_t request_time, std::time_t response_time, std::time_t now) noexcept {
    return detail::compute_http_cache_age_headers(
        response_headers, request_time, response_time, now);
}

// RFC 9110 section 15.4.5 and RFC 9111 section 4.3.4: representation metadata
// and framing fields that must not be emitted in a 304 Not Modified response or
// overwrite stored representation headers during 304 cache freshening.
[[nodiscard]] bool is_http_304_forbidden_header(std::string_view field_name) noexcept;

void sanitize_http_304_response_headers(std::vector<http_header>& headers);
void sanitize_http_304_response_headers(http_response_headers& headers);
void sanitize_http_304_response_headers(http_response& response);

template <http_header_field_source headers_type>
void merge_http_304_response_headers(
    std::vector<http_header>& cached_headers, const headers_type& not_modified_headers) {
    if constexpr (std::same_as<std::remove_cvref_t<headers_type>, std::vector<http_header>>) {
        if (&cached_headers == &not_modified_headers) {
            auto copy = not_modified_headers;
            detail::merge_http_304_response_headers_into(cached_headers, copy);
            return;
        }
    }
    detail::merge_http_304_response_headers_into(cached_headers, not_modified_headers);
}
template <http_header_field_source headers_type>
void merge_http_304_response_headers(
    http_response_headers& cached_headers, const headers_type& not_modified_headers) {
    if constexpr (std::same_as<std::remove_cvref_t<headers_type>, http_response_headers>) {
        if (&cached_headers == &not_modified_headers) {
            auto copy = not_modified_headers.clone();
            detail::merge_http_304_response_headers_into(cached_headers, copy);
            return;
        }
    }
    detail::merge_http_304_response_headers_into(cached_headers, not_modified_headers);
}
void merge_http_304_response_headers(
    http_response& cached_response, const http_response& not_modified_response);

}  // namespace ruvia

namespace ruvia {

struct http_conditional_method_plan final {
    bool evaluates_preconditions_;
    bool uses_not_modified_response_;
    bool evaluates_if_modified_since_;
    bool evaluates_range_;
};

[[nodiscard]] inline constexpr http_conditional_method_plan get_http_conditional_method_plan(
    http_known_method method) noexcept {
    switch (method) {
        case http_known_method::get:
            return {true, true, true, true};
        case http_known_method::head:
            return {true, true, true, false};
        case http_known_method::post:
        case http_known_method::put:
        case http_known_method::delete_value:
        case http_known_method::patch:
            return {true, false, false, false};
        case http_known_method::options:
        case http_known_method::connect:
        case http_known_method::unknown:
            return {false, false, false, false};
    }
    return {false, false, false, false};
}

// Precondition and range fields borrowed from the request. Presence of If-Range
// is tracked separately because an empty value is still a present field.
struct http_conditional_headers final {
    std::string_view if_unmodified_since_;
    std::string_view if_modified_since_;
    std::string_view range_;
    std::string_view if_range_;
    bool has_if_range_;
};

// Repeated If-Modified-Since and If-Unmodified-Since lines are ignored as
// invalid date conditions (RFC 9110 sections 13.1.3 and 13.1.4); their views
// are empty here rather than exposing the last line as an effective condition.
[[nodiscard]] http_conditional_headers get_http_conditional_headers(const http_request& request) noexcept;
http_conditional_headers get_http_conditional_headers(const http_request&&) = delete;

// One entity-tag precondition field's outcome, accumulated across every field
// line of that name. `valid` is false for a malformed field, which the caller
// must treat differently from a field that simply did not match. A valid
// wildcard is the only member across all accumulated field lines.
struct http_etag_field_condition final {
    bool present_{false};
    bool valid_{true};
    bool matched_{false};
    bool wildcard_{false};

    void update(std::string_view value, std::string_view expected, bool strong) noexcept;

    [[nodiscard]] bool matches() const noexcept {
        return valid_ && (wildcard_ || matched_);
    }
};

struct http_etag_preconditions final {
    http_etag_field_condition if_match_;
    http_etag_field_condition if_none_match_;
};

[[nodiscard]] http_etag_preconditions get_http_etag_preconditions(
    const http_request& request, std::string_view etag) noexcept;

// If-Modified-Since / If-Unmodified-Since: the "<=" comparisons of RFC 9110
// section 13.1.3 and 13.1.4. A malformed date is ignored.
[[nodiscard]] bool http_date_not_modified(
    std::string_view header_value, std::time_t modified_seconds) noexcept;

[[nodiscard]] bool http_date_unmodified(
    std::string_view header_value, std::time_t modified_seconds) noexcept;

// Whether an If-Range still authorises a range response. A date validator here
// must match EXACTLY, unlike If-Modified-Since.
[[nodiscard]] bool http_if_range_allows(std::string_view header_value, std::string_view etag,
    std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept;

// Single-value RFC 9110 §8.8.2 entity-tag validation, comparison, and weakening.
[[nodiscard]] bool is_valid_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool is_weak_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool is_strong_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool http_strong_etag_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool http_weak_etag_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] std::string weaken_http_etag(std::string_view etag);
}  // namespace ruvia

namespace ruvia {

// Case-insensitive set of field/option tokens parsed from one or more
// Connection header lines (RFC 9110 section 7.6.1).
class http_connection_nominated_fields final {
public:
    http_connection_nominated_fields() noexcept = default;

    void update(std::string_view connection_field_value);
    template <http_header_field_source headers_type>
    void update_headers(const headers_type& headers) {
        update_header_range(headers);
    }

    [[nodiscard]] bool contains(std::string_view field_name) const noexcept;
    [[nodiscard]] constexpr bool has_close() const noexcept {
        return has_close_;
    }
    [[nodiscard]] constexpr bool has_keep_alive() const noexcept {
        return has_keep_alive_;
    }
    [[nodiscard]] constexpr bool has_upgrade() const noexcept {
        return has_upgrade_;
    }
    [[nodiscard]] constexpr bool has_te() const noexcept {
        return has_te_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return tokens_.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return tokens_.size();
    }
    [[nodiscard]] std::span<const std::string> tokens() const& noexcept RUVIA_LIFETIMEBOUND {
        return tokens_;
    }
    std::span<const std::string> tokens() const&& = delete;

    [[nodiscard]] std::vector<std::string> non_standard_options() const;

    [[nodiscard]] std::string rebuild_connection_field(
        bool retain_upgrade = false, bool include_non_standard = true) const;

private:
    void update_header_range(detail::http_header_range_view headers);

    std::vector<std::string> tokens_;
    bool has_close_{false};
    bool has_keep_alive_{false};
    bool has_upgrade_{false};
    bool has_te_{false};
};

[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::string_view connection_field_value);
template <http_header_field_source headers_type>
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    const headers_type& headers) {
    http_connection_nominated_fields nominated;
    nominated.update_headers(headers);
    return nominated;
}

// Protocol hop-by-hop fields (RFC 9110 section 7.6.1): Connection, Keep-Alive,
// Proxy-Connection, TE, Transfer-Encoding, Trailer, Upgrade,
// Proxy-Authenticate, and Proxy-Authorization.
[[nodiscard]] bool is_http_hop_by_hop_field_name(std::string_view field_name) noexcept;

// Standard connection control/hop-by-hop tokens ("close", "keep-alive", and the
// protocol hop-by-hop field names).
[[nodiscard]] bool is_http_standard_connection_option(std::string_view option) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated, bool retain_upgrade = false) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_trailer_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated = http_connection_nominated_fields{}) noexcept;

[[nodiscard]] bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept;
[[nodiscard]] bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept;

}  // namespace ruvia

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
