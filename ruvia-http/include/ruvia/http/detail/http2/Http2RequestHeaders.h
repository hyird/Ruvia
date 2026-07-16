#pragma once

#include <charconv>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/HttpCorsFields.h"
#include "ruvia/http/detail/http2/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/Http2StreamState.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {

struct Http2HeaderDecodeContext final {
    Http2StreamState& stream;
    std::size_t decodedHeaderListBytes{0};
};

[[nodiscard]] inline bool http2AccumulateHeaderListBytes(
    Http2HeaderDecodeContext& context,
    std::string_view name,
    std::string_view value) noexcept {
    constexpr std::size_t kHeaderListEntryOverhead = 32;

    if (name.size() > kMaxHttpHeaderBytes ||
        value.size() > kMaxHttpHeaderBytes ||
        name.size() > kMaxHttpHeaderBytes - value.size()) {
        return false;
    }

    auto fieldBytes = name.size() + value.size();
    if (fieldBytes > kMaxHttpHeaderBytes - kHeaderListEntryOverhead) {
        return false;
    }
    fieldBytes += kHeaderListEntryOverhead;

    if (context.decodedHeaderListBytes > kMaxHttpHeaderBytes ||
        fieldBytes > kMaxHttpHeaderBytes - context.decodedHeaderListBytes) {
        return false;
    }

    context.decodedHeaderListBytes += fieldBytes;
    return true;
}

[[nodiscard]] inline bool http2AppendCookieHeaderValue(
    Http2StreamState& stream,
    std::string_view value) {
    if (!stream.appendRequestCookieHeaderValue(value, stream.hasCookie())) {
        return false;
    }
    stream.markCookie();
    return true;
}

[[nodiscard]] inline bool http2OnDecodedInitialHeader(
    Http2HeaderDecodeContext& context,
    std::string_view name,
    std::string_view value) {
    if (!http2AccumulateHeaderListBytes(context, name, value)) {
        return false;
    }

    auto& stream = context.stream;
    if (name.empty() || stream.requestHeadersFull()) {
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
            if (stream.hasScheme() || (value != "http" && value != "https")) {
                return false;
            }
            stream.markScheme(value == "https" ? 443 : 80);
            return true;
        }
        if (name == ":authority") {
            if (stream.hasAuthority() || !isValidHostHeader(value)) {
                return false;
            }
            stream.assignRequestAuthority(value);
            stream.markAuthority();
            return true;
        }
        if (name == ":path") {
            if (stream.hasPath() || !isValidOriginFormTarget(value)) {
                return false;
            }
            stream.assignRequestPath(value);
            stream.markPath();
            return true;
        }
        return false;
    }

    if (!http2IsValidRegularHeader(name, value)) {
        return false;
    }
    stream.markRegularHeaderSeen();
    const auto kind = classifyRequestHeader(name);
    if ((kind == RequestHeaderKind::kOrigin &&
         !isValidHttpOriginFieldValue(value)) ||
        (kind == RequestHeaderKind::kAccessControlRequestMethod &&
         !isValidHttpCorsRequestMethod(value)) ||
        (kind == RequestHeaderKind::kAccessControlRequestHeaders &&
         !isValidHttpCorsRequestHeaderNames(value))) {
        return false;
    }
    if (kind == RequestHeaderKind::kHost) {
        if (stream.hasHost() || !isValidHostHeader(value)) {
            return false;
        }
        if (stream.hasAuthority() &&
            !authorityMatchesHost(stream.requestAuthority(), value, stream.schemeDefaultPort())) {
            return false;
        }
        stream.markHost();
    }
    if (kind == RequestHeaderKind::kCookie) {
        return http2AppendCookieHeaderValue(stream, value);
    }
    if (kind == RequestHeaderKind::kExpect) {
        // Expect is an extensible semantic list, not an HTTP/2 field-block
        // validity condition. Preserve unsupported members for the Web product's
        // 417 policy while still accepting the conformant header section.
        stream.parseRequestExpectationField(value);
    }
    if (const auto singletonBit = singletonRequestHeaderBit(kind); singletonBit != 0) {
        if (!stream.markSingletonRequestHeader(singletonBit)) {
            return false;
        }
    }
    if (kind == RequestHeaderKind::kContentLength) {
        std::size_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
            return false;
        }
        if (!stream.declareRemoteContentLength(parsed)) {
            return false;
        }
    }
    return stream.appendRequestHeader(name, value, kind);
}

[[nodiscard]] inline bool http2OnDecodedTrailer(
    Http2HeaderDecodeContext& context,
    std::string_view name,
    std::string_view value) {
    if (!http2AccumulateHeaderListBytes(context, name, value)) {
        return false;
    }

    return http2IsValidRegularHeader(name, value) &&
        !http2IsForbiddenTrailerHeader(name);
}

}  // namespace ruvia::detail
