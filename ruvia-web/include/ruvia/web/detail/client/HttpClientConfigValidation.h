#pragma once

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/detail/config/ConfigValidation.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/cookie/CookieValidation.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/ClientTransport.h"

namespace ruvia::detail {

inline void validateHttpClientUserAgent(std::string_view userAgent) {
    if (!userAgent.empty() && !isValidHttpHeaderValue(userAgent)) {
        throw std::invalid_argument("http client user agent is invalid");
    }
}

inline void validateHttpClientConfig(const HttpClientConfig& config) {
    const auto scheme = config.scheme;
    const std::string_view host = config.host;
    const auto port = config.port.value_or(scheme == HttpScheme::kHttps ? 443 : 80);
    if (scheme != HttpScheme::kHttp && scheme != HttpScheme::kHttps) {
        throw std::invalid_argument("http client scheme is invalid");
    }
    if (config.protocol != HttpClientProtocol::kNegotiate && config.protocol != HttpClientProtocol::kHttp1Only && config.protocol != HttpClientProtocol::kHttp2Only) {
        throw std::invalid_argument("http client protocol is invalid");
    }
    validateClientTransportConfig(clientTransportConfigView(config));
    if (config.receivedCookies != HttpClientReceivedCookiePolicy::kIgnore && config.receivedCookies != HttpClientReceivedCookiePolicy::kRetainAndSend) {
        throw std::invalid_argument("http client received cookie policy is invalid");
    }
    validateClientOriginHost(host, "http client host must not be empty", "http client host is invalid");
    if (port == 0) {
        throw std::invalid_argument("http client port must be greater than zero");
    }
    ensurePositiveSize(config.connectionCount, "http client connection count must be greater than zero");
    ensurePositiveSize(config.maxConcurrentHttp2StreamsPerConnection, "http client HTTP/2 stream limit per connection must be greater than zero");
    if (config.maxConcurrentHttp2StreamsPerConnection > std::numeric_limits<std::size_t>::max() / config.connectionCount) {
        throw std::invalid_argument("HTTP client connection and HTTP/2 stream capacity is too large");
    }
    ensurePositiveSize(config.maxBufferedRequests, "http client buffered request limit must be greater than zero");
    ensurePositiveSize(config.maxCookies, "http client cookie limit must be greater than zero");
    ensurePositiveSize(config.maxCookieBytes, "http client cookie byte limit must be greater than zero");
    ensurePositiveSize(config.maxResponseBytes, "http client response byte limit must be greater than zero");
    ensurePositiveOptionalDurations("configured http client timeouts must be greater than zero", std::optional{config.connectTimeout}, config.writeTimeout, config.requestTimeout, config.acquireTimeout);
    validateHttpClientUserAgent(config.userAgent);
    if (config.cookies.size() > config.maxCookies) {
        throw std::invalid_argument("configured HTTP client cookies exceed the client cookie limit");
    }
    std::size_t cookieBytes = 0;
    for (const auto& [name, value] : config.cookies) {
        if (!isValidHttpHeaderName(name) || !isValidCookieValue(value)) {
            throw std::invalid_argument("configured HTTP client cookie is invalid");
        }
        if (name.size() > config.maxCookieBytes - cookieBytes) {
            throw std::invalid_argument("configured HTTP client cookies exceed the client byte limit");
        }
        cookieBytes += name.size();
        if (value.size() > config.maxCookieBytes - cookieBytes) {
            throw std::invalid_argument("configured HTTP client cookies exceed the client byte limit");
        }
        cookieBytes += value.size();
        if (cookieBytes == config.maxCookieBytes) {
            throw std::invalid_argument("configured HTTP client cookies exceed the client byte limit");
        }
        ++cookieBytes;  // Every configured cookie is stored with the default "/" path.
    }
}

}  // namespace ruvia::detail
