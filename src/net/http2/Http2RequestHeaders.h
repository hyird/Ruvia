#pragma once

#include <charconv>
#include <string_view>

#include "Http2StreamState.h"
#include "../../http/parser/HttpParserSyntax.h"
#include "../../http/HeaderTokenUtils.h"
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

[[nodiscard]] inline bool http2IsForbiddenUpgradedRequestHeader(std::string_view name) noexcept {
    return httpAsciiEqualsIgnoreCase(name, "connection") ||
        httpAsciiEqualsIgnoreCase(name, "upgrade") ||
        httpAsciiEqualsIgnoreCase(name, "http2-settings") ||
        httpAsciiEqualsIgnoreCase(name, "keep-alive") ||
        httpAsciiEqualsIgnoreCase(name, "proxy-connection") ||
        httpAsciiEqualsIgnoreCase(name, "transfer-encoding");
}

[[nodiscard]] inline bool http2IsValidRegularRequestHeader(
    std::string_view name,
    std::string_view value) noexcept {
    if (name.empty() || name.front() == ':') {
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

[[nodiscard]] inline bool http2AppendCookieHeaderValue(
    Http2StreamState& stream,
    std::string_view value) {
    constexpr std::string_view kCookieSeparator = "; ";
    const auto separatorBytes = stream.hasCookie ? kCookieSeparator.size() : 0;
    if (value.size() > kMaxHttpHeaderBytes ||
        stream.cookie.size() > kMaxHttpHeaderBytes - separatorBytes ||
        stream.cookie.size() + separatorBytes > kMaxHttpHeaderBytes - value.size()) {
        return false;
    }

    if (stream.hasCookie) {
        stream.cookie.append(kCookieSeparator.data(), kCookieSeparator.size());
    }
    if (!value.empty()) {
        stream.cookie.append(value.data(), value.size());
    }
    stream.hasCookie = true;
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
    if (name.empty() || stream.headers.full()) {
        return false;
    }

    if (name.front() == ':') {
        if (stream.regularHeaderSeen) {
            return false;
        }
        if (name == ":method") {
            if (stream.hasMethod) {
                return false;
            }
            stream.method = parseMethod(value);
            stream.hasMethod = true;
            return true;
        }
        if (name == ":protocol") {
            if (stream.hasProtocol || value.empty()) {
                return false;
            }
            stream.protocolIsWebSocket = value == "websocket";
            stream.hasProtocol = true;
            return true;
        }
        if (name == ":scheme") {
            if (stream.hasScheme) {
                return false;
            }
            stream.hasScheme = true;
            return true;
        }
        if (name == ":authority") {
            if (stream.hasAuthority) {
                return false;
            }
            stream.authority.assign(value.data(), value.size());
            stream.hasAuthority = true;
            return true;
        }
        if (name == ":path") {
            if (stream.hasPath || value.empty()) {
                return false;
            }
            stream.path.assign(value.data(), value.size());
            stream.hasPath = true;
            return true;
        }
        return false;
    }

    if (!http2IsValidRegularRequestHeader(name, value)) {
        return false;
    }
    stream.regularHeaderSeen = true;
    const auto kind = classifyRequestHeader(name);
    if (kind == RequestHeaderKind::kHost) {
        if (stream.hasHost) {
            return false;
        }
        stream.hasHost = true;
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
        if (stream.hasContentLength && stream.contentLength != parsed) {
            return false;
        }
        stream.contentLength = parsed;
        stream.hasContentLength = true;
    }
    return stream.headers.append(name, value, kind);
}

[[nodiscard]] inline bool http2OnDecodedTrailer(
    Http2HeaderDecodeContext& context,
    std::string_view name,
    std::string_view value) {
    if (!http2AccumulateHeaderListBytes(context, name, value)) {
        return false;
    }

    return http2IsValidRegularRequestHeader(name, value);
}

}  // namespace ruvia::detail
