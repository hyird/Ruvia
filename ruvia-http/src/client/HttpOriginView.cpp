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

}  // namespace

HttpOriginView HttpOriginView::http(std::string_view host, std::uint16_t port) {
    validateOriginHost(host);
    return HttpOriginView(HttpScheme::kHttp, host, port);
}

HttpOriginView HttpOriginView::https(std::string_view host, std::uint16_t port) {
    validateOriginHost(host);
    return HttpOriginView(HttpScheme::kHttps, host, port);
}

}  // namespace ruvia
