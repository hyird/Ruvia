#include "ruvia/http/detail/parser/HttpParserSyntax.h"

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/HttpHeader.h"

#include <limits>

namespace ruvia::detail {

RequestHeaderKind classifyRequestHeader(std::string_view name) noexcept {
    if (name.empty()) {
        return RequestHeaderKind::kOther;
    }
    const auto first = httpAsciiToLower(static_cast<unsigned char>(name.front()));
    switch (name.size()) {
        case 4:
            if (first == 'h' && httpAsciiEqualsIgnoreCase(name, "Host")) {
                return RequestHeaderKind::kHost;
            }
            break;
        case 5:
            if (first == 'r' && httpAsciiEqualsIgnoreCase(name, "Range")) {
                return RequestHeaderKind::kRange;
            }
            break;
        case 6:
            switch (first) {
                case 'a':
                    if (httpAsciiEqualsIgnoreCase(name, "Accept")) {
                        return RequestHeaderKind::kAccept;
                    }
                    break;
                case 'c':
                    if (httpAsciiEqualsIgnoreCase(name, "Cookie")) {
                        return RequestHeaderKind::kCookie;
                    }
                    break;
                case 'e':
                    if (httpAsciiEqualsIgnoreCase(name, "Expect")) {
                        return RequestHeaderKind::kExpect;
                    }
                    break;
                case 'o':
                    if (httpAsciiEqualsIgnoreCase(name, "Origin")) {
                        return RequestHeaderKind::kOrigin;
                    }
                    break;
                default:
                    break;
            }
            break;
        case 7:
            if (first == 'u' && httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
                return RequestHeaderKind::kUpgrade;
            }
            break;
        case 8:
            if (first == 'i') {
                if (httpAsciiEqualsIgnoreCase(name, "If-Match")) {
                    return RequestHeaderKind::kIfMatch;
                }
                if (httpAsciiEqualsIgnoreCase(name, "If-Range")) {
                    return RequestHeaderKind::kIfRange;
                }
            }
            break;
        case 10:
            switch (first) {
                case 'c':
                    if (httpAsciiEqualsIgnoreCase(name, "Connection")) {
                        return RequestHeaderKind::kConnection;
                    }
                    break;
                case 'u':
                    if (httpAsciiEqualsIgnoreCase(name, "User-Agent")) {
                        return RequestHeaderKind::kUserAgent;
                    }
                    break;
                default:
                    break;
            }
            break;
        case 12:
            if (first == 'c' && httpAsciiEqualsIgnoreCase(name, "Content-Type")) {
                return RequestHeaderKind::kContentType;
            }
            break;
        case 13:
            switch (first) {
                case 'a':
                    if (httpAsciiEqualsIgnoreCase(name, "Authorization")) {
                        return RequestHeaderKind::kAuthorization;
                    }
                    break;
                case 'i':
                    if (httpAsciiEqualsIgnoreCase(name, "If-None-Match")) {
                        return RequestHeaderKind::kIfNoneMatch;
                    }
                    break;
                default:
                    break;
            }
            break;
        case 14:
            if (first == 'c' && httpAsciiEqualsIgnoreCase(name, "Content-Length")) {
                return RequestHeaderKind::kContentLength;
            }
            break;
        case 15:
            if (first == 'a' && httpAsciiEqualsIgnoreCase(name, "Accept-Encoding")) {
                return RequestHeaderKind::kAcceptEncoding;
            }
            break;
        case 16:
            if (first == 'c' && httpAsciiEqualsIgnoreCase(name, "Content-Encoding")) {
                return RequestHeaderKind::kContentEncoding;
            }
            break;
        case 17:
            switch (first) {
                case 'i':
                    if (httpAsciiEqualsIgnoreCase(name, "If-Modified-Since")) {
                        return RequestHeaderKind::kIfModifiedSince;
                    }
                    break;
                case 's':
                    if (httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Key")) {
                        return RequestHeaderKind::kSecWebSocketKey;
                    }
                    break;
                case 't':
                    if (httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) {
                        return RequestHeaderKind::kTransferEncoding;
                    }
                    break;
                default:
                    break;
            }
            break;
        case 19:
            if (first == 'i' && httpAsciiEqualsIgnoreCase(name, "If-Unmodified-Since")) {
                return RequestHeaderKind::kIfUnmodifiedSince;
            }
            break;
        case 21:
            if (first == 's' && httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Version")) {
                return RequestHeaderKind::kSecWebSocketVersion;
            }
            break;
        case 22:
            if (first == 's' && httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Protocol")) {
                return RequestHeaderKind::kSecWebSocketProtocol;
            }
            break;
        case 29:
            if (first == 'a' && httpAsciiEqualsIgnoreCase(name, "Access-Control-Request-Method")) {
                return RequestHeaderKind::kAccessControlRequestMethod;
            }
            break;
        case 30:
            if (first == 'a' && httpAsciiEqualsIgnoreCase(name, "Access-Control-Request-Headers")) {
                return RequestHeaderKind::kAccessControlRequestHeaders;
            }
            break;
        default:
            break;
    }
    return RequestHeaderKind::kOther;
}

bool isValidHttpHeaderName(std::string_view name) noexcept {
    return ruvia::isValidHttpHeaderName(name);
}

bool isValidHttpHeaderValue(std::string_view value) noexcept {
    return ruvia::isValidHttpHeaderValue(value);
}

bool isValidHttpChunkExtension(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }

