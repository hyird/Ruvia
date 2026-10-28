#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"

namespace ruvia::detail {

// RFC 9112 §3.2.2: an absolute-form target's authority is the request's host,
// and an HTTP/1.0 request may carry it without any Host field.
[[nodiscard]] inline std::string_view auto_https_redirect_authority(const http_request& request) noexcept {
    if (!request.authority().empty()) {
        return request.authority();
    }
    return request.header("Host").value_or(std::string_view{});
}

inline http_response make_auto_https_redirect_response(
    const http_request& request, request_memory& memory, std::uint16_t https_port) {
    http_response response(memory.resource());
    response.status(ruvia::http_status::permanent_redirect);

    const auto host = parse_http_authority_host(auto_https_redirect_authority(request)).value_or(std::string_view{});
    auto path = request.path();
    if (path.empty() || path.front() != '/') {
        path = "/";
    }

    // The raw target retains the distinction between an absent query and an
    // empty query ("/x" versus "/x?"); query_string() alone cannot express it.
    const bool has_query = (request.target().find('?') != std::string_view::npos);
    const auto query = request.query_string();
    std::array<char, http_max_unsigned_decimal_size> port_buffer{};
    const auto port_digits = https_port == 443 ? std::string_view{}
                                               : http_format_unsigned(port_buffer, https_port);
    static constexpr std::string_view scheme_prefix = "https://";
    const std::size_t total_size = scheme_prefix.size() + host.size() +
                                   (port_digits.empty() ? 0U : 1U + port_digits.size()) +
                                   path.size() + (has_query ? 1U + query.size() : 0U);
    static constexpr std::size_t inline_location_capacity = 256;
    if (total_size <= inline_location_capacity) {
        std::array<char, inline_location_capacity> inline_location{};
        std::size_t length = 0;
        std::memcpy(inline_location.data() + length, scheme_prefix.data(), scheme_prefix.size());
        length += scheme_prefix.size();
        if (!host.empty()) {
            std::memcpy(inline_location.data() + length, host.data(), host.size());
            length += host.size();
        }
        if (!port_digits.empty()) {
            inline_location[length++] = ':';
            std::memcpy(inline_location.data() + length, port_digits.data(), port_digits.size());
            length += port_digits.size();
        }
        std::memcpy(inline_location.data() + length, path.data(), path.size());
        length += path.size();
        if (has_query) {
            inline_location[length++] = '?';
            if (!query.empty()) {
                std::memcpy(inline_location.data() + length, query.data(), query.size());
                length += query.size();
            }
        }
        response.header("Location", std::string_view(inline_location.data(), length));
    } else {
        std::pmr::string location(memory.allocator<char>());
        location.reserve(total_size);
        location.append(scheme_prefix);
        location.append(host.data(), host.size());
        if (!port_digits.empty()) {
            location.push_back(':');
            location.append(port_digits);
        }
        location.append(path.data(), path.size());
        if (has_query) {
            location.push_back('?');
            if (!query.empty()) {
                location.append(query.data(), query.size());
            }
        }
        response.header("Location", location);
    }
    // The Location is derived from the request's Host header, so this redirect
    // varies by Host. Mark it private so a shared cache never stores one Host's
    // redirect and serves it for another (a Host-header cache-poisoning open
    // redirect); a browser still caches it per-origin, keeping the HTTPS memory.
    response.header_stable_view("Cache-Control", "private");
    return response;
}

}  // namespace ruvia::detail
