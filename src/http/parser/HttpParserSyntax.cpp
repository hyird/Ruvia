#include "HttpParserSyntax.h"

#include "ruvia/http/HeaderUtils.h"

#include <limits>

namespace ruvia::detail {

RequestHeaderKind classifyRequestHeader(std::string_view name) noexcept {
    switch (name.size()) {
        case 4:
            if (httpAsciiEqualsIgnoreCase(name, "Host")) {
                return RequestHeaderKind::kHost;
            }
            break;
        case 5:
            if (httpAsciiEqualsIgnoreCase(name, "Range")) {
                return RequestHeaderKind::kRange;
            }
            break;
        case 6:
            if (httpAsciiEqualsIgnoreCase(name, "Accept")) {
                return RequestHeaderKind::kAccept;
            }
            if (httpAsciiEqualsIgnoreCase(name, "Cookie")) {
                return RequestHeaderKind::kCookie;
            }
            if (httpAsciiEqualsIgnoreCase(name, "Expect")) {
                return RequestHeaderKind::kExpect;
            }
            if (httpAsciiEqualsIgnoreCase(name, "Origin")) {
                return RequestHeaderKind::kOrigin;
            }
            break;
        case 7:
            if (httpAsciiEqualsIgnoreCase(name, "Upgrade")) {
                return RequestHeaderKind::kUpgrade;
            }
            break;
        case 8:
            if (httpAsciiEqualsIgnoreCase(name, "If-Match")) {
                return RequestHeaderKind::kIfMatch;
            }
            if (httpAsciiEqualsIgnoreCase(name, "If-Range")) {
                return RequestHeaderKind::kIfRange;
            }
            break;
        case 10:
            if (httpAsciiEqualsIgnoreCase(name, "Connection")) {
                return RequestHeaderKind::kConnection;
            }
            if (httpAsciiEqualsIgnoreCase(name, "User-Agent")) {
                return RequestHeaderKind::kUserAgent;
            }
            break;
        case 12:
            if (httpAsciiEqualsIgnoreCase(name, "Content-Type")) {
                return RequestHeaderKind::kContentType;
            }
            break;
        case 13:
            if (httpAsciiEqualsIgnoreCase(name, "Authorization")) {
                return RequestHeaderKind::kAuthorization;
            }
            if (httpAsciiEqualsIgnoreCase(name, "If-None-Match")) {
                return RequestHeaderKind::kIfNoneMatch;
            }
            break;
        case 14:
            if (httpAsciiEqualsIgnoreCase(name, "Content-Length")) {
                return RequestHeaderKind::kContentLength;
            }
            break;
        case 15:
            if (httpAsciiEqualsIgnoreCase(name, "Accept-Encoding")) {
                return RequestHeaderKind::kAcceptEncoding;
            }
            break;
        case 17:
            if (httpAsciiEqualsIgnoreCase(name, "If-Modified-Since")) {
                return RequestHeaderKind::kIfModifiedSince;
            }
            if (httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Key")) {
                return RequestHeaderKind::kSecWebSocketKey;
            }
            if (httpAsciiEqualsIgnoreCase(name, "Transfer-Encoding")) {
                return RequestHeaderKind::kTransferEncoding;
            }
            break;
        case 19:
            if (httpAsciiEqualsIgnoreCase(name, "If-Unmodified-Since")) {
                return RequestHeaderKind::kIfUnmodifiedSince;
            }
            break;
        case 21:
            if (httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Version")) {
                return RequestHeaderKind::kSecWebSocketVersion;
            }
            break;
        case 22:
            if (httpAsciiEqualsIgnoreCase(name, "Sec-WebSocket-Protocol")) {
                return RequestHeaderKind::kSecWebSocketProtocol;
            }
            break;
        case 29:
            if (httpAsciiEqualsIgnoreCase(name, "Access-Control-Request-Method")) {
                return RequestHeaderKind::kAccessControlRequestMethod;
            }
            break;
        case 30:
            if (httpAsciiEqualsIgnoreCase(name, "Access-Control-Request-Headers")) {
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
            return true;
        }
        if (value[cursor] != ';') {
            return false;
        }
        ++cursor;
        skipBws();
        if (!parseToken()) {
            return false;
        }
        skipBws();
        if (cursor == value.size() || value[cursor] == ';') {
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
                    if (escaped == 0x7F || escaped < 0x20) {
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
        skipBws();
        if (cursor < value.size() && value[cursor] != ';') {
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
    while (cursor < value.size() && isHttpHexDigit(static_cast<unsigned char>(value[cursor]))) {
        if (parsed > maxBeforeShift) {
            return ChunkSizeLineStatus::kOverflow;
        }
        parsed = (parsed << 4U) | httpHexValue(static_cast<unsigned char>(value[cursor]));
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
