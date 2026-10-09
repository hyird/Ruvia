#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"

namespace ruvia::detail {

inline void append_https_port(std::pmr::string& location, std::uint16_t https_port) {
    if (https_port == 443) {
        return;
    }

    std::array<char, 5> port_buffer{};
    const auto [end, ec] =
        std::to_chars(port_buffer.data(), port_buffer.data() + port_buffer.size(), https_port);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format HTTPS redirect port");
    }

    location.push_back(':');
    location.append(port_buffer.data(), static_cast<std::size_t>(end - port_buffer.data()));
}

inline http_response make_auto_https_redirect_response(
    const http_request& request, request_memory& memory, std::uint16_t https_port) {
    http_response response({.resource_ = memory.resource()});
    response.status(ruvia::http_status::permanent_redirect);

    const auto host_field = request.header("Host").value_or(std::string_view{});
    const auto host = parse_http_authority_host(host_field).value_or(std::string_view{});
    auto path = request.path();
    if (path.empty() || path.front() != '/') {
        path = "/";
    }

    // The raw target retains the distinction between an absent query and an
    // empty query ("/x" versus "/x?"); query_string() alone cannot express it.
    const bool has_query = (request.target().find('?') != std::string_view::npos);
    std::pmr::string location(memory.allocator<char>());
    location.reserve(std::string_view("https://").size() + host.size() +
                     (https_port == 443 ? 0U : 6U) + path.size() +
                     (has_query ? 1U + request.query_string().size() : 0U));
    location.append("https://");
    location.append(host.data(), host.size());
    append_https_port(location, https_port);
    location.append(path.data(), path.size());
    if (has_query) {
        location.push_back('?');
        if (!request.query_string().empty()) {
            location.append(request.query_string().data(), request.query_string().size());
        }
    }

    response.header("Location", location);
    // The Location is derived from the request's Host header, so this redirect
    // varies by Host. Mark it private so a shared cache never stores one Host's
    // redirect and serves it for another (a Host-header cache-poisoning open
    // redirect); a browser still caches it per-origin, keeping the HTTPS memory.
    response.header("Cache-Control", "private");
    return response;
}

}  // namespace ruvia::detail
