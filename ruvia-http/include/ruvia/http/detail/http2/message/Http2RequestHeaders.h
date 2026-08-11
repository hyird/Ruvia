#pragma once

#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/coding/HttpContentLength.h"
#include "ruvia/http/detail/field/HttpMediaType.h"
#include "ruvia/http/detail/field/HttpCorsFields.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/field/HttpHeaderSectionSize.h"
#include "ruvia/http/detail/http2/message/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/stream/Http2StreamState.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
namespace ruvia::detail {

struct Http2HeaderDecodeContext final {
    explicit Http2HeaderDecodeContext(Http2StreamState& streamValue, Http2StreamHeaderDecodeTransaction* transactionValue = nullptr) noexcept
        : stream(streamValue),
          transaction(transactionValue) {}

    void commitHeaderDecode() noexcept {
        if (transaction != nullptr) {
            transaction->commit();
        }
    }

    [[nodiscard]] bool acceptRegularField() noexcept {
        if (regularFieldCount == kMaxHttpHeaderFields) {
            return false;
        }
        ++regularFieldCount;
        return true;
    }

    Http2StreamState& stream;
    Http2StreamHeaderDecodeTransaction* transaction{nullptr};
    HttpHeaderSectionSize decodedHeaderListSize;
    std::size_t regularFieldCount{0};
};

[[nodiscard]] inline bool http2IsHttpRequestScheme(std::string_view scheme) noexcept {
    return httpAsciiEqualsIgnoreCase(scheme, "http") || httpAsciiEqualsIgnoreCase(scheme, "https");
}

// RFC 9110 defines both http-URI and https-URI with a mandatory authority.
// Asterisk-form OPTIONS is server-wide: the request target itself contains no
// authority information (RFC 9113 section 8.3.1). A direct HTTP/2 sender may
// still convey the target URI authority separately via :authority; when present,
// callers validate it with http2IsValidRequestAuthority().
[[nodiscard]] inline bool http2RegularRequestRequiresAuthority(std::string_view scheme, std::string_view path) noexcept {
    return path != "*" && http2IsHttpRequestScheme(scheme);
}

[[nodiscard]] inline bool http2IsValidRegularRequestPath(HttpKnownMethod method, std::string_view scheme, std::string_view path) noexcept {
    if (path.empty()) {
        return !http2IsHttpRequestScheme(scheme);
    }
    return isValidOriginOrAsteriskFormTarget(method, path);
}

[[nodiscard]] inline bool http2IsValidExtendedConnectPath(std::string_view scheme, std::string_view path) noexcept {
    if (path.empty()) {
        return !http2IsHttpRequestScheme(scheme);
    }
    return isValidOriginFormTarget(path);
}

[[nodiscard]] inline bool http2IsValidRequestAuthority(std::string_view scheme, std::string_view authority) noexcept {
    if (http2IsHttpRequestScheme(scheme)) {
        // HTTP(S) URI authority is mandatory even though an empty Host field is
        // valid HTTP/1 wire syntax for target URIs of other schemes.
        return !authority.empty() && isValidHostHeader(authority);
    }
    return isValidUriAuthority(authority);
}

[[nodiscard]] inline bool http2AccumulateHeaderListBytes(Http2HeaderDecodeContext& context, std::string_view name, std::string_view value) noexcept {
    return context.decodedHeaderListSize.add(name, value);
}

[[nodiscard]] inline bool http2AppendCookieHeaderValue(Http2StreamState& stream, std::string_view value) {
    if (!stream.appendRequestCookieHeaderValue(value, stream.hasCookie())) {
        return false;
    }
    stream.markCookie();
    return true;
}

[[nodiscard]] inline bool http2OnDecodedInitialHeader(Http2HeaderDecodeContext& context, std::string_view name, std::string_view value) {
    if (!http2AccumulateHeaderListBytes(context, name, value)) {
        return false;
    }

    auto& stream = context.stream;
    if (name.empty()) {
        return false;
    }

    if (name.front() == ':') {
        if (stream.regularHeaderSeen()) {
            return false;
        }
        if (name == ":method") {
            if (stream.hasMethod() || !isValidHttpMethodToken(value)) {
                return false;
            }
            stream.assignRequestMethod(value);
            return true;
        }
        if (name == ":protocol") {
            if (stream.hasProtocol() || !isValidHttpHeaderName(value)) {
                return false;
            }
            stream.setProtocol(value);
            return true;
        }
        if (name == ":scheme") {
            if (stream.hasScheme() || !isValidUriScheme(value)) {
                return false;
            }
            stream.assignRequestScheme(value);
            stream.markScheme(httpUriSchemeDefaultPort(value));
            return true;
        }
        if (name == ":authority") {
            if (stream.hasAuthority() || !isValidUriAuthority(value)) {
                return false;
            }
            stream.assignRequestAuthority(value);
            stream.markAuthority();
            return true;
        }
        if (name == ":path") {
            if (stream.hasPath() || (!value.empty() && !isValidOriginOrAsteriskFormTarget(value))) {
                return false;
            }
            stream.assignRequestPath(value);
            stream.markPath();
            return true;
        }
        return false;
    }

    if (!context.acceptRegularField() || !http2IsValidRegularHeader(name, value)) {
        return false;
    }
    stream.markRegularHeaderSeen();
    const auto kind = classifyRequestHeader(name);
    if ((kind == RequestHeaderKind::kOrigin && !isValidHttpOriginFieldValue(value)) || (kind == RequestHeaderKind::kAccessControlRequestMethod && !isValidHttpCorsRequestMethod(value)) || (kind == RequestHeaderKind::kAccessControlRequestHeaders && !isValidHttpCorsRequestHeaderNames(value))) {
        return false;
    }
    if (kind == RequestHeaderKind::kHost) {
        if (stream.hasHost() || !isValidHostHeader(value)) {
            return false;
        }
        if (stream.hasAuthority() && !authorityMatchesHost(stream.requestAuthority(), value, stream.schemeDefaultPort())) {
            return false;
        }
        stream.markHost();
    }
    if (kind == RequestHeaderKind::kCookie) {
        return http2AppendCookieHeaderValue(stream, value);
    }
    if (kind == RequestHeaderKind::kExpect) {
        if (!isValidReceivedHttpExpectFieldValue(value)) {
            return false;
        }
        // Expect is an extensible semantic list. Preserve unsupported but
        // syntactically valid members for the Web product's 417 policy.
        stream.parseRequestExpectationField(value);
    }
    if (kind == RequestHeaderKind::kContentType) {
        if (!isValidHttpContentTypeFieldValue(value)) {
            return false;
        }
    }
    if (kind == RequestHeaderKind::kContentEncoding && !isValidHttpContentEncodingFieldValue(value, HttpFieldListRole::kRecipient)) {
        return false;
    }
    if (name == "trailer" && !isValidHttpRequestTrailerFieldValue(value, HttpFieldListRole::kRecipient)) {
        return false;
    }
    if (const auto singletonBit = singletonRequestHeaderBit(kind); singletonBit != 0 && kind != RequestHeaderKind::kContentLength) {
        if (!stream.markSingletonRequestHeader(singletonBit)) {
            return false;
        }
    }
    if (kind == RequestHeaderKind::kContentLength) {
        HttpContentLengthState contentLength;
        if (contentLength.parseField(value) != HttpContentLengthParseStatus::kOk) {
            return false;
        }
        if (!stream.declareRemoteContentLength(*contentLength.value())) {
            return false;
        }
    }
    return stream.appendRemoteHeader(name, value, kind);
}

[[nodiscard]] inline bool http2OnDecodedRequestTrailer(Http2HeaderDecodeContext& context, std::string_view name, std::string_view value) {
    if (!http2AccumulateHeaderListBytes(context, name, value)) {
        return false;
    }

    return context.acceptRegularField() && http2IsValidRegularHeader(name, value) && !http2IsForbiddenRequestTrailerHeader(name);
}

}  // namespace ruvia::detail
