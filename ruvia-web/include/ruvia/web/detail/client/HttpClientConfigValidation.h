#pragma once

#include <stdexcept>

#include "ruvia/web/detail/app/ConfigValidation.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/http/detail/cookie/CookieValidation.h"

namespace ruvia::detail {

template <typename Config>
void validateHttpClientConfig(const Config& config) {
    ensureConfigHost(config.host, "http client host must not be empty", "http client host is invalid", kSeparatedPortHostRules);
    ensurePositiveSize(config.connectionsPerWorker, "http client connections per worker must be greater than zero");
    ensurePositiveSize(config.maxConcurrentHttp2StreamsPerConnection, "http client HTTP/2 stream limit per connection must be greater than zero");
    ensurePositiveSize(config.maxBufferedRequestsPerWorker, "http client buffered request limit per worker must be greater than zero");
    ensurePositiveSize(config.maxCookiesPerWorker, "http client cookie limit per worker must be greater than zero");
    ensurePositiveSize(config.maxCookieBytesPerWorker, "http client cookie byte limit per worker must be greater than zero");
    ensurePositiveSize(config.maxResponseBytes, "http client response byte limit must be greater than zero");
    ensurePositiveOptionalDurations("configured http client timeouts must be greater than zero", std::optional{config.connectTimeout}, config.writeTimeout, config.requestTimeout, config.acquireTimeout);
    if ((config.certificateChainFile.empty()) != (config.privateKeyFile.empty())) {
        throw std::invalid_argument("http client certificate chain and private key must be configured together");
    }
    if (config.cookies.size() > config.maxCookiesPerWorker) {
        throw std::invalid_argument("configured HTTP client cookies exceed the per-worker cookie limit");
    }
    std::size_t cookieBytes = 0;
    for (const auto& [name, value] : config.cookies) {
        if (!isValidHttpHeaderName(name) || !isValidCookieValue(value)) {
            throw std::invalid_argument("configured HTTP client cookie is invalid");
        }
        if (name.size() > config.maxCookieBytesPerWorker - cookieBytes) {
            throw std::invalid_argument("configured HTTP client cookies exceed the per-worker byte limit");
        }
        cookieBytes += name.size();
        if (value.size() > config.maxCookieBytesPerWorker - cookieBytes) {
            throw std::invalid_argument("configured HTTP client cookies exceed the per-worker byte limit");
        }
        cookieBytes += value.size();
        if (cookieBytes == config.maxCookieBytesPerWorker) {
            throw std::invalid_argument("configured HTTP client cookies exceed the per-worker byte limit");
        }
        ++cookieBytes;  // Every configured cookie is stored with the default "/" path.
    }
    if (config.scheme == HttpScheme::kHttp && config.protocol == HttpClientProtocol::kNegotiate) {
        return;
    }
}

[[nodiscard]] inline std::uint16_t httpClientPort(const HttpClientConfigStorage& config) noexcept {
    return config.port != 0 ? config.port : (config.scheme == HttpScheme::kHttps ? 443 : 80);
}

[[nodiscard]] inline std::pmr::string httpClientWireHost(
    const HttpClientConfigStorage& config,
    std::pmr::memory_resource* resource) {
    std::pmr::string host(resource);
    if (config.host.find(':') != std::string_view::npos) {
        host.reserve(config.host.size() + 2);
        host.push_back('[');
        host.append(config.host);
        host.push_back(']');
    } else {
        host.assign(config.host);
    }
    return host;
}

}  // namespace ruvia::detail
