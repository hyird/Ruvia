#pragma once

#include "ruvia/http/HttpHeader.h"

#include <string_view>

#include "ruvia/http/detail/AsciiCase.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http2HeaderNameHasUppercase(std::string_view name) noexcept {
    for (const auto ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool http2IsForbiddenConnectionHeader(std::string_view name) noexcept {
    return name == "connection" ||
        name == "keep-alive" ||
        name == "proxy-connection" ||
        name == "transfer-encoding" ||
        name == "upgrade";
}

// Application response fields are protocol-neutral and may retain their
// conventional HTTP/1 casing. Before encoding an HTTP/2 response, match the
// complete RFC 9113 section 8.2.2 forbidden set case-insensitively. The TE
// exception applies only to requests, so it is forbidden in responses too.
[[nodiscard]] inline bool http2IsForbiddenResponseConnectionField(
    std::string_view name) noexcept {
    return httpAsciiEqualsIgnoreCase(name, "connection") ||
        httpAsciiEqualsIgnoreCase(name, "keep-alive") ||
        httpAsciiEqualsIgnoreCase(name, "proxy-connection") ||
        httpAsciiEqualsIgnoreCase(name, "te") ||
        httpAsciiEqualsIgnoreCase(name, "transfer-encoding") ||
        httpAsciiEqualsIgnoreCase(name, "upgrade");
}

[[nodiscard]] inline bool http2IsValidRegularHeader(
    std::string_view name,
    std::string_view value) noexcept {
    if (name.empty() || name.front() == ':') {
        return false;
    }
    if (!isValidHttpHeaderName(name) || !isValidHttpHeaderValue(value)) {
        return false;
    }
    if (http2HeaderNameHasUppercase(name)) {
        return false;
    }
    if (http2IsForbiddenConnectionHeader(name)) {
        return false;
    }
    return name != "te" || value == "trailers";
}

// Decoded HTTP/2 field names are already required to be lowercase. The TE
// exception above belongs only to requests (RFC 9113 Section 8.2.2); a response
// carrying TE is malformed even when its value is exactly "trailers".
[[nodiscard]] inline bool http2IsValidDecodedResponseHeader(
    std::string_view name,
    std::string_view value) noexcept {
    return http2IsValidRegularHeader(name, value) &&
        !http2IsForbiddenResponseConnectionField(name);
}

[[nodiscard]] inline bool http2IsForbiddenRequestTrailerHeader(
    std::string_view name) noexcept {
    switch (classifyRequestHeader(name)) {
        case RequestHeaderKind::kHost:
        case RequestHeaderKind::kContentLength:
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
        case RequestHeaderKind::kAuthorization:
        case RequestHeaderKind::kAccessControlRequestHeaders:
        case RequestHeaderKind::kAccessControlRequestMethod:
        case RequestHeaderKind::kOrigin:
            return true;
        case RequestHeaderKind::kOther:
        case RequestHeaderKind::kAccept:
        case RequestHeaderKind::kAcceptEncoding:
        case RequestHeaderKind::kTransferEncoding:
        case RequestHeaderKind::kUpgrade:
        case RequestHeaderKind::kUserAgent:
        case RequestHeaderKind::kSecWebSocketKey:
        case RequestHeaderKind::kSecWebSocketProtocol:
        case RequestHeaderKind::kSecWebSocketVersion:
            break;
    }

    switch (name.size()) {
        case 2:
            return name == "te";
        case 7:
            return name == "trailer";
        case 10:
            return name == "keep-alive" || name == "set-cookie";
        case 12:
            return name == "max-forwards";
        case 13:
            return name == "cache-control" ||
                name == "accept-ranges" ||
                name == "content-range";
        case 18:
            return name == "proxy-authenticate";
        case 19:
            return name == "proxy-authorization";
        default:
            return false;
    }
}

}  // namespace ruvia::detail
