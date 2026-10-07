#pragma once

#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

// Shared request/response restrictions (RFC 9110 section 6.5.1): fields that
// control framing, routing, authentication or representation interpretation.
// Direction-specific permissions are composed below and by response policy.
[[nodiscard]] inline bool is_forbidden_common_trailer_name(
    std::string_view name, RequestHeaderKind kind) noexcept {
    switch (kind) {
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
        case RequestHeaderKind::kOrigin:
        case RequestHeaderKind::kUserAgent:
        case RequestHeaderKind::kSecWebSocketKey:
        case RequestHeaderKind::kSecWebSocketProtocol:
        case RequestHeaderKind::kSecWebSocketVersion:
        case RequestHeaderKind::kForwarded:
        case RequestHeaderKind::kXForwardedFor:
        case RequestHeaderKind::kXForwardedProto:
        case RequestHeaderKind::kSecWebSocketExtensions:
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
                   httpAsciiEqualsIgnoreCase(name, "Content-Range");
        case 16:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Connection");
        case 18:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Authenticate");
        case 19:
            return httpAsciiEqualsIgnoreCase(name, "Proxy-Authorization");
        default:
            return false;
    }
}

// Trailer advertisements and actual request sections use the same policy.
// Accept-Ranges is permitted in response trailers, not request trailers.
[[nodiscard]] inline bool isForbiddenHttpRequestTrailerName(std::string_view name) noexcept {
    const auto kind = classifyRequestHeader(name);
    if (is_forbidden_common_trailer_name(name, kind)) {
        return true;
    }
    return kind == RequestHeaderKind::kAccessControlRequestHeaders ||
           kind == RequestHeaderKind::kAccessControlRequestMethod ||
           kind == RequestHeaderKind::kOrigin ||
           httpAsciiEqualsIgnoreCase(name, "Accept-Ranges");
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
