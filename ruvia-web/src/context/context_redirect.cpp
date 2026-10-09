#include <stdexcept>
#include <string_view>

#include "ruvia/core/hex.h"
#include "ruvia/web/context.h"

// A redirect target the application supplies may contain bytes that are not
// legal in a URI-reference carried by Location. Percent-encode exactly those,
// leaving an already valid URI -- including its existing percent-escapes -- byte
// for byte intact, so a caller that encoded correctly is never double-encoded.

namespace ruvia {
namespace {

struct redirect_authority_span {
    std::size_t begin_ = 0;
    std::size_t end_ = 0;
    bool present_ = false;
};

[[nodiscard]] bool is_valid_percent_escape(std::string_view value, std::size_t index) noexcept {
    return index + 2 < value.size() && decode_hex_nibble(value[index + 1]) >= 0 &&
           decode_hex_nibble(value[index + 2]) >= 0;
}

[[nodiscard]] bool is_uri_scheme_first(unsigned char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

[[nodiscard]] bool is_uri_scheme_rest(unsigned char ch) noexcept {
    return is_uri_scheme_first(ch) || (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.';
}

[[nodiscard]] std::size_t find_redirect_authority_end(
    std::string_view location, std::size_t begin) noexcept {
    for (std::size_t i = begin; i < location.size(); ++i) {
        switch (location[i]) {
            case '/':
            case '?':
            case '#':
                return i;
            default:
                break;
        }
    }
    return location.size();
}

[[nodiscard]] redirect_authority_span find_redirect_authority(std::string_view location) noexcept {
    if (location.size() >= 2 && location[0] == '/' && location[1] == '/') {
        return redirect_authority_span{2, find_redirect_authority_end(location, 2), true};
    }
    if (location.empty() || !is_uri_scheme_first(static_cast<unsigned char>(location.front()))) {
        return {};
    }
    for (std::size_t i = 1; i < location.size(); ++i) {
        const auto ch = static_cast<unsigned char>(location[i]);
        if (ch == ':') {
            const auto begin = i + 3;
            if (begin <= location.size() && location[i + 1] == '/' && location[i + 2] == '/') {
                return redirect_authority_span{
                    begin, find_redirect_authority_end(location, begin), true};
            }
            return {};
        }
        if (ch == '/' || ch == '?' || ch == '#' || !is_uri_scheme_rest(ch)) {
            return {};
        }
    }
    return {};
}

[[nodiscard]] bool is_ip_literal_bracket_delimiter(
    std::string_view location, std::size_t index, redirect_authority_span authority) noexcept {
    if (!authority.present_ || index < authority.begin_ || index >= authority.end_) {
        return false;
    }

    std::size_t host_begin = authority.begin_;
    for (std::size_t i = authority.begin_; i < authority.end_; ++i) {
        if (location[i] == '@') {
            host_begin = i + 1;
        }
    }
    if (host_begin >= authority.end_ || location[host_begin] != '[') {
        return false;
    }

    for (std::size_t i = host_begin + 1; i < authority.end_; ++i) {
        if (location[i] == ']') {
            return index == host_begin || index == i;
        }
    }
    return false;
}

[[nodiscard]] bool redirect_location_contains_line_break(std::string_view location) noexcept {
    for (const char ch : location) {
        if (ch == '\r' || ch == '\n') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool encode_uri_keeps_byte(unsigned char ch, std::string_view location,
    std::size_t index, redirect_authority_span authority) noexcept;

[[nodiscard]] bool redirect_location_needs_encoding(std::string_view location) noexcept {
    const auto authority = find_redirect_authority(location);
    for (std::size_t i = 0; i < location.size(); ++i) {
        const auto ch = static_cast<unsigned char>(location[i]);
        if (ch == '%') {
            if (!is_valid_percent_escape(location, i)) {
                return true;
            }
            i += 2;
            continue;
        }
        if (!encode_uri_keeps_byte(ch, location, i, authority)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool encode_uri_keeps_byte(unsigned char ch, std::string_view location,
    std::size_t index, redirect_authority_span authority) noexcept {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
        return true;
    }

    if (ch == '[' || ch == ']') {
        return is_ip_literal_bracket_delimiter(location, index, authority);
    }

    switch (ch) {
        case ';':
        case ',':
        case '/':
        case '?':
        case ':':
        case '@':
        case '&':
        case '=':
        case '+':
        case '$':
        case '-':
        case '_':
        case '.':
        case '!':
        case '~':
        case '*':
        case '\'':
        case '(':
        case ')':
        case '#':
            return true;
        default:
            return false;
    }
}

void append_percent_encoded_byte(std::pmr::string& output, unsigned char ch) {
    output.push_back('%');
    output.push_back(upper_hex_digit(ch >> 4));
    output.push_back(upper_hex_digit(ch & 0x0F));
}

[[nodiscard]] std::pmr::string encode_redirect_location(
    std::string_view location, std::pmr::memory_resource* resource) {
    std::pmr::string encoded(resource);
    encoded.reserve(location.size());
    const auto authority = find_redirect_authority(location);
    for (std::size_t i = 0; i < location.size(); ++i) {
        const auto ch = static_cast<unsigned char>(location[i]);
        // Pass an already well-formed percent-escape (%HH) through verbatim. The
        // encoder rewrites the whole string when any byte needs escaping -- so
        // without this, a location that is already percent-encoded elsewhere
        // (e.g. "%20") would have its '%' re-encoded to "%25", double-encoding
        // it to "%2520" and corrupting the redirect target.
        // RFC 3986 2.4 forbids encoding the same string more than once; the caller
        // means a valid target, not a literal percent. A lone or malformed '%' is
        // not a valid escape and is percent-encoded like any other octet below.
        if (ch == '%' && is_valid_percent_escape(location, i)) {
            encoded.push_back('%');
            encoded.push_back(location[i + 1]);
            encoded.push_back(location[i + 2]);
            i += 2;
            continue;
        }
        if (encode_uri_keeps_byte(ch, location, i, authority)) {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        append_percent_encoded_byte(encoded, ch);
    }
    return encoded;
}

[[nodiscard]] bool is_redirect_status(http_status_code status_code) noexcept {
    return status_code == http_status::moved_permanently || status_code == http_status::found ||
           status_code == http_status::see_other || status_code == http_status::temporary_redirect ||
           status_code == http_status::permanent_redirect;
}

}  // namespace

http_response context::redirect(redirect_response_options options) const {
    const auto location = options.location_.view();
    const auto status_code = options.status_;
    if (!is_redirect_status(status_code)) {
        throw std::invalid_argument("redirect status must be 301, 302, 303, 307, or 308");
    }
    if (redirect_location_contains_line_break(location)) {
        throw std::invalid_argument("redirect location must not contain CR or LF");
    }
    http_response response({.resource_ = arena()});
    apply_response_state(response, status_code);
    if (redirect_location_needs_encoding(location)) {
        auto encoded_location = encode_redirect_location(location, pool());
        response.header("Location", encoded_location);
    } else {
        response.header("Location", location);
    }
    return response;
}

}  // namespace ruvia
