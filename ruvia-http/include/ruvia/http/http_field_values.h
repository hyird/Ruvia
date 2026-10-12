#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_field_whitespace.h"

namespace ruvia {

// Index of the next `delimiter` at or after `start` that is outside an RFC 9110
// section 5.6.4 quoted-string, or value.size() when there is none. A quoted-pair
// (`\"`) does not end the string. This is the only quote-aware delimiter scan;
// every list and parameter visitor below is built on it.
[[nodiscard]] constexpr std::size_t http_find_unquoted_delimiter(
    std::string_view value, std::size_t start, char delimiter) noexcept {
    if (start >= value.size()) {
        return value.size();
    }
    bool quoted = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (quoted) {
            if (ch == '\\' && index + 1 < value.size()) {
                ++index;
            } else if (ch == '"') {
                quoted = false;
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == delimiter && index >= start) {
            return index;
        }
    }
    return value.size();
}

namespace detail {

// Splits one OWS-trimmed `name=value` segment on its first '=' and hands the
// OWS-trimmed pair to the visitor. A segment without '=' is not a parameter and
// is skipped. Returns the visitor's keep-going result.
template <typename visitor_type>
[[nodiscard]] inline bool http_emit_field_parameter(std::string_view part, visitor_type& visitor) {
    const auto equals = part.find('=');
    if (equals == std::string_view::npos) {
        return true;
    }
    return visitor(http_trim_ows(part.substr(0, equals)), http_trim_ows(part.substr(equals + 1)));
}

}  // namespace detail

// Visits comma-separated field members, treating commas inside quoted strings
// (including escaped quotes) as data. Members are OWS-trimmed and empty members
// are reported; framing-sensitive callers can reject them. Returning false from
// the visitor stops traversal. Views borrow the input field.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted_field_items(
    std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    for (;;) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ',');
        if (!visitor(http_trim_ows(value.substr(start, end - start))) || end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Visits semicolon-separated field parameters while treating semicolons inside
// quoted strings (including escaped quotes) as data. Parameter names and values
// are trimmed of OWS; segments without '=' are ignored. Values keep their
// quotes. Do not use for Cookie: RFC 6265 gives '"' no special meaning there.
// Views borrow the input.
template <typename visitor_type>
inline void http_visit_semicolon_parameters_quoted_field(
    std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    for (;;) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ';');
        if (!detail::http_emit_field_parameter(http_trim_ows(value.substr(start, end - start)), visitor) ||
            end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Removes a surrounding pair of DQUOTE bytes when present; does not unescape
// quoted-pairs. The returned view borrows the input.
[[nodiscard]] constexpr std::string_view http_trim_quoted_field_value(borrowed_text text) noexcept {
    auto value = text.view();
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

// Allocation-free RFC 9110 section 5.6.4 quoted-string reader. The input starts
// at the opening DQUOTE; next() validates and decodes one content byte at a time
// (qdtext or the byte of a quoted-pair: HTAB, SP, VCHAR, obs-text) and returns
// false at the closing DQUOTE or on malformed input. closed() tells the two
// apart; consumed() then counts the bytes read, both quotes included. Bytes after
// the closing DQUOTE are never read.
class http_quoted_string_cursor final {
public:
    explicit constexpr http_quoted_string_cursor(std::string_view input) noexcept
        : input_(input),
          state_(!input.empty() && input.front() == '"' ? state_type::open : state_type::failed) {}

    [[nodiscard]] constexpr bool next(unsigned char& out) noexcept {
        if (state_ != state_type::open) {
            return false;
        }
        if (position_ == input_.size()) {
            state_ = state_type::failed;
            return false;
        }
        auto byte = static_cast<unsigned char>(input_[position_++]);
        if (byte == '"') {
            state_ = state_type::closed;
            return false;
        }
        if (byte == '\\') {
            if (position_ == input_.size()) {
                state_ = state_type::failed;
                return false;
            }
            byte = static_cast<unsigned char>(input_[position_++]);
        }
        if (!is_http_field_value_char(byte)) {
            state_ = state_type::failed;
            return false;
        }
        out = byte;
        return true;
    }

    [[nodiscard]] constexpr bool closed() const noexcept {
        return state_ == state_type::closed;
    }

    [[nodiscard]] constexpr std::size_t consumed() const noexcept {
        return position_;
    }

private:
    enum class state_type : unsigned char { open,
        closed,
        failed };

    std::string_view input_;
    std::size_t position_{1};
    state_type state_;
};

// Byte length of the well-formed quoted-string that starts `input`, quotes
// included, or 0 when `input` does not start with one.
[[nodiscard]] constexpr std::size_t http_quoted_string_size(std::string_view input) noexcept {
    http_quoted_string_cursor cursor(input);
    unsigned char byte = 0;
    while (cursor.next(byte)) {
    }
    return cursor.closed() ? cursor.consumed() : 0;
}

// True when the whole of `value` is exactly one well-formed quoted-string.
[[nodiscard]] constexpr bool http_is_quoted_string(std::string_view value) noexcept {
    return !value.empty() && http_quoted_string_size(value) == value.size();
}

}  // namespace ruvia
