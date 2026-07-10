#pragma once

#include <array>
#include <charconv>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "ruvia/http/HttpClient.h"
#include "ruvia/http/detail/PmrResource.h"

namespace ruvia::detail {

[[nodiscard]] inline bool httpOriginUsesDefaultPort(const HttpOrigin& origin) noexcept {
    return (!origin.tls && origin.port == 80) || (origin.tls && origin.port == 443);
}

[[nodiscard]] inline bool isValidHttpOriginHost(std::string_view host) noexcept {
    if (host.empty()) {
        return false;
    }
    std::size_t colonCount = 0;
    for (const auto ch : host) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F || byte == '/' || byte == '\\' ||
            byte == '[' || byte == ']') {
            return false;
        }
        colonCount += byte == ':' ? 1 : 0;
    }
    return colonCount != 1;
}

inline void ensureHttpOriginHost(
    std::string_view host,
    const char* emptyMessage,
    const char* invalidMessage) {
    if (host.empty()) {
        throw std::invalid_argument(emptyMessage);
    }
    if (!isValidHttpOriginHost(host)) {
        throw std::invalid_argument(invalidMessage);
    }
}

inline void validateHttpOrigin(const HttpOrigin& origin) {
    ensureHttpOriginHost(
        origin.host,
        "HTTP origin host must not be empty",
        "HTTP origin host is invalid");
    if (origin.port == 0) {
        throw std::invalid_argument("HTTP origin port must not be zero");
    }
}

[[nodiscard]] inline std::pmr::string makeHttpOriginAuthority(
    const HttpOrigin& origin,
    std::pmr::memory_resource* resource) {
    validateHttpOrigin(origin);

    auto* const targetResource = httpPmrResourceOrDefault(resource);
    const auto host = std::string_view(origin.host);
    const bool needsBrackets = host.find(':') != std::string_view::npos;
    const bool includePort = !httpOriginUsesDefaultPort(origin);
    std::pmr::string header(targetResource);
    header.reserve(
        host.size() +
        (needsBrackets ? 2 : 0) +
        (includePort ? 6 : 0));
    if (needsBrackets) {
        header.push_back('[');
    }
    header.append(host.data(), host.size());
    if (needsBrackets) {
        header.push_back(']');
    }
    if (includePort) {
        std::array<char, 5> portBuffer;
        const auto [end, ec] = std::to_chars(
            portBuffer.data(),
            portBuffer.data() + portBuffer.size(),
            origin.port);
        if (ec != std::errc{}) {
            throw std::logic_error("http client: failed to format Host port");
        }
        header.push_back(':');
        header.append(portBuffer.data(), static_cast<std::size_t>(end - portBuffer.data()));
    }
    return header;
}

}  // namespace ruvia::detail
