#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"

// Reading the client's address and scheme out of forwarding headers, for a
// request whose peer the deployment has already declared trusted.
//
// RFC 7239's `Forwarded` is preferred when present; X-Forwarded-For and
// X-Forwarded-Proto are the de-facto fields every proxy still emits, and are
// read only when Forwarded is absent so a proxy sending both cannot have the
// two disagree behind the caller's back.

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

// The leftmost element is the original client as the closest proxy saw it. Every
// element to its right was recorded by a node further from us, and the leftmost
// value itself is whatever the immediate client sent -- so this is only as
// trustworthy as the decision to trust the peer that delivered it.
[[nodiscard]] inline std::string_view forwardedForLeftmost(std::string_view value) noexcept {
    const auto comma = value.find(',');
    return forwardedNodeAddress(comma == std::string_view::npos ? value : value.substr(0, comma));
}

// Parses the first element of an RFC 7239 Forwarded field into for= and proto=.
inline void parseForwardedElement(std::string_view element, ForwardedClient& out) noexcept {
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
        if (httpAsciiEqualsIgnoreCase(name, "for") && out.address.empty()) {
            out.address = forwardedNodeAddress(value);
        } else if (httpAsciiEqualsIgnoreCase(name, "proto") && out.scheme.empty()) {
            out.scheme = value;
        }
    }
}

// Resolves what a trusted proxy says about the client. Fields it did not send
// stay empty and the caller keeps what the transport already knows.
[[nodiscard]] inline ForwardedClient resolveForwardedClient(const HttpRequest& request) noexcept {
    ForwardedClient result;

    for (const auto& header : request.headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Forwarded")) {
            const auto value = header.value();
            const auto comma = value.find(',');
            parseForwardedElement(
                comma == std::string_view::npos ? value : value.substr(0, comma), result);
            if (!result.address.empty() || !result.scheme.empty()) {
                return result;
            }
        }
    }

    for (const auto& header : request.headers()) {
        if (result.address.empty() && httpAsciiEqualsIgnoreCase(header.name(), "X-Forwarded-For")) {
            result.address = forwardedForLeftmost(header.value());
        } else if (result.scheme.empty() &&
                   httpAsciiEqualsIgnoreCase(header.name(), "X-Forwarded-Proto")) {
            result.scheme = httpTrimOws(header.value());
        }
    }
    return result;
}

}  // namespace ruvia::detail
