#include "ruvia/http/detail/parser/http_parser_syntax.h"

#include <limits>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/util/hex.h"
#include "ruvia/http/http_header.h"

namespace ruvia::detail {

request_header_kind classify_request_header(std::string_view name) noexcept {
    if (name.empty()) {
        return request_header_kind::other;
    }
    const auto first = http_ascii_to_lower(static_cast<unsigned char>(name.front()));
    switch (name.size()) {
        case 4:
            if (first == 'h' && http_ascii_equals_ignore_case(name, "Host")) {
                return request_header_kind::host;
            }
            break;
        case 5:
            if (first == 'r' && http_ascii_equals_ignore_case(name, "Range")) {
                return request_header_kind::range;
            }
            break;
        case 6:
            switch (first) {
                case 'a':
                    if (http_ascii_equals_ignore_case(name, "Accept")) {
                        return request_header_kind::accept;
                    }
                    break;
                case 'c':
                    if (http_ascii_equals_ignore_case(name, "Cookie")) {
                        return request_header_kind::cookie;
                    }
                    break;
                case 'e':
                    if (http_ascii_equals_ignore_case(name, "Expect")) {
                        return request_header_kind::expect;
                    }
                    break;
                case 'o':
                    if (http_ascii_equals_ignore_case(name, "Origin")) {
                        return request_header_kind::origin;
                    }
                    break;
                default:
                    break;
            }
            break;
        case 7:
            if (first == 'u' && http_ascii_equals_ignore_case(name, "Upgrade")) {
                return request_header_kind::upgrade;
            }
            break;
        case 9:
            if (first == 'f' && http_ascii_equals_ignore_case(name, "Forwarded")) {
                return request_header_kind::forwarded;
            }
            break;
        case 8:
            if (first == 'i') {
                if (http_ascii_equals_ignore_case(name, "If-Match")) {
                    return request_header_kind::if_match;
                }
                if (http_ascii_equals_ignore_case(name, "If-Range")) {
                    return request_header_kind::if_range;
                }
            }
            break;
        case 10:
            switch (first) {
                case 'c':
                    if (http_ascii_equals_ignore_case(name, "Connection")) {
                        return request_header_kind::connection;
                    }
                    break;
                case 'u':
                    if (http_ascii_equals_ignore_case(name, "User-Agent")) {
                        return request_header_kind::user_agent;
                    }
                    break;
                default:
                    break;
            }
            break;
        case 12:
            if (first == 'c' && http_ascii_equals_ignore_case(name, "Content-Type")) {
                return request_header_kind::content_type;
            }
            break;
        case 13:
            switch (first) {
                case 'a':
                    if (http_ascii_equals_ignore_case(name, "Authorization")) {
                        return request_header_kind::authorization;
                    }
                    break;
                case 'i':
                    if (http_ascii_equals_ignore_case(name, "If-None-Match")) {
                        return request_header_kind::if_none_match;
                    }
                    break;
                default:
                    break;
            }
            break;
        case 14:
            if (first == 'c' && http_ascii_equals_ignore_case(name, "Content-Length")) {
                return request_header_kind::content_length;
            }
            break;
        case 15:
            if (first == 'a' && http_ascii_equals_ignore_case(name, "Accept-Encoding")) {
                return request_header_kind::accept_encoding;
            }
            if (first == 'x' && http_ascii_equals_ignore_case(name, "X-Forwarded-For")) {
                return request_header_kind::x_forwarded_for;
            }
            break;
        case 16:
            if (first == 'c' && http_ascii_equals_ignore_case(name, "Content-Encoding")) {
                return request_header_kind::content_encoding;
            }
            break;
        case 17:
            switch (first) {
                case 'i':
                    if (http_ascii_equals_ignore_case(name, "If-Modified-Since")) {
                        return request_header_kind::if_modified_since;
                    }
                    break;
                case 's':
                    if (http_ascii_equals_ignore_case(name, "Sec-WebSocket-Key")) {
                        return request_header_kind::sec_websocket_key;
                    }
                    break;
                case 't':
                    if (http_ascii_equals_ignore_case(name, "Transfer-Encoding")) {
                        return request_header_kind::transfer_encoding;
                    }
                    break;
                case 'x':
                    if (http_ascii_equals_ignore_case(name, "X-Forwarded-Proto")) {
                        return request_header_kind::x_forwarded_proto;
                    }
                    break;
                default:
                    break;
            }
            break;
        case 19:
            if (first == 'i' && http_ascii_equals_ignore_case(name, "If-Unmodified-Since")) {
                return request_header_kind::if_unmodified_since;
            }
            break;
        case 21:
            if (first == 's' && http_ascii_equals_ignore_case(name, "Sec-WebSocket-Version")) {
                return request_header_kind::sec_websocket_version;
            }
            break;
        case 22:
            if (first == 's' && http_ascii_equals_ignore_case(name, "Sec-WebSocket-Protocol")) {
                return request_header_kind::sec_websocket_protocol;
            }
            break;
        case 24:
            if (first == 's' && http_ascii_equals_ignore_case(name, "Sec-WebSocket-Extensions")) {
                return request_header_kind::sec_websocket_extensions;
            }
            break;
        case 29:
            if (first == 'a' && http_ascii_equals_ignore_case(name, "Access-Control-Request-Method")) {
                return request_header_kind::access_control_request_method;
            }
            break;
        case 30:
            if (first == 'a' && http_ascii_equals_ignore_case(name, "Access-Control-Request-Headers")) {
                return request_header_kind::access_control_request_headers;
            }
            break;
        default:
            break;
    }
    return request_header_kind::other;
}

bool is_valid_http_header_name(std::string_view name) noexcept {
    return ruvia::is_valid_http_header_name(name);
}

bool is_valid_http_header_value(std::string_view value) noexcept {
    return ruvia::is_valid_http_header_value(value);
}

bool is_valid_http_chunk_extension(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }

