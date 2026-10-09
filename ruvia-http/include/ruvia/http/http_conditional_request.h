#pragma once

#include <cstddef>
#include <ctime>
#include <optional>
#include <string_view>

#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request.h"
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

[[nodiscard]] http_conditional_headers get_http_conditional_headers(const http_request& request) noexcept;

// One entity-tag precondition field's outcome, accumulated across every field
// line of that name. `valid` is false for a malformed field, which the caller
// must treat differently from a field that simply did not match.
struct http_etag_field_condition final {
    bool present_{false};
    bool valid_{true};
    bool matched_{false};
    bool wildcard_{false};
    std::size_t line_count_{0};

    void update(std::string_view value, std::string_view expected, bool strong) noexcept;

    [[nodiscard]] bool matches() const noexcept {
        return valid_ && ((wildcard_ && line_count_ == 1) || (!wildcard_ && matched_));
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

}  // namespace ruvia
