#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/detail/util/ascii_case.h"
#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/detail/util/http_ows.h"

namespace ruvia::detail {

// Append `value` to `out`, decoding RFC 7230 §3.2.6 quoted-pairs ("\X" -> "X").
// `value` must be a quote-trimmed parameter value: a valid unquoted token cannot
// contain a backslash, so any '\' present came from a quoted-string and is an
// escape. (A trailing lone '\' -- only possible from malformed input -- is emitted
// verbatim.) Used to unescape multipart Content-Disposition name/filename.
inline void http_append_decoded_quoted_pairs(std::pmr::string& out, std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            ++i;
        }
        out.push_back(value[i]);
    }
}

[[nodiscard]] inline std::string_view http_trim_quotes(std::string_view value) noexcept {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

template <http_temporary_owning_char_string value_type>
std::string_view http_trim_quotes(value_type&&) = delete;

template <typename predicate_type>
[[nodiscard]] inline std::string_view http_find_header_token(
    std::string_view value, predicate_type&& predicate) noexcept {
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto token =
            http_trim_ows(comma == std::string_view::npos ? value : value.substr(0, comma));
        if (!token.empty() && predicate(token)) {
            return token;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return {};
}

template <http_temporary_owning_char_string value_type, typename predicate_type>
std::string_view http_find_header_token(value_type&&, predicate_type&&) = delete;

// Index of the next `delimiter` in `value` at/after `start` that is not inside an
// RFC quoted-string (honoring quoted-pairs, so a `\"` does not end the string), or
// value.size() if there is none. Sole owner of the quote-aware delimiter scan
// shared by the comma-list and semicolon-parameter quoted visitors below.
[[nodiscard]] inline std::size_t http_find_unquoted_delimiter(
    std::string_view value, std::size_t start, char delimiter) noexcept {
    bool in_quotes = false;
    for (std::size_t i = start; i < value.size(); ++i) {
        const char c = value[i];
        if (in_quotes) {
            if (c == '\\' && i + 1 < value.size()) {
                ++i;
            } else if (c == '"') {
                in_quotes = false;
            }
        } else if (c == '"') {
            in_quotes = true;
        } else if (c == delimiter) {
            return i;
        }
    }
    return value.size();
}

// Iterate every item in a comma-delimited HTTP list whose items may contain
// quoted-string parameters. A comma inside "..." is data, not a list separator.
// Empty items are still reported; framing-sensitive callers can reject them.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted_items(std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = http_find_unquoted_delimiter(value, start, ',');
        const auto item = http_trim_ows(value.substr(start, end - start));
        if (!visitor(item)) {
            return;
        }
        if (end >= value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Accept-style negotiation fields can skip empty list items; stricter headers
// should use http_visit_comma_separated_quoted_items directly.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted(std::string_view value, visitor_type&& visitor) {
    http_visit_comma_separated_quoted_items(
        value, [&visitor](std::string_view item) { return item.empty() || visitor(item); });
}

// Iterate the `key=value` parameters of a `;`-delimited list (cookie pairs,
// Content-Type / Content-Disposition parameters). Each key and value is trimmed
// of OWS; segments without '=' are skipped. The visitor returns false to stop
// (e.g. once it has found the parameter it wants). Quote-stripping and key
// matching are left to the caller, which differs per RFC.
// Split one already-OWS-trimmed "name=value" segment on its first '=', trim OWS
// from each side, and hand the pair to the visitor. Sole owner of the parameter
// key/value emit shared by both semicolon scanners below. Returns the visitor's
// keep-going result; a segment with no '=' is not a parameter, so it is skipped
// (returns true to continue the scan).
template <typename visitor_type>
[[nodiscard]] inline bool http_emit_semicolon_parameter(std::string_view part, visitor_type&& visitor) {
    const auto equals = part.find('=');
    if (equals == std::string_view::npos) {
        return true;
    }
    return visitor(http_trim_ows(part.substr(0, equals)), http_trim_ows(part.substr(equals + 1)));
}

template <typename visitor_type>
inline void http_visit_semicolon_parameters(std::string_view value, visitor_type&& visitor) {
    while (!value.empty()) {
        const auto semicolon = value.find(';');
        const auto part =
            http_trim_ows(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
        if (!http_emit_semicolon_parameter(part, visitor)) {
            return;
        }
        if (semicolon == std::string_view::npos) {
            return;
        }
        value.remove_prefix(semicolon + 1);
    }
}

// True when `predicate` accepts every ';'-separated parameter that follows a
// field value's leading token — the media type, coding or extension name, which
// carries no '=' and is never a parameter itself. The scan is quote-aware, so a
// ';' inside a quoted parameter value does not split, and each parameter reaches
// the predicate with its surrounding whitespace trimmed. A value with no ';' has
// no parameters and trivially satisfies the predicate.
//
// Unlike http_visit_semicolon_parameters_quoted this reports rejection: a predicate
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
        const auto end = http_find_unquoted_delimiter(value, start, ';');
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

// Like http_visit_semicolon_parameters, but treats an RFC quoted-string value as
// opaque so a ';' inside a "..." value does not split the parameter. Use for
// Content-Type / Content-Disposition parameters (RFC 7231 §3.1.1.1, RFC 6266),
// whose values may be quoted-strings. Do NOT use for Cookie headers: RFC 6265
// gives '"' no special meaning there, so cookie parsing must stay literal.
template <typename visitor_type>
inline void http_visit_semicolon_parameters_quoted(std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = http_find_unquoted_delimiter(value, start, ';');
        const auto part = http_trim_ows(value.substr(start, end - start));
        if (!http_emit_semicolon_parameter(part, visitor)) {
            return;
        }
        if (end >= value.size()) {
            return;
        }
        start = end + 1;
    }
}

[[nodiscard]] inline std::optional<std::string_view> http_find_semicolon_parameter(
    std::string_view value, std::string_view name) {
    std::optional<std::string_view> result;
    http_visit_semicolon_parameters(
        value, [name, &result](std::string_view key, std::string_view parameter_value) {
            if (key == name) {
                result = parameter_value;
            }
            return true;
        });
    return result;
}

template <http_temporary_owning_char_string value_type>
std::optional<std::string_view> http_find_semicolon_parameter(value_type&&, std::string_view) = delete;

[[nodiscard]] inline std::optional<std::string_view> http_find_semicolon_parameter_quoted(
    std::string_view value, std::string_view name) {
    std::optional<std::string_view> result;
    http_visit_semicolon_parameters_quoted(
        value, [name, &result](std::string_view key, std::string_view parameter_value) {
            if (key == name) {
                result = parameter_value;
            }
            return true;
        });
    return result;
}

template <http_temporary_owning_char_string value_type>
std::optional<std::string_view> http_find_semicolon_parameter_quoted(
    value_type&&, std::string_view) = delete;

[[nodiscard]] inline std::optional<std::string_view> http_find_semicolon_parameter_quoted_ignore_case(
    std::string_view value, std::string_view name) {
    std::optional<std::string_view> result;
    http_visit_semicolon_parameters_quoted(
        value, [name, &result](std::string_view key, std::string_view parameter_value) {
            if (http_ascii_equals_ignore_case(key, name)) {
                result = parameter_value;
            }
            return true;
        });
    return result;
}

template <http_temporary_owning_char_string value_type>
std::optional<std::string_view> http_find_semicolon_parameter_quoted_ignore_case(
    value_type&&, std::string_view) = delete;

[[nodiscard]] inline std::optional<std::string_view> http_find_semicolon_parameter_ignore_case(
    std::string_view value, std::string_view name) {
    std::optional<std::string_view> result;
    http_visit_semicolon_parameters(
        value, [name, &result](std::string_view key, std::string_view parameter_value) {
            if (http_ascii_equals_ignore_case(key, name)) {
                result = parameter_value;
            }
            return true;
        });
    return result;
}

template <http_temporary_owning_char_string value_type>
std::optional<std::string_view> http_find_semicolon_parameter_ignore_case(
    value_type&&, std::string_view) = delete;

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

[[nodiscard]] inline bool http_has_exact_token(
    std::string_view value, std::string_view expected) noexcept {
    if (expected.empty()) {
        return false;
    }
    return !http_find_header_token(value, [expected](std::string_view token) noexcept {
        return token == expected;
    }).empty();
}

}  // namespace ruvia::detail