    std::size_t cursor_value = 0;
    const auto skip_bws = [&value, &cursor_value]() noexcept {
        while (cursor_value < value.size() && (value[cursor_value] == ' ' || value[cursor_value] == '\t')) {
            ++cursor_value;
        }
    };
    const auto parse_token = [&value, &cursor_value]() noexcept {
        const auto begin = cursor_value;
        while (
            cursor_value < value.size() && is_http_token_char(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        return cursor_value != begin;
    };

    while (cursor_value < value.size()) {
        skip_bws();
        if (cursor_value == value.size()) {
            // Reached end-of-line after consuming whitespace with no ";": the
            // chunk-ext grammar (RFC 9112 7.1) permits BWS only before ";"/"=",
            // never as trailing space before CRLF. Accepting "5 " is a
            // request-smuggling differential of the same class as the leading-OWS
            // case that parse_http_chunk_size_line already rejects.
            return false;
        }
        if (value[cursor_value] != ';') {
            return false;
        }
        ++cursor_value;
        skip_bws();
        if (!parse_token()) {
            return false;
        }
        const auto after_name = cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            // Bare ext-name at end-of-line: valid only with no trailing BWS
            // between the name and the CRLF (RFC 9112 7.1).
            return cursor_value == after_name;
        }
        if (value[cursor_value] == ';') {
            continue;
        }
        if (value[cursor_value] != '=') {
            return false;
        }
        ++cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            return false;
        }
        if (value[cursor_value] == '"') {
            ++cursor_value;
            bool closed = false;
            while (cursor_value < value.size()) {
                const auto c = static_cast<unsigned char>(value[cursor_value]);
                if (c == '"') {
                    ++cursor_value;
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    ++cursor_value;
                    if (cursor_value == value.size()) {
                        return false;
                    }
                    const auto escaped = static_cast<unsigned char>(value[cursor_value]);
                    if (escaped == 0x7F || (escaped < 0x20 && escaped != '\t')) {
                        return false;
                    }
                    ++cursor_value;
                    continue;
                }
                if (c == 0x7F || (c < 0x20 && c != '\t')) {
                    return false;
                }
                ++cursor_value;
            }
            if (!closed) {
                return false;
            }
        } else if (!parse_token()) {
            return false;
        }
        const auto before_trailing_bws = cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            // End-of-line after a chunk-ext value is valid only if it lands
            // exactly on the value; whitespace between the value and CRLF is
            // rejected for the same reason as trailing space after the size.
            return cursor_value == before_trailing_bws;
        }
        if (value[cursor_value] != ';') {
            return false;
        }
    }
    return true;
}

chunk_size_line_status parse_http_chunk_size_line(std::string_view value, std::size_t& size) noexcept {
    // RFC 9112: chunk-size starts at the first byte of the line. Leading OWS
    // before the size is a known request-smuggling vector and is rejected,
    // matching picohttpparser/llhttp strict parsing.
    std::size_t cursor_value = 0;
    std::size_t parsed_value = 0;
    constexpr auto max_before_shift = std::numeric_limits<std::size_t>::max() >> 4U;
    while (cursor_value < value.size()) {
        const int nibble = decode_hex_nibble(value[cursor_value]);
        if (nibble < 0) {
            break;
        }
        if (parsed_value > max_before_shift) {
            return chunk_size_line_status::overflow;
        }
        parsed_value = (parsed_value << 4U) | static_cast<std::size_t>(nibble);
        ++cursor_value;
    }
    if (cursor_value == 0) {
        return chunk_size_line_status::invalid_size;
    }
    if (!is_valid_http_chunk_extension(value.substr(cursor_value))) {
        return chunk_size_line_status::invalid_extension;
    }

    size = parsed_value;
    return chunk_size_line_status::ok;
}

}  // namespace ruvia::detail
