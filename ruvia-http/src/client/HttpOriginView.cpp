#include "client/HttpOriginView.h"

#include <array>
#include <charconv>
#include <stdexcept>
#include <system_error>

#include "ruvia/http/HttpClient.h"
#include "ruvia/http/detail/util/PmrResource.h"

#include "parser/HttpRequestTarget.h"

namespace ruvia {
namespace {

void validateOriginHost(std::string_view host) {
    if (host.empty()) {
        throw std::invalid_argument("HTTP origin host must not be empty");
    }
    if (!detail::isValidHttpHost(host)) {
        throw std::invalid_argument("HTTP origin host is invalid");
    }
}

std::uint16_t resolvedOriginPort(HttpScheme scheme, const HttpOriginOptions& options) noexcept {
    if (options.port.has_value()) {
        return *options.port;
    }
    return scheme == HttpScheme::kHttps ? std::uint16_t{443} : std::uint16_t{80};
}

}  // namespace

HttpOriginView HttpOriginView::http(HttpOriginOptions options) {
    const auto host = options.host.view();
    validateOriginHost(host);
    return HttpOriginView(HttpScheme::kHttp, host, resolvedOriginPort(HttpScheme::kHttp, options));
}

HttpOriginView HttpOriginView::https(HttpOriginOptions options) {
    const auto host = options.host.view();
    validateOriginHost(host);
    return HttpOriginView(
        HttpScheme::kHttps, host, resolvedOriginPort(HttpScheme::kHttps, options));
}

std::pmr::string makeHttpOriginAuthority(const HttpOriginView& origin,
    std::pmr::memory_resource* resource) {
    const auto host = origin.host();
    const bool includePort = !detail::httpOriginUsesDefaultPort(origin);
    std::pmr::string authority(detail::httpPmrResourceOrDefault(resource));
    authority.reserve(host.size() + (includePort ? 6 : 0));
    authority.append(host);
    if (includePort) {
        std::array<char, 5> portBuffer;
        const auto [end, error] = std::to_chars(portBuffer.data(), portBuffer.data() + portBuffer.size(), origin.port());
        if (error != std::errc{}) {
            throw std::logic_error("HTTP origin port formatting failed");
        }
        authority.push_back(':');
        authority.append(portBuffer.data(), static_cast<std::size_t>(end - portBuffer.data()));
    }
    return authority;
}

}  // namespace ruvia
