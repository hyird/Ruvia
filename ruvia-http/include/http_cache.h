#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "borrowed_text.h"
#include "decimal_text.h"
#include "http_header.h"
#include "http_response.h"

namespace ruvia {

class http_response;

enum class cache_control_directive : std::uint16_t {
    no_store = 1U << 0,
    no_cache = 1U << 1,
    no_transform = 1U << 2,
    must_revalidate = 1U << 3,
    proxy_revalidate = 1U << 4,
    private_ = 1U << 5,
    public_ = 1U << 6,
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
        return has(cache_control_directive::private_);
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
        return has(cache_control_directive::public_);
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
    [[nodiscard]] constexpr std::string_view private_field(std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return is_unqualified_private() ? std::string_view{} : private_fields_.at(index);
    }
    std::string_view private_field(std::size_t) const&& = delete;
    [[nodiscard]] bool has_private_field(std::string_view field_name) const noexcept;

    [[nodiscard]] constexpr std::size_t no_cache_field_count() const noexcept {
        return is_unqualified_no_cache() ? 0U : no_cache_fields_.count_;
    }
    [[nodiscard]] constexpr std::string_view no_cache_field(std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return is_unqualified_no_cache() ? std::string_view{} : no_cache_fields_.at(index);
    }
    std::string_view no_cache_field(std::size_t) const&& = delete;
    [[nodiscard]] bool has_no_cache_field(std::string_view field_name) const noexcept;

private:
    friend class cache_control_field_parser;
    friend std::optional<cache_control> parse_targeted_cache_control(std::string_view value) noexcept;
    template <typename input_type>
    friend std::optional<cache_control> parse_targeted_cache_control_from_input(input_type input);

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

// Parse a Cache-Control field value (a single line, or several joined by
// commas), or every Cache-Control line of a header block.
[[nodiscard]] cache_control parse_cache_control(std::string_view value) noexcept;
[[nodiscard]] cache_control parse_cache_control(const http_header_range_view& headers) noexcept;

// RFC 9213 Targeted HTTP Cache Control (e.g. CDN-Cache-Control).
// Parses an RFC 8941 Structured Field Dictionary of cache directives; returns
// std::nullopt when absent, empty, or syntactically invalid per RFC 9213 §2.1.
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control(std::string_view value) noexcept;
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control(const http_header_range_view& headers, std::string_view header_name = "CDN-Cache-Control");

// RFC 9213 §2.2 target-list selection: evaluates `target_headers` in priority
// order and returns the first present, valid non-empty targeted policy; falls
// back to standard Cache-Control when no targeted header matches.
[[nodiscard]] targeted_cache_control_selection resolve_targeted_cache_control(const http_header_range_view& headers, std::span<const std::string_view> target_headers);

// RFC 9111 field-qualified stripping. Rejection (no-store, unqualified
// private, or a nominated protected name) leaves the entire block unchanged.
bool strip_http_cache_private_headers(editable_http_headers headers, const cache_control& control, std::span<const std::string_view> protected_fields = {}) noexcept;
// The no-cache="field1, field2" counterpart: false if `no-store` or
// unqualified `no-cache` forbids reuse without validation.
bool strip_http_cache_no_cache_headers(editable_http_headers headers, const cache_control& control) noexcept;

[[nodiscard]] bool http_cache_control_has_no_transform(std::string_view field_value) noexcept;
[[nodiscard]] bool http_cache_control_has_no_transform(const http_header_range_view& headers) noexcept;

// Rewrites only max-age, preserving s-maxage and every other directive.
// nullopt removes max-age without imposing a browser freshness lifetime.
[[nodiscard]] std::string rewrite_http_cache_control_max_age(std::string_view field_value, std::optional<std::uint64_t> client_max_age);
[[nodiscard]] std::string rewrite_http_cache_control_max_age(const http_header_range_view& headers, std::optional<std::uint64_t> client_max_age);
// Replaces Cache-Control atomically and clears Expires only when setting a TTL.
void rewrite_http_response_cache_max_age(editable_http_headers response_headers, std::optional<std::uint64_t> client_max_age);

// Parsed multi-line Vary specification (RFC 9111 section 4.1). Field names are
// normalized to lowercase, deduplicated, and sorted in fixed ASCII order.
class http_vary_spec final {
public:
    http_vary_spec() noexcept = default;

