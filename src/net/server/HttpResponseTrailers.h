#pragma once

#include "../../http/HeaderTokenUtils.h"
#include "../../http/parser/HttpParserSyntax.h"

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

// A trailer value may carry any field-vchar / obs-text byte but never CR, LF, or
// NUL — forbidding those is what stops header/response splitting on the HTTP/1.1
// chunked-trailer path, where the value is written verbatim onto the wire.
[[nodiscard]] inline bool isValidResponseTrailerValue(std::string_view value) noexcept {
    for (const char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '\0' || c == '\r' || c == '\n') {
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
            return asciiEqualsIgnoreCase(name, "TE");
        case 3:
            // Response control data (RFC 9110 §7.4).
            return asciiEqualsIgnoreCase(name, "Age");
        case 4:
            return asciiEqualsIgnoreCase(name, "Date") ||
                asciiEqualsIgnoreCase(name, "Vary");
        case 6:
            return asciiEqualsIgnoreCase(name, "Pragma");
        case 7:
            return asciiEqualsIgnoreCase(name, "Trailer") ||
                asciiEqualsIgnoreCase(name, "Expires") ||
                asciiEqualsIgnoreCase(name, "Warning");
        case 8:
            return asciiEqualsIgnoreCase(name, "Location");
        case 10:
            return asciiEqualsIgnoreCase(name, "Keep-Alive") ||
                asciiEqualsIgnoreCase(name, "Set-Cookie");
        case 11:
            return asciiEqualsIgnoreCase(name, "Retry-After");
        case 12:
            return asciiEqualsIgnoreCase(name, "Max-Forwards");
        case 13:
            return asciiEqualsIgnoreCase(name, "Cache-Control") ||
                asciiEqualsIgnoreCase(name, "Accept-Ranges") ||
                asciiEqualsIgnoreCase(name, "Content-Range");
        case 18:
            return asciiEqualsIgnoreCase(name, "Proxy-Authenticate");
        case 19:
            return asciiEqualsIgnoreCase(name, "Proxy-Authorization");
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
