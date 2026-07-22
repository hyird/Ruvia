#pragma once

#include <array>
#include <charconv>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "ruvia/http/HttpClient.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {

[[nodiscard]] constexpr std::uint16_t httpSchemeDefaultPort(HttpScheme scheme) noexcept {
    return scheme == HttpScheme::kHttps ? std::uint16_t{443} : std::uint16_t{80};
}

[[nodiscard]] inline bool httpOriginUsesDefaultPort(const HttpOrigin& origin) noexcept {
    return origin.port() == httpSchemeDefaultPort(origin.scheme());
}

[[nodiscard]] inline std::pmr::string makeHttpOriginAuthority(
    const HttpOrigin& origin,
    std::pmr::memory_resource* resource) {
    auto* const targetResource = httpPmrResourceOrDefault(resource);
    const auto host = origin.host();
    const bool includePort = !httpOriginUsesDefaultPort(origin);
    std::pmr::string header(targetResource);
    header.reserve(host.size() + (includePort ? 6 : 0));
    header.append(host.data(), host.size());
    if (includePort) {
        std::array<char, 5> portBuffer;
        const auto [end, ec] = std::to_chars(
            portBuffer.data(),
            portBuffer.data() + portBuffer.size(),
            origin.port());
        if (ec != std::errc{}) {
            throw std::logic_error("http client: failed to format Host port");
        }
        header.push_back(':');
        header.append(portBuffer.data(), static_cast<std::size_t>(end - portBuffer.data()));
    }
    return header;
}

}  // namespace ruvia::detail
