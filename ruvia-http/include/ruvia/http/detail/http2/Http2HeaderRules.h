#pragma once

#include <string_view>

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

[[nodiscard]] inline bool http2IsForbiddenTrailerHeader(std::string_view name) noexcept {
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
            return true;
        case RequestHeaderKind::kOther:
        case RequestHeaderKind::kAccept:
        case RequestHeaderKind::kAcceptEncoding:
        case RequestHeaderKind::kAccessControlRequestHeaders:
        case RequestHeaderKind::kAccessControlRequestMethod:
        case RequestHeaderKind::kTransferEncoding:
        case RequestHeaderKind::kUpgrade:
        case RequestHeaderKind::kUserAgent:
        case RequestHeaderKind::kOrigin:
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
