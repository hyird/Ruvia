#pragma once

#include <concepts>
#include <memory_resource>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/HttpClientTypes.h"

#include "client/ClientTransport.h"
#include "client/HttpClientConfigValidation.h"
#include "integration/NamedCapability.h"

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
    std::optional<std::chrono::milliseconds> write_timeout;
    std::optional<std::chrono::milliseconds> requestTimeout;
    std::optional<std::chrono::milliseconds> acquireTimeout;
    std::size_t maxResponseBytes;
    HttpClientProtocol protocol;
    quic_version initial_quic_version;
    bool http3_early_data;
    Http3QpackConfig http3Qpack;
    HttpClientAdvertisementConfig advertisements;
    HttpClientPushConfig push;
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

    template <typename source_type>
    [[nodiscard]] static std::uint16_t resolved_port(const source_type& source) noexcept {
        if constexpr (std::same_as<source_type, HttpClientConfig>) {
            return source.port.value_or(source.scheme == HttpScheme::kHttps ? 443 : 80);
        } else {
            return source.port;
        }
    }

    template <typename source_type>
    [[nodiscard]] static const Http3QpackConfig& qpack_config(const source_type& source) noexcept {
        if constexpr (std::same_as<source_type, HttpClientConfig>) {
            return source.qpack;
        } else {
            return source.http3Qpack;
        }
    }

    // Resolve representation differences at the boundary, then own every field
    // and cookie through one allocator-bound construction path.
    template <typename source_type>
        requires(std::same_as<source_type, HttpClientConfig> ||
                    std::same_as<source_type, HttpClientConfigStorage>)
    HttpClientConfigStorage(
        ValidatedConfigTag, const source_type& source, std::pmr::memory_resource* resource)
        : host(source.host, resource),
          scheme(source.scheme),
          port(resolved_port(source)),
          connectionCount(source.connectionCount),
          maxConcurrentHttp2StreamsPerConnection(source.maxConcurrentHttp2StreamsPerConnection),
          maxBufferedRequests(source.maxBufferedRequests),
          maxCookies(source.maxCookies),
          maxCookieBytes(source.maxCookieBytes),
          connectTimeout(source.connectTimeout),
          write_timeout(source.write_timeout),
          requestTimeout(source.requestTimeout),
          acquireTimeout(source.acquireTimeout),
          maxResponseBytes(source.maxResponseBytes),
          protocol(source.protocol),
          initial_quic_version(source.initial_quic_version),
          http3_early_data(source.http3_early_data),
          http3Qpack(qpack_config(source)),
          advertisements(source.advertisements),
          push(source.push),
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
};

[[nodiscard]] inline std::uint16_t httpClientPort(const HttpClientConfigStorage& config) noexcept {
    return config.port;
}

using HttpClientDefinition = NamedCapabilityDefinition<HttpClientConfigStorage>;

}  // namespace ruvia::detail
