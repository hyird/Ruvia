#pragma once

#include <cstddef>
#include <string_view>

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
// delivered the request, so walking from the right, the first element whose
// node is not trusted is the client hop -- including an element with no node,
// `unknown` or an obfuscated name (HAProxy appends a bare `proto=`), since its
// writer did not vouch for anything further left. Such a hop names no client,
// so the client is the nearest trusted address to its right, or the transport
// peer when the hop is the rightmost. Scheme is the proto on the client hop,
// not one from an earlier prepended element -- taking the last proto in the
// whole list would let the caller claim TLS whenever the real hop omitted it.

namespace ruvia::detail {

struct forwarded_client final {
    std::string_view address_;
    std::string_view scheme_;
};

// "[2001:db8::1]:443" -> "2001:db8::1"; "192.0.2.1:8080" -> "192.0.2.1".
// A bare IPv6 literal has colons of its own, so only a bracketed form or a
// single trailing colon can carry a port.
[[nodiscard]] inline std::string_view forwarded_node_address(std::string_view node_value) noexcept {
    node_value = ::ruvia::http_trim_quoted_field_value(::ruvia::http_trim_ows(node_value));
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
    // would invent hops from inside a single `for="..."` element. Empty list
    // elements are not hops (RFC 9110 §5.6.1).
    http_visit_comma_separated_quoted_field_items(value, [&](std::string_view item) {
        if (!item.empty()) {
            fn(item);
        }
        return true;
    });
}

[[nodiscard]] inline forwarded_client parse_forwarded_element(std::string_view element) noexcept {
    forwarded_client hop;
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
    return hop;
}

// One forwarding list, folded left to right across same-name fields.
class forwarding_chain final {
public:
    void append(const trusted_proxy_set& trusted, forwarded_client hop) noexcept {
        if (size_ == 0) {
            leftmost_ = hop;
        }
        const auto trust = trusted.classify(hop.address_);
        if (trust == trusted_proxy_set::hop_trust::trusted) {
            if (awaiting_nearest_trusted_) {
                nearest_trusted_ = hop.address_;
                awaiting_nearest_trusted_ = false;
            }
        } else {
            client_hop_ = hop;
            client_index_ = size_;
            client_identified_ = trust == trusted_proxy_set::hop_trust::untrusted;
            has_client_hop_ = true;
            nearest_trusted_ = {};
            awaiting_nearest_trusted_ = true;
        }
        ++size_;
    }

    // Every hop trusted: the leftmost is as far as the trusted proxies reach.
    [[nodiscard]] forwarded_client client() const noexcept {
        if (!has_client_hop_) {
            return leftmost_;
        }
        return {client_identified_ ? client_hop_.address_ : nearest_trusted_, client_hop_.scheme_};
    }

    [[nodiscard]] std::size_t client_index() const noexcept {
        return client_index_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    forwarded_client leftmost_;
    forwarded_client client_hop_;
    std::string_view nearest_trusted_;
    std::size_t client_index_{0};
    std::size_t size_{0};
    bool has_client_hop_{false};
    bool client_identified_{false};
    bool awaiting_nearest_trusted_{false};
};

// Resolves what a trusted proxy says about the client. Fields it did not send
// stay empty and the caller keeps what the transport already knows.
[[nodiscard]] inline forwarded_client resolve_forwarded_client(
    const http_request& request, const trusted_proxy_set& trusted) noexcept {
    forwarding_chain forwarded;
    forwarding_chain x_forwarded_for;
    bool has_x_for = false;
    bool has_x_proto = false;
    const auto headers = request.headers();
    for (const auto& header : headers) {
        const auto name = header.name();
        if (name.size() == 9U && http_ascii_equals_ignore_case(name, "Forwarded")) {
            visit_comma_separated(header.value(), [&](std::string_view element) {
                forwarded.append(trusted, parse_forwarded_element(element));
            });
        } else if (name.size() == 15U && http_ascii_equals_ignore_case(name, "X-Forwarded-For")) {
            has_x_for = true;
            visit_comma_separated(header.value(), [&](std::string_view element) {
                x_forwarded_for.append(trusted, {forwarded_node_address(element), {}});
            });
        } else if (name.size() == 17U && http_ascii_equals_ignore_case(name, "X-Forwarded-Proto")) {
            has_x_proto = true;
        }
    }
    if (!has_x_for) {
        return forwarded.client();
    }
    // Never combine an XFF address with proto from an unrelated Forwarded
    // chain. XFP is usable only under an explicit sanitizing-proxy contract.
    forwarded_client result_value{x_forwarded_for.client().address_, {}};
    if (!trusted.trusts_x_forwarded_proto() || !has_x_proto) {
        return result_value;
    }
    std::size_t proto_count = 0;
    for (const auto& header : headers) {
        const auto name = header.name();
        if (name.size() == 17U && http_ascii_equals_ignore_case(name, "X-Forwarded-Proto")) {
            visit_comma_separated(header.value(), [&](std::string_view element) {
                if (proto_count == x_forwarded_for.client_index()) {
                    result_value.scheme_ = forwarded_http_scheme_token(element);
                }
                ++proto_count;
            });
        }
    }
    if (x_forwarded_for.size() != proto_count) {
        result_value.scheme_ = {};
    }
    return result_value;
}

}  // namespace ruvia::detail
