#pragma once

#include "HttpServerResponseState.h"

#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace ruvia::detail {

inline std::string_view hostWithoutExplicitPort(std::string_view host) noexcept {
    if (host.empty()) {
        return host;
    }
    if (host.front() == '[') {
        const auto close = host.find(']');
        if (close == std::string_view::npos) {
            return host;
        }
        return host.substr(0, close + 1);
    }

    const auto colon = host.find(':');
    return colon == std::string_view::npos ? host : host.substr(0, colon);
}

inline void appendHttpsPort(std::pmr::string& location, std::uint16_t httpsPort) {
    if (httpsPort == 443) {
        return;
    }

    std::array<char, 5> portBuffer{};
    const auto [end, ec] = std::to_chars(portBuffer.data(), portBuffer.data() + portBuffer.size(), httpsPort);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format HTTPS redirect port");
    }

    location.push_back(':');
    location.append(portBuffer.data(), static_cast<std::size_t>(end - portBuffer.data()));
}

inline HttpResponse makeAutoHttpsRedirectResponse(
    const HttpRequest& request,
    RequestMemory& memory,
    std::uint16_t httpsPort) {
    HttpResponse response(memory.resource());
    response.setStatus(308, {});

    const auto host = hostWithoutExplicitPort(request.header(HttpRequest::KnownHeader::kHost));
    auto path = request.path();
    if (path.empty() || path.front() != '/') {
        path = "/";
    }

    std::pmr::string location(memory.allocator<char>());
    location.reserve(
        std::string_view("https://").size() +
        host.size() +
        (httpsPort == 443 ? 0U : 6U) +
        path.size() +
        (request.queryString().empty() ? 0U : 1U + request.queryString().size()));
    location.append("https://");
    location.append(host.data(), host.size());
    appendHttpsPort(location, httpsPort);
    location.append(path.data(), path.size());
    if (!request.queryString().empty()) {
        location.push_back('?');
        location.append(request.queryString().data(), request.queryString().size());
    }

    response.setHeader("Location", location);
    markConnectionClose(response);
    return response;
}

}  // namespace ruvia::detail
