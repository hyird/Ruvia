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

HttpOrigin HttpOrigin::http(std::string_view host, std::uint16_t port) {
    validateOriginHost(host);
    return HttpOrigin(HttpScheme::kHttp, host, port);
}

HttpOrigin HttpOrigin::https(std::string_view host, std::uint16_t port) {
    validateOriginHost(host);
    return HttpOrigin(HttpScheme::kHttps, host, port);
}

}  // namespace ruvia
