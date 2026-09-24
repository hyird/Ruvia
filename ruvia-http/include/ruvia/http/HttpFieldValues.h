#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/HttpFieldWhitespace.h"

namespace ruvia {

// Visits comma-separated field members, treating commas inside quoted strings
// (including escaped quotes) as data. Empty members are reported. Returning
// false from the visitor stops traversal. Views borrow the input field.
template <typename Visitor>
inline void httpVisitCommaSeparatedQuotedFieldItems(
    std::string_view value, Visitor&& visitor) {
    std::size_t start = 0;
    while (start <= value.size()) {
        bool quoted = false;
        std::size_t end = start;
        for (; end < value.size(); ++end) {
            const char c = value[end];
            if (quoted) {
                if (c == '\\' && end + 1 < value.size()) {
                    ++end;
                } else if (c == '"') {
                    quoted = false;
                }
            } else if (c == '"') {
                quoted = true;
            } else if (c == ',') {
                break;
            }
        }
        const auto item = httpTrimOws(value.substr(start, end - start));
        if (!visitor(item) || end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Visits semicolon-separated field parameters while treating semicolons inside
// quoted strings (including escaped quotes) as data. Parameter names and values
// are trimmed of OWS; segments without '=' are ignored. Views borrow the input.
template <typename Visitor>
inline void httpVisitSemicolonParametersQuotedField(
    std::string_view value, Visitor&& visitor) {
    std::size_t start = 0;
    while (start <= value.size()) {
        bool quoted = false;
        std::size_t end = start;
        for (; end < value.size(); ++end) {
            const char c = value[end];
            if (quoted) {
                if (c == '\\' && end + 1 < value.size()) {
                    ++end;
                } else if (c == '"') {
                    quoted = false;
                }
            } else if (c == '"') {
                quoted = true;
            } else if (c == ';') {
                break;
            }
        }
        const auto part = httpTrimOws(value.substr(start, end - start));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos &&
            !visitor(httpTrimOws(part.substr(0, equals)),
                httpTrimOws(part.substr(equals + 1)))) {
            return;
        }
        if (end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Removes a surrounding pair of DQUOTE bytes when present; does not unescape
// quoted-pairs. The returned view borrows the input.
[[nodiscard]] inline std::string_view httpTrimQuotedFieldValue(
    std::string_view value) noexcept {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

}  // namespace ruvia
