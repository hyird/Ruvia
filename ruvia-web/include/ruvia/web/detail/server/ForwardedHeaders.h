#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/util/HttpOws.h"
#include "ruvia/web/detail/server/TrustedProxies.h"

// Reading the client's address and scheme out of forwarding headers, for a
// request whose peer the deployment has already declared trusted.
//
// RFC 7239's `Forwarded` is preferred when present; X-Forwarded-For and
// X-Forwarded-Proto are the de-facto fields every proxy still emits, and are
// read only when Forwarded is absent so a proxy sending both cannot have the
// two disagree behind the caller's back.
//
// A client can prepend hops to these lists. The trusted peer is the hop that
// delivered the request, so the client is the last untrusted address. Scheme is
// the proto on that hop (or a later trusted hop), not a proto from an earlier
// prepended element -- taking the last proto in the whole list would let the
// caller claim TLS whenever the real hop omitted proto.

namespace ruvia::detail {

struct ForwardedClient final {
    std::string_view address;
    std::string_view scheme;
};

// "[2001:db8::1]:443" -> "2001:db8::1"; "192.0.2.1:8080" -> "192.0.2.1".
// A bare IPv6 literal has colons of its own, so only a bracketed form or a
// single trailing colon can carry a port.
[[nodiscard]] inline std::string_view forwardedNodeAddress(std::string_view node) noexcept {
    node = httpTrimOws(node);
    if (node.size() >= 2 && node.front() == '"' && node.back() == '"') {
        node = node.substr(1, node.size() - 2);
    }
    if (!node.empty() && node.front() == '[') {
        const auto close = node.find(']');
        return close == std::string_view::npos ? std::string_view{} : node.substr(1, close - 1);
    }
    const auto colon = node.find(':');
    if (colon != std::string_view::npos && node.find(':', colon + 1) == std::string_view::npos) {
        return node.substr(0, colon);
    }
    return node;
}

[[nodiscard]] inline std::string_view forwardedHttpSchemeToken(std::string_view token) noexcept {
    token = httpTrimOws(token);
    if (httpAsciiEqualsIgnoreCase(token, "http")) {
        return "http";
    }
    if (httpAsciiEqualsIgnoreCase(token, "https")) {
        return "https";
    }
    return {};
}

template <typename Fn>
inline void visitCommaSeparated(std::string_view value, Fn&& fn) {
    std::size_t offset = 0;
    while (offset <= value.size()) {
        const auto comma = value.find(',', offset);
        const auto end = comma == std::string_view::npos ? value.size() : comma;
        fn(value.substr(offset, end - offset));
        if (comma == std::string_view::npos) {
            break;
        }
        offset = comma + 1;
    }
}

[[nodiscard]] inline std::string_view forwardedChainClient(
    std::string_view value, const TrustedProxySet& trusted) noexcept {
    std::string_view leftmost{};
    std::string_view client{};
    visitCommaSeparated(value, [&](std::string_view element) {
        const auto address = forwardedNodeAddress(element);
        if (address.empty()) {
            return;
        }
        if (leftmost.empty()) {
            leftmost = address;
        }
        if (!trusted.trusts(address)) {
            client = address;
        }
    });
    return client.empty() ? leftmost : client;
}

[[nodiscard]] inline std::string_view forwardedChainScheme(std::string_view value) noexcept {
    std::string_view scheme{};
    visitCommaSeparated(value, [&](std::string_view element) {
        const auto token = forwardedHttpSchemeToken(element);
        if (!token.empty()) {
            scheme = token;
        }
    });
    return scheme;
}

inline void parseForwardedElement(std::string_view element, ForwardedClient& hop) noexcept {
    hop = {};
    std::size_t offset = 0;
    while (offset < element.size()) {
        auto end = element.find(';', offset);
        if (end == std::string_view::npos) {
            end = element.size();
        }
        const auto pair = httpTrimOws(element.substr(offset, end - offset));
        offset = end + 1;

        const auto equals = pair.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const auto name = httpTrimOws(pair.substr(0, equals));
        auto value = httpTrimOws(pair.substr(equals + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (httpAsciiEqualsIgnoreCase(name, "for") && hop.address.empty()) {
            hop.address = forwardedNodeAddress(value);
        } else if (httpAsciiEqualsIgnoreCase(name, "proto") && hop.scheme.empty()) {
            hop.scheme = forwardedHttpSchemeToken(value);
        }
    }
}

[[nodiscard]] inline ForwardedClient resolveForwardedChain(
    std::string_view value, const TrustedProxySet& trusted) noexcept {
    ForwardedClient leftmost{};
    ForwardedClient client{};
    visitCommaSeparated(value, [&](std::string_view element) {
        ForwardedClient hop;
        parseForwardedElement(element, hop);
        if (hop.address.empty() && hop.scheme.empty()) {
            return;
        }
        if (leftmost.address.empty() && !hop.address.empty()) {
            leftmost.address = hop.address;
            leftmost.scheme = hop.scheme;
        }
        if (!hop.address.empty() && !trusted.trusts(hop.address)) {
            // A new client hop discards proto claimed by a prepended hop.
            client.address = hop.address;
            client.scheme = hop.scheme;
        } else if (!client.address.empty() && !hop.scheme.empty()) {
            client.scheme = hop.scheme;
        }
    });
    if (client.address.empty()) {
        return leftmost;
    }
    return client;
}

// Resolves what a trusted proxy says about the client. Fields it did not send
// stay empty and the caller keeps what the transport already knows.
[[nodiscard]] inline ForwardedClient resolveForwardedClient(
    const HttpRequest& request, const TrustedProxySet& trusted) noexcept {
    ForwardedClient result;

    for (const auto& header : request.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Forwarded")) {
            result = resolveForwardedChain(header.value(), trusted);
            if (!result.address.empty() || !result.scheme.empty()) {
                return result;
            }
        }
    }

    for (const auto& header : request.headers()) {
        if (result.address.empty() && httpAsciiEqualsIgnoreCase(header.name(), "X-Forwarded-For")) {
            result.address = forwardedChainClient(header.value(), trusted);
        } else if (result.scheme.empty() &&
                   httpAsciiEqualsIgnoreCase(header.name(), "X-Forwarded-Proto")) {
            result.scheme = forwardedChainScheme(header.value());
        }
    }
    return result;
}

}  // namespace ruvia::detail
