#pragma once

#include <charconv>
#include <string_view>

#include "Http2HeaderRules.h"
#include "Http2StreamState.h"
#include "../../http/parser/HttpRequestTarget.h"
#include "../../http/parser/HttpParserSyntax.h"
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
            if (stream.hasMethod()) {
                return false;
            }
            const auto method = parseMethod(value);
            if (method == HttpMethod::kUnknown) {
                return false;
            }
            stream.setRequestMethod(method);
            stream.markMethod();
            return true;
        }
        if (name == ":protocol") {
            if (stream.hasProtocol() || value.empty()) {
                return false;
            }
            stream.setProtocol(value == "websocket");
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
    if (kind == RequestHeaderKind::kContentLength) {
        std::size_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
            return false;
        }
        if (!stream.setContentLength(parsed)) {
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