    void update(std::string_view field_value);
    void update_headers(const http_header_range_view& headers);

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
    std::vector<std::string> field_names_;
    bool wildcard_{false};
};

[[nodiscard]] http_vary_spec parse_http_vary(std::string_view field_value);
[[nodiscard]] http_vary_spec parse_http_vary(const http_header_range_view& headers);

// Normalizes the request header values nominated by `vary` in canonical field
// order for cache-key generation. Returns std::nullopt when vary.is_wildcard().
[[nodiscard]] std::optional<std::string> format_http_vary_cache_key(const http_vary_spec& vary, const http_header_range_view& request_headers);

[[nodiscard]] bool http_vary_requests_match(const http_vary_spec& vary, const http_header_range_view& first_request, const http_header_range_view& second_request);

// RFC 9111 section 4.2.3 Age calculation.
struct http_cache_age_input final {
    std::optional<std::time_t> date_value_{};
    std::optional<std::uint64_t> age_value_{};
    std::time_t request_time_{0};
    std::time_t response_time_{0};
    std::time_t now_{0};
};

[[nodiscard]] std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept;

[[nodiscard]] constexpr std::uint64_t compute_http_cache_age(const http_cache_age_input& input) noexcept {
    const std::uint64_t apparent_age = (input.date_value_.has_value() && input.response_time_ > *input.date_value_) ? static_cast<std::uint64_t>(input.response_time_) - static_cast<std::uint64_t>(*input.date_value_) : 0U;
    const std::uint64_t response_delay = input.response_time_ > input.request_time_ ? static_cast<std::uint64_t>(input.response_time_) - static_cast<std::uint64_t>(input.request_time_) : 0U;
    const std::uint64_t corrected_age_value = saturating_add(input.age_value_.value_or(0U), response_delay);
    const std::uint64_t corrected_initial_age = (std::max)(apparent_age, corrected_age_value);
    const std::uint64_t resident_time = input.now_ > input.response_time_ ? static_cast<std::uint64_t>(input.now_) - static_cast<std::uint64_t>(input.response_time_) : 0U;
    return saturating_add(corrected_initial_age, resident_time);
}

[[nodiscard]] constexpr std::uint64_t compute_http_cache_age(std::optional<std::time_t> date_value, std::optional<std::uint64_t> age_value, std::time_t request_time, std::time_t response_time, std::time_t now) noexcept {
    return compute_http_cache_age(http_cache_age_input{
        .date_value_ = date_value,
        .age_value_ = age_value,
        .request_time_ = request_time,
        .response_time_ = response_time,
        .now_ = now,
    });
}

[[nodiscard]] std::uint64_t compute_http_cache_age(const http_header_range_view& response_headers, std::time_t request_time, std::time_t response_time, std::time_t now) noexcept;

// RFC 9110 section 15.4.5 and RFC 9111 section 4.3.4: representation metadata
// and framing fields that must not be emitted in a 304 Not Modified response or
// overwrite stored representation headers during 304 cache freshening.
[[nodiscard]] bool is_http_304_forbidden_header(std::string_view field_name) noexcept;

void sanitize_http_304_response_headers(editable_http_headers headers) noexcept;

enum class http_cache_validator_match : std::uint8_t { none,
    unvalidated,
    weak,
    strong };

// RFC 9111 section 4.3.4 identification for one stored response. ETag takes
// precedence over Last-Modified. Equal dates are strong only when the stored
// Date is at least 60 seconds later (RFC 9110 section 8.8.2.2).
// An explicitly supplied Content-Encoding must match the stored coding stack;
// its absence in a 304 preserves the stored coding.
[[nodiscard]] http_cache_validator_match match_http_cache_validators(const http_header_range_view& cached_headers, const http_header_range_view& validating_headers) noexcept;

// Returns false without mutation when validators, encoding, or Connection
// syntax do not match. Otherwise atomically replaces eligible stored fields.
// Inputs may alias wholly or in part. No CDN policy is inferred.
bool merge_http_304_response_headers(editable_http_headers cached_headers, const http_header_range_view& not_modified_headers);

// Reuses representation ETag semantics when a cache transforms content coding.
// Invalid or repeated tags reject without mutation; absence is a no-op success.
bool weaken_http_cache_etag(editable_http_headers headers);

}  // namespace ruvia
