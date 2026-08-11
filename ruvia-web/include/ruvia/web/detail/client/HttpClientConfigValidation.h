#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/detail/app/ConfigValidation.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/http/detail/cookie/CookieValidation.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"

namespace ruvia::detail {

inline void validateHttpClientUserAgent(std::string_view userAgent) {
    if (!userAgent.empty() && !isValidHttpHeaderValue(userAgent)) {
        throw std::invalid_argument("http client user agent is invalid");
    }
}

template <typename Config>
void validateHttpClientConfig(const Config& config) {
    const auto scheme = [&]() {
        if constexpr (requires { config.scheme(); }) {
            return config.scheme();
        } else {
            return config.scheme;
        }
    }();
    const auto host = [&]() -> std::string_view {
        if constexpr (requires { config.host(); }) {
            return config.host();
        } else {
            return config.host;
        }
    }();
    const auto port = [&]() {
        if constexpr (requires { config.port(); }) {
            return config.port();
        } else {
            return config.port;
        }
    }();
    if (scheme != HttpScheme::kHttp && scheme != HttpScheme::kHttps) {
        throw std::invalid_argument("http client scheme is invalid");
    }
    if (config.protocol != HttpClientProtocol::kNegotiate &&
        config.protocol != HttpClientProtocol::kHttp1Only &&
        config.protocol != HttpClientProtocol::kHttp2Only) {
        throw std::invalid_argument("http client protocol is invalid");
    }
    ensureConfigHost(host, "http client host must not be empty", "http client host is invalid", kSeparatedPortHostRules);
    if (port == 0) {
        throw std::invalid_argument("http client port must be greater than zero");
    }
    std::string wireHost;
    if (host.find(':') != std::string_view::npos) {
        wireHost.reserve(host.size() + 2);
        wireHost.push_back('[');
        wireHost.append(host);
        wireHost.push_back(']');
    } else {
        wireHost.assign(host);
    }
    if (!isValidHttpHost(wireHost)) {
        throw std::invalid_argument("http client host is invalid");
    }
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
    validateHttpClientUserAgent(config.userAgent);
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
    if (scheme == HttpScheme::kHttp && config.protocol == HttpClientProtocol::kNegotiate) {
        return;
    }
}

[[nodiscard]] inline std::uint16_t httpClientPort(const HttpClientConfigStorage& config) noexcept {
    return config.port;
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
