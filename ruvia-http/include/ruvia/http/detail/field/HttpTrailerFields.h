#pragma once

#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

// Trailer = #field-name (RFC 9110 section 6.6.2). The names listed here are the
// request-trailer fields this protocol layer will later reject if they appear in
// an actual trailer section, so a sender/recipient must not accept an initial
// Trailer header that advertises them.
[[nodiscard]] inline bool isForbiddenHttpRequestTrailerName(std::string_view name) noexcept {
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
        case RequestHeaderKind::kAccessControlRequestHeaders:
        case RequestHeaderKind::kAccessControlRequestMethod:
        case RequestHeaderKind::kOrigin:
            return true;
        case RequestHeaderKind::kOther:
        case RequestHeaderKind::kAccept:
        case RequestHeaderKind::kAcceptEncoding:
        case RequestHeaderKind::kUserAgent:
        case RequestHeaderKind::kSecWebSocketKey:
        case RequestHeaderKind::kSecWebSocketProtocol:
        case RequestHeaderKind::kSecWebSocketVersion:
            break;
    }

    switch (name.size()) {
        case 2:
            return httpAsciiEqualsIgnoreCase(name, "TE");
        case 7:
            return httpAsciiEqualsIgnoreCase(name, "Trailer");
        case 10:
            return httpAsciiEqualsIgnoreCase(name, "Keep-Alive") ||
                   httpAsciiEqualsIgnoreCase(name, "Set-Cookie");
        case 12:
            return httpAsciiEqualsIgnoreCase(name, "Max-Forwards");
        case 13:
            return httpAsciiEqualsIgnoreCase(name, "Cache-Control") ||
                   httpAsciiEqualsIgnoreCase(name, "Accept-Ranges") ||
                   httpAsciiEqualsIgnoreCase(name, "Content-Range");
        case 16:
            return httpAsciiEqualsIgnoreCase(name, "Content-Encoding") ||
                   httpAsciiEqualsIgnoreCase(name, "Proxy-Connection");
        case 18:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Authenticate");
        case 19:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Authorization");
        default:
            return false;
    }
}

template <typename ForbiddenName>
[[nodiscard]] inline bool isValidHttpTrailerFieldValue(
    std::string_view value, HttpFieldListRole role, ForbiddenName&& forbiddenName) noexcept {
    const bool emptyField = httpTrimOws(value).empty();
    bool valid = true;
    httpVisitCommaSeparatedQuotedItems(
        value, [&valid, emptyField, role, &forbiddenName](std::string_view item) noexcept {
            if (item.empty()) {
                // RFC 9110 section 5.6.1.1: senders must not generate empty list
                // elements, but `#field-name` itself can represent an empty list.
                if (role == HttpFieldListRole::kSender && !emptyField) {
                    valid = false;
                    return false;
                }
                return true;
            }
            if (!isValidHttpHeaderName(item) || forbiddenName(item)) {
                valid = false;
                return false;
            }
            return true;
        });
    return valid;
}

[[nodiscard]] inline bool isValidHttpRequestTrailerFieldValue(
    std::string_view value, HttpFieldListRole role) noexcept {
    return isValidHttpTrailerFieldValue(value, role,
        [](std::string_view name) noexcept { return isForbiddenHttpRequestTrailerName(name); });
}

}  // namespace ruvia::detail