    std::size_t cursor = 0;
    const auto skipBws = [&value, &cursor]() noexcept {
        while (cursor < value.size() && (value[cursor] == ' ' || value[cursor] == '\t')) {
            ++cursor;
        }
    };
    const auto parseToken = [&value, &cursor]() noexcept {
        const auto begin = cursor;
        while (cursor < value.size() && isHttpTokenChar(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        return cursor != begin;
    };

    while (cursor < value.size()) {
        skipBws();
        if (cursor == value.size()) {
            // Reached end-of-line after consuming whitespace with no ";": the
            // chunk-ext grammar (RFC 9112 7.1) permits BWS only before ";"/"=",
            // never as trailing space before CRLF. Accepting "5 " is a
            // request-smuggling differential of the same class as the leading-OWS
            // case that parseHttpChunkSizeLine already rejects.
            return false;
        }
        if (value[cursor] != ';') {
            return false;
        }
        ++cursor;
        skipBws();
        if (!parseToken()) {
            return false;
        }
        const auto afterName = cursor;
        skipBws();
        if (cursor == value.size()) {
            // Bare ext-name at end-of-line: valid only with no trailing BWS
            // between the name and the CRLF (RFC 9112 7.1).
            return cursor == afterName;
        }
        if (value[cursor] == ';') {
            continue;
        }
        if (value[cursor] != '=') {
            return false;
        }
        ++cursor;
        skipBws();
        if (cursor == value.size()) {
            return false;
        }
        if (value[cursor] == '"') {
            ++cursor;
            bool closed = false;
            while (cursor < value.size()) {
                const auto c = static_cast<unsigned char>(value[cursor]);
                if (c == '"') {
                    ++cursor;
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    ++cursor;
                    if (cursor == value.size()) {
                        return false;
                    }
                    const auto escaped = static_cast<unsigned char>(value[cursor]);
                    if (escaped == 0x7F || (escaped < 0x20 && escaped != '\t')) {
                        return false;
                    }
                    ++cursor;
                    continue;
                }
                if (c == 0x7F || (c < 0x20 && c != '\t')) {
                    return false;
                }
                ++cursor;
            }
            if (!closed) {
                return false;
            }
        } else if (!parseToken()) {
            return false;
        }
        const auto beforeTrailingBws = cursor;
        skipBws();
        if (cursor == value.size()) {
            // End-of-line after a chunk-ext value is valid only if it lands
            // exactly on the value; whitespace between the value and CRLF is
            // rejected for the same reason as trailing space after the size.
            return cursor == beforeTrailingBws;
        }
        if (value[cursor] != ';') {
            return false;
        }
    }
    return true;
}

ChunkSizeLineStatus parseHttpChunkSizeLine(std::string_view value, std::size_t& size) noexcept {
    // RFC 9112: chunk-size starts at the first byte of the line. Leading OWS
    // before the size is a known request-smuggling vector and is rejected,
    // matching picohttpparser/llhttp strict parsing.
    std::size_t cursor = 0;
    std::size_t parsed = 0;
    constexpr auto maxBeforeShift = std::numeric_limits<std::size_t>::max() >> 4U;
    while (cursor < value.size()) {
        const int nibble = decodeHexNibble(value[cursor]);
        if (nibble < 0) {
            break;
        }
        if (parsed > maxBeforeShift) {
            return ChunkSizeLineStatus::kOverflow;
        }
        parsed = (parsed << 4U) | static_cast<std::size_t>(nibble);
        ++cursor;
    }
    if (cursor == 0) {
        return ChunkSizeLineStatus::kInvalidSize;
    }
    if (!isValidHttpChunkExtension(value.substr(cursor))) {
        return ChunkSizeLineStatus::kInvalidExtension;
    }

    size = parsed;
    return ChunkSizeLineStatus::kOk;
}

}  // namespace ruvia::detail
