#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_request.h"

#include "server/trusted_proxies.h"

// Reading the client's address and scheme out of forwarding headers, for a
// request whose peer the deployment has already declared trusted.
//
// Reverse proxies rewrite X-Forwarded-For / X-Forwarded-Proto and commonly
// forward a client `Forwarded` field unchanged. Preferring RFC 7239 when both
// are present would let the caller pick conn_info while the proxy-written
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

struct forwarded_client final {
    std::string_view address_;
    std::string_view scheme_;
};

// "[2001:db8::1]:443" -> "2001:db8::1"; "192.0.2.1:8080" -> "192.0.2.1".
// A bare IPv6 literal has colons of its own, so only a bracketed form or a
// single trailing colon can carry a port.
[[nodiscard]] inline std::string_view forwarded_node_address(std::string_view node_value) noexcept {
    node_value = ::ruvia::http_trim_ows(node_value);
    if (node_value.size() >= 2 && node_value.front() == '"' && node_value.back() == '"') {
        node_value = node_value.substr(1, node_value.size() - 2);
    }
    if (!node_value.empty() && node_value.front() == '[') {
        const auto close = node_value.find(']');
        return close == std::string_view::npos ? std::string_view{} : node_value.substr(1, close - 1);
    }
    const auto colon = node_value.find(':');
    if (colon != std::string_view::npos && node_value.find(':', colon + 1) == std::string_view::npos) {
        return node_value.substr(0, colon);
    }
    return node_value;
}

[[nodiscard]] inline std::string_view forwarded_http_scheme_token(std::string_view token) noexcept {
    token = ::ruvia::http_trim_ows(token);
    if (http_ascii_equals_ignore_case(token, "http")) {
        return "http";
    }
    if (http_ascii_equals_ignore_case(token, "https")) {
        return "https";
    }
    return {};
}

template <typename fn_type>
inline void visit_comma_separated(std::string_view value, fn_type&& fn) {
    // RFC 7239 quoted-string values may contain commas; a quote-blind split
    // would invent hops from inside a single `for="..."` element.
    http_visit_comma_separated_quoted_field_items(value, [&](std::string_view item) {
        fn(item);
        return true;
    });
}

inline void parse_forwarded_element(std::string_view element, forwarded_client& hop) noexcept {
    hop = {};
    // RFC 7239 values are token / quoted-string; a ';' inside quotes is data.
    http_visit_semicolon_parameters_quoted_field(
        element, [&](std::string_view name, std::string_view value) {
            value = http_trim_quoted_field_value(value);
            if (http_ascii_equals_ignore_case(name, "for") && hop.address_.empty()) {
                hop.address_ = forwarded_node_address(value);
            } else if (http_ascii_equals_ignore_case(name, "proto") && hop.scheme_.empty()) {
                hop.scheme_ = forwarded_http_scheme_token(value);
            }
            return true;
        });
}

inline void accumulate_forwarded_chain(std::string_view value, const trusted_proxy_set& trusted,
    forwarded_client& leftmost, forwarded_client& client) noexcept {
    visit_comma_separated(value, [&](std::string_view element) {
        forwarded_client hop;
        parse_forwarded_element(element, hop);
        if (hop.address_.empty() && hop.scheme_.empty()) {
            return;
        }
        if (leftmost.address_.empty() && !hop.address_.empty()) {
            leftmost.address_ = hop.address_;
            leftmost.scheme_ = hop.scheme_;
        }
        if (!hop.address_.empty() && !trusted.trusts(hop.address_)) {
            // A new client hop discards proto claimed by a prepended hop.
            client.address_ = hop.address_;
            client.scheme_ = hop.scheme_;
        }
    });
}

// Resolves what a trusted proxy says about the client. Fields it did not send
// stay empty and the caller keeps what the transport already knows.
[[nodiscard]] inline forwarded_client resolve_forwarded_client(
    const http_request& request, const trusted_proxy_set& trusted) noexcept {
    forwarded_client forwarded_leftmost;
    forwarded_client forwarded_peer;
    std::string_view x_address{};
    std::size_t address_count = 0;
    std::size_t client_index = 0;
    bool has_x_for = false;
    const auto headers = request.headers();
    for (const auto& header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Forwarded")) {
            accumulate_forwarded_chain(
                header.value(), trusted, forwarded_leftmost, forwarded_peer);
        } else if (http_ascii_equals_ignore_case(header.name(), "X-Forwarded-For")) {
            has_x_for = true;
            visit_comma_separated(header.value(), [&](std::string_view element) {
                const auto address = forwarded_node_address(element);
                if (address_count == 0 || !trusted.trusts(address)) {
                    x_address = address;
                    client_index = address_count;
                }
                ++address_count;
            });
        }
    }
    if (!has_x_for) {
        return forwarded_peer.address_.empty() ? forwarded_leftmost : forwarded_peer;
    }
    // Never combine an XFF address with proto from an unrelated Forwarded
    // chain. XFP is usable only under an explicit sanitizing-proxy contract.
    forwarded_client result_value{x_address, {}};
    if (!trusted.trusts_x_forwarded_proto()) {
        return result_value;
    }
    std::size_t proto_count = 0;
    for (const auto& header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "X-Forwarded-Proto")) {
            visit_comma_separated(header.value(), [&](std::string_view element) {
                if (proto_count == client_index) {
                    result_value.scheme_ = forwarded_http_scheme_token(element);
                }
                ++proto_count;
            });
        }
    }
    if (address_count != proto_count) {
        result_value.scheme_ = {};
    }
    return result_value;
}

}  // namespace ruvia::detail
