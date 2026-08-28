#pragma once

#include <memory_resource>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "ruvia/web/detail/integration/NamedCapability.h"

namespace ruvia::detail {

struct HttpClientConfigStorage final {
    HttpClientConfigStorage(const HttpClientConfig& source, std::pmr::memory_resource* resource)
        : HttpClientConfigStorage(
              ValidatedConfigTag{}, validate(source), pmrResourceOrDefault(resource)) {}

    HttpClientConfigStorage(
        const HttpClientConfigStorage& source, std::pmr::memory_resource* resource)
        : HttpClientConfigStorage(ValidatedConfigTag{}, source, pmrResourceOrDefault(resource)) {}

    std::pmr::string host;
    HttpScheme scheme;
    std::uint16_t port;
    std::size_t connectionCount;
    std::size_t maxConcurrentHttp2StreamsPerConnection;
    std::size_t maxBufferedRequests;
    std::size_t maxCookies;
    std::size_t maxCookieBytes;
    std::chrono::milliseconds connectTimeout;
    std::optional<std::chrono::milliseconds> writeTimeout;
    std::optional<std::chrono::milliseconds> requestTimeout;
    std::optional<std::chrono::milliseconds> acquireTimeout;
    std::size_t maxResponseBytes;
    HttpClientProtocol protocol;
    ClientTransportConfigStorage transport;
    HttpClientReceivedCookiePolicy receivedCookies;
    std::pmr::string userAgent;
    std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> cookies;

private:
    struct ValidatedConfigTag final {};

    [[nodiscard]] static const HttpClientConfig& validate(const HttpClientConfig& source) {
        validateHttpClientConfig(source);
        return source;
    }

    HttpClientConfigStorage(
        ValidatedConfigTag, const HttpClientConfig& source, std::pmr::memory_resource* resource)
        : host(source.host, resource),
          scheme(source.scheme),
          port(source.port.value_or(source.scheme == HttpScheme::kHttps ? 443 : 80)),
          connectionCount(source.connectionCount),
          maxConcurrentHttp2StreamsPerConnection(source.maxConcurrentHttp2StreamsPerConnection),
          maxBufferedRequests(source.maxBufferedRequests),
          maxCookies(source.maxCookies),
          maxCookieBytes(source.maxCookieBytes),
          connectTimeout(source.connectTimeout),
          writeTimeout(source.writeTimeout),
          requestTimeout(source.requestTimeout),
          acquireTimeout(source.acquireTimeout),
          maxResponseBytes(source.maxResponseBytes),
          protocol(source.protocol),
          transport(clientTransportConfigView(source), resource),
          receivedCookies(source.receivedCookies),
          userAgent(source.userAgent, resource),
          cookies(resource) {
        cookies.reserve(source.cookies.size());
        for (const auto& [name, value] : source.cookies) {
            cookies.emplace_back(
                std::pmr::string(name, resource), std::pmr::string(value, resource));
        }
    }

    HttpClientConfigStorage(ValidatedConfigTag, const HttpClientConfigStorage& source,
        std::pmr::memory_resource* resource)
        : host(source.host, resource),
          scheme(source.scheme),
          port(source.port),
          connectionCount(source.connectionCount),
          maxConcurrentHttp2StreamsPerConnection(source.maxConcurrentHttp2StreamsPerConnection),
          maxBufferedRequests(source.maxBufferedRequests),
          maxCookies(source.maxCookies),
          maxCookieBytes(source.maxCookieBytes),
          connectTimeout(source.connectTimeout),
          writeTimeout(source.writeTimeout),
          requestTimeout(source.requestTimeout),
          acquireTimeout(source.acquireTimeout),
          maxResponseBytes(source.maxResponseBytes),
          protocol(source.protocol),
          transport(source.transport, resource),
          receivedCookies(source.receivedCookies),
          userAgent(source.userAgent, resource),
          cookies(resource) {
        cookies.reserve(source.cookies.size());
        for (const auto& [name, value] : source.cookies) {
            cookies.emplace_back(
                std::pmr::string(name, resource), std::pmr::string(value, resource));
        }
    }
};

[[nodiscard]] inline std::uint16_t httpClientPort(const HttpClientConfigStorage& config) noexcept {
    return config.port;
}

[[nodiscard]] inline std::pmr::string httpClientWireHost(
    const HttpClientConfigStorage& config, std::pmr::memory_resource* resource) {
    std::pmr::string host(pmrResourceOrDefault(resource));
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

using HttpClientDefinition = NamedCapabilityDefinition<HttpClientConfigStorage>;

}  // namespace ruvia::detail
