#include "ruvia/http/HttpClient.h"

#include "ruvia/http/detail/parser/HttpRequestTarget.h"

#include <stdexcept>

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

}  // namespace ruvia
