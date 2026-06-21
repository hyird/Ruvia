#pragma once

#include <charconv>
#include <string_view>

#include "Http2StreamState.h"
#include "../../http/parser/HttpParserSyntax.h"
#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpLimits.h"

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

[[nodiscard]] inline bool http2OnDecodedInitialHeader(
    Http2StreamState& stream,
    std::string_view name,
    std::string_view value) {
    if (name.empty() || stream.headers.size() >= kMaxRequestHeaders) {
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
            stream.method.assign(value.data(), value.size());
            stream.hasMethod = true;
            return true;
        }
        if (name == ":protocol") {
            if (stream.hasProtocol || value.empty()) {
                return false;
            }
            stream.protocol.assign(value.data(), value.size());
            stream.hasProtocol = true;
            return true;
        }
        if (name == ":scheme") {
            if (stream.hasScheme) {
                return false;
            }
            stream.scheme.assign(value.data(), value.size());
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
        if (!stream.cookie.empty()) {
            stream.cookie.append("; ");
        }
        stream.cookie.append(value.data(), value.size());
        stream.hasCookie = true;
        return true;
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
    stream.headers.emplace_back(name, value, kind, stream.headers.get_allocator().resource());
    return true;
}

[[nodiscard]] inline bool http2OnDecodedTrailer(
    Http2StreamState&,
    std::string_view name,
    std::string_view value) {
    return http2IsValidRegularRequestHeader(name, value);
}

}  // namespace ruvia::detail
