#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/borrowed_view.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_request_trailers.h"

namespace ruvia::detail {

// RFC 9110 section 5.6.1 deliberately gives senders and recipients different
// list obligations: senders cannot generate empty members, while recipients
// must ignore a reasonable number of them. The HTTP/1 head-size limit bounds
// recipient work, so the tolerant path remains allocation-free and O(head).
enum class http_field_list_role : std::uint8_t { recipient,
    sender };

enum class http_field_list_parse_status : std::uint8_t { ok,
    malformed,
    rejected };

// The one role-aware list walk (RFC 9110 section 5.6.1) over the quote-aware
// splitter: an empty member is malformed for a sender and skipped for a
// recipient; every other member goes to `visitor`, whose non-ok status stops
// the walk and is returned.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status http_visit_list_members(
    std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    auto status = http_field_list_parse_status::ok;
    http_visit_comma_separated_quoted_field_items(value, [&status, role, &visitor](std::string_view member) {
        if (member.empty()) {
            if (role == http_field_list_role::sender) {
                status = http_field_list_parse_status::malformed;
            }
        } else {
            status = visitor(member);
        }
        return status == http_field_list_parse_status::ok;
    });
    return status;
}

// `#token` lists: a member that is not a token is malformed; a visitor that
// returns false rejects the field.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status http_visit_token_list(
    std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    return http_visit_list_members(value, role, [&visitor](std::string_view member) {
        if (!is_valid_http_header_name(member)) {
            return http_field_list_parse_status::malformed;
        }
        return visitor(member) ? http_field_list_parse_status::ok : http_field_list_parse_status::rejected;
    });
}

// The first nonempty list member accepted by `predicate`, or an empty view.
// Members are not validated; a quoted-string member is never split.
template <typename predicate_type>
[[nodiscard]] inline std::string_view http_find_header_token(
    std::string_view value, predicate_type&& predicate) noexcept {
    std::string_view found;
    http_visit_comma_separated_quoted_field_items(value, [&found, &predicate](std::string_view member) noexcept {
        if (!member.empty() && predicate(member)) {
            found = member;
            return false;
        }
        return true;
    });
    return found;
}

template <http_temporary_owning_char_string value_type, typename predicate_type>
std::string_view http_find_header_token(value_type&&, predicate_type&&) = delete;

// Accept-style negotiation fields skip empty list items without a role rule;
// grammar-checked lists use http_visit_list_members.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted(std::string_view value, visitor_type&& visitor) {
    http_visit_comma_separated_quoted_field_items(
        value, [&visitor](std::string_view item) { return item.empty() || visitor(item); });
}

// Plain comma-delimited token scan for fields whose grammar never permits
// quoted-string (such as Connection and Vary). Empty segments are skipped.
template <typename visitor_type>
inline void http_visit_comma_separated_tokens(std::string_view value, visitor_type&& visitor) {
    for (;;) {
        const auto comma = value.find(',');
        const auto item = http_trim_ows(value.substr(0, comma));
        if (!item.empty() && !visitor(item)) {
            return;
        }
        if (comma == std::string_view::npos) {
            return;
        }
        value.remove_prefix(comma + 1);
    }
}

// True when `predicate` accepts every ';'-separated parameter that follows a
// field value's leading token — the media type, coding or extension name, which
// carries no '=' and is never a parameter itself. The scan is quote-aware, so a
// ';' inside a quoted parameter value does not split, and each parameter reaches
// the predicate with its surrounding whitespace trimmed. A value with no ';' has
// no parameters and trivially satisfies the predicate.
//
// Unlike http_visit_semicolon_parameters_quoted_field this reports rejection: a predicate
// that returns false stops the walk and fails the whole field value. Use it
// where a malformed parameter must invalidate the field rather than be skipped.
template <typename predicate_type>
[[nodiscard]] inline bool http_all_parameters(std::string_view value, predicate_type&& predicate) {
    auto start = http_find_unquoted_delimiter(value, 0, ';');
    if (start >= value.size()) {
        return true;
    }
    ++start;
    while (start <= value.size()) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ';');
        if (!predicate(http_trim_ows(value.substr(start, end - start)))) {
            return false;
        }
        if (end >= value.size()) {
            return true;
        }
        start = end + 1;
    }
    return true;
}

[[nodiscard]] inline bool http_has_token(std::string_view value, std::string_view expected) noexcept {
    if (expected.empty()) {
        return false;
    }
    const auto expected_first = http_ascii_to_lower(static_cast<unsigned char>(expected.front()));
    return !http_find_header_token(value, [expected, expected_first](std::string_view token) noexcept {
        return token.size() == expected.size() &&
               http_ascii_to_lower(static_cast<unsigned char>(token.front())) == expected_first &&
               http_ascii_equals_ignore_case(token, expected);
    }).empty();
}

[[nodiscard]] inline bool is_http_token_or_quoted_string(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return value.front() == '"' ? http_is_quoted_string(value) : is_valid_http_header_name(value);
}

// RFC 9112 section 7 transfer-parameter: `token BWS "=" BWS ( token / quoted-string )`.
// `part` is one OWS-trimmed segment as produced by http_all_parameters.
[[nodiscard]] inline bool is_valid_http_transfer_parameter(std::string_view part) noexcept {
    const auto equals = part.find('=');
    return equals != std::string_view::npos && is_valid_http_header_name(http_trim_ows(part.substr(0, equals))) &&
           is_http_token_or_quoted_string(http_trim_ows(part.substr(equals + 1)));
}

[[nodiscard]] inline std::string_view http_header_token_before_parameters(
    std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    return http_trim_ows(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
}

template <http_temporary_owning_char_string value_type>
std::string_view http_header_token_before_parameters(value_type&&) = delete;

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class http_field_kind : std::uint8_t;

// Shared request/response restrictions (RFC 9110 section 6.5.1): fields that
// control framing, routing, authentication or representation interpretation.
// Direction-specific permissions are composed below and by response policy.
[[nodiscard]] bool is_forbidden_common_trailer_name(
    std::string_view name, http_field_kind kind) noexcept;

// Trailer advertisements and actual request sections use the same policy.
// Accept-Ranges is permitted in response trailers, not request trailers.
[[nodiscard]] bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept;

template <typename forbidden_name_type>
[[nodiscard]] inline bool is_valid_http_trailer_field_value(
    std::string_view value, http_field_list_role role, forbidden_name_type&& forbidden_name) noexcept {
    // RFC 9110 section 5.6.1.1: senders must not generate empty list elements,
    // but `#field-name` itself can represent an empty list.
    const auto list_role = http_trim_ows(value).empty() ? http_field_list_role::recipient : role;
    return http_visit_token_list(value, list_role, [&forbidden_name](std::string_view name) noexcept {
        return !forbidden_name(name);
    }) == http_field_list_parse_status::ok;
}

[[nodiscard]] inline bool is_valid_http_request_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_http_request_trailer_name(name); });
}

// The single rule for one generated request trailer field: valid field syntax
// (RFC 9110 section 5), a name a sender may place in request trailers (RFC 9110
// section 6.5.1), and a charge to the section `budget`, which is unchanged on
// failure.
[[nodiscard]] inline std::optional<http_request_trailer_error> charge_http_request_trailer_field(
    http_field_section_budget& budget, std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return http_request_trailer_error::invalid_field;
    }
    if (is_forbidden_http_request_trailer_name(name)) {
        return http_request_trailer_error::forbidden_field;
    }
    if (!budget.add_field(name, value)) {
        return http_request_trailer_error::section_too_large;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail
