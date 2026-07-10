#pragma once

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

#include <string_view>

namespace ruvia::detail {

// A valid field name is a non-empty RFC 9110 token. isHttpTokenChar rejects ':',
// which also keeps HTTP/2 pseudo-headers out of a trailer section (RFC 9113
// §8.1). Reuses the parser's shared tchar table.
[[nodiscard]] inline bool isValidResponseTrailerName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const char ch : name) {
        if (!isHttpTokenChar(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

// A trailer value must be a valid HTTP field value (RFC 9110 §5.5): field-vchar
// (VCHAR / obs-text) plus HTAB, and nothing else. Rejecting NUL/CR/LF alone left
// the other control bytes (0x01-0x08, VT, FF, 0x0E-0x1F, DEL) able to reach the
// wire verbatim on the HTTP/1.1 chunked-trailer path, emitting a non-conformant
// field-value. Enforce the shared field-value rule so this matches the request
// trailer path and every other header-value check (isHttpFieldValueChar).
[[nodiscard]] inline bool isValidResponseTrailerValue(std::string_view value) noexcept {
    for (const char ch : value) {
        if (!isHttpFieldValueChar(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

// Fields that must never appear in a trailer section because they govern message
// framing, routing, authentication, response controls, or content format
// (RFC 9110 §6.5.1, RFC 9113 §8.1).
[[nodiscard]] inline bool isForbiddenResponseTrailerName(std::string_view name) noexcept {
    switch (classifyRequestHeader(name)) {
        case RequestHeaderKind::kHost:
        case RequestHeaderKind::kContentLength:
        case RequestHeaderKind::kTransferEncoding:
        case RequestHeaderKind::kConnection:
        case RequestHeaderKind::kContentEncoding:
        case RequestHeaderKind::kContentType:
        case RequestHeaderKind::kCookie:
        case RequestHeaderKind::kExpect:
        case RequestHeaderKind::kIfMatch:
        case RequestHeaderKind::kIfModifiedSince:
        case RequestHeaderKind::kIfNoneMatch:
        case RequestHeaderKind::kIfRange:
        case RequestHeaderKind::kIfUnmodifiedSince:
        case RequestHeaderKind::kRange:
        case RequestHeaderKind::kUpgrade:
        case RequestHeaderKind::kAuthorization:
            return true;
        case RequestHeaderKind::kOther:
        case RequestHeaderKind::kAccept:
        case RequestHeaderKind::kAcceptEncoding:
        case RequestHeaderKind::kAccessControlRequestHeaders:
        case RequestHeaderKind::kAccessControlRequestMethod:
        case RequestHeaderKind::kUserAgent:
        case RequestHeaderKind::kOrigin:
        case RequestHeaderKind::kSecWebSocketKey:
        case RequestHeaderKind::kSecWebSocketProtocol:
        case RequestHeaderKind::kSecWebSocketVersion:
            break;
    }

    switch (name.size()) {
        case 2:
            return httpAsciiEqualsIgnoreCase(name, "TE");
        case 3:
            // Response control data (RFC 9110 §7.4).
            return httpAsciiEqualsIgnoreCase(name, "Age");
        case 4:
            return httpAsciiEqualsIgnoreCase(name, "Date") ||
                httpAsciiEqualsIgnoreCase(name, "Vary");
        case 6:
            return httpAsciiEqualsIgnoreCase(name, "Pragma");
        case 7:
            return httpAsciiEqualsIgnoreCase(name, "Trailer") ||
                httpAsciiEqualsIgnoreCase(name, "Expires") ||
                httpAsciiEqualsIgnoreCase(name, "Warning");
        case 8:
            return httpAsciiEqualsIgnoreCase(name, "Location");
        case 10:
            return httpAsciiEqualsIgnoreCase(name, "Keep-Alive") ||
                httpAsciiEqualsIgnoreCase(name, "Set-Cookie");
        case 11:
            return httpAsciiEqualsIgnoreCase(name, "Retry-After");
        case 12:
            return httpAsciiEqualsIgnoreCase(name, "Max-Forwards");
        case 13:
            return httpAsciiEqualsIgnoreCase(name, "Cache-Control") ||
                httpAsciiEqualsIgnoreCase(name, "Accept-Ranges") ||
                httpAsciiEqualsIgnoreCase(name, "Content-Range");
        case 18:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Authenticate");
        case 19:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Authorization");
        default:
            return false;
    }
}

// True if (name, value) is an acceptable response trailer field. Shared by the
// HTTP/1.1 chunked-trailer and HTTP/2 trailing-HEADERS sinks so both transports
// enforce the same rules.
[[nodiscard]] inline bool responseTrailerFieldValid(std::string_view name, std::string_view value) noexcept {
    return isValidResponseTrailerName(name) &&
        !isForbiddenResponseTrailerName(name) &&
        isValidResponseTrailerValue(value);
}

}  // namespace ruvia::detail
