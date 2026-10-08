#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpFieldValues.h"
#include "ruvia/http/HttpRequest.h"

#include "server/TrustedProxies.h"

// Reading the client's address and scheme out of forwarding headers, for a
// request whose peer the deployment has already declared trusted.
//
// Reverse proxies rewrite X-Forwarded-For / X-Forwarded-Proto and commonly
// forward a client `Forwarded` field unchanged. Preferring RFC 7239 when both
// are present would let the caller pick ConnInfo while the proxy-written
// X- headers name the real hop. X-Forwarded-* therefore win when present;
// `Forwarded` is the fallback when the proxy speaks only RFC 7239.
//
// Same-name list fields are one chain (RFC 9110 §5.2). A client line followed
// by a proxy-appended line is walked together; taking only the first line
// would let the caller pick the client and claim TLS.
//
// A client can prepend hops to these lists. The trusted peer is the hop that
// delivered the request, so the client is the last untrusted address. Scheme is
// the proto on that hop, not a proto from an earlier
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
    node = ::ruvia::httpTrimOws(node);
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
    token = ::ruvia::httpTrimOws(token);
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
    // RFC 7239 quoted-string values may contain commas; a quote-blind split
    // would invent hops from inside a single `for="..."` element.
    httpVisitCommaSeparatedQuotedFieldItems(value, [&](std::string_view item) {
        fn(item);
        return true;
    });
}

inline void parseForwardedElement(std::string_view element, ForwardedClient& hop) noexcept {
    hop = {};
    // RFC 7239 values are token / quoted-string; a ';' inside quotes is data.
    httpVisitSemicolonParametersQuotedField(
        element, [&](std::string_view name, std::string_view value) {
            value = httpTrimQuotedFieldValue(value);
            if (httpAsciiEqualsIgnoreCase(name, "for") && hop.address.empty()) {
                hop.address = forwardedNodeAddress(value);
            } else if (httpAsciiEqualsIgnoreCase(name, "proto") && hop.scheme.empty()) {
                hop.scheme = forwardedHttpSchemeToken(value);
            }
            return true;
        });
}

inline void accumulateForwardedChain(std::string_view value, const TrustedProxySet& trusted,
    ForwardedClient& leftmost, ForwardedClient& client) noexcept {
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
        }
    });
}

// Resolves what a trusted proxy says about the client. Fields it did not send
// stay empty and the caller keeps what the transport already knows.
[[nodiscard]] inline ForwardedClient resolveForwardedClient(
    const HttpRequest& request, const TrustedProxySet& trusted) noexcept {
    ForwardedClient forwardedLeftmost;
    ForwardedClient forwardedClient;
    std::string_view x_address{};
    std::size_t address_count = 0;
    std::size_t client_index = 0;
    bool has_x_for = false;
    const auto headers = request.headers();
    for (const auto& header : headers) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Forwarded")) {
            accumulateForwardedChain(
                header.value(), trusted, forwardedLeftmost, forwardedClient);
        } else if (httpAsciiEqualsIgnoreCase(header.name(), "X-Forwarded-For")) {
            has_x_for = true;
            visitCommaSeparated(header.value(), [&](std::string_view element) {
                const auto address = forwardedNodeAddress(element);
                if (address_count == 0 || !trusted.trusts(address)) {
                    x_address = address;
                    client_index = address_count;
                }
                ++address_count;
            });
        }
    }
    if (!has_x_for) {
        return forwardedClient.address.empty() ? forwardedLeftmost : forwardedClient;
    }
    // Never combine an XFF address with proto from an unrelated Forwarded
    // chain. XFP is usable only under an explicit sanitizing-proxy contract.
    ForwardedClient result{x_address, {}};
    if (!trusted.trusts_x_forwarded_proto()) {
        return result;
    }
    std::size_t proto_count = 0;
    for (const auto& header : headers) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "X-Forwarded-Proto")) {
            visitCommaSeparated(header.value(), [&](std::string_view element) {
                if (proto_count == client_index) {
                    result.scheme = forwardedHttpSchemeToken(element);
                }
                ++proto_count;
            });
        }
    }
    if (address_count != proto_count) {
        result.scheme = {};
    }
    return result;
}

}  // namespace ruvia::detail
