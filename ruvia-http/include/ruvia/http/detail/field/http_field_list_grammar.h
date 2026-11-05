#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_values.h"

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
