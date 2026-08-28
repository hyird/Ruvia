#pragma once

#include "ruvia/web/WebSocketClient.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/WebSocketClientConfigValidation.h"

namespace ruvia::detail {

struct WebSocketClientStoredHeader final {
    WebSocketClientStoredHeader(
        std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
        : name(name, resource),
          value(value, resource) {}

    std::pmr::string name;
    std::pmr::string value;
};

struct WebSocketClientConfigStorage final {
    WebSocketClientConfigStorage(
        const WebSocketClientConfig& source, std::pmr::memory_resource* resource)
        : WebSocketClientConfigStorage(
              ValidatedConfigTag{}, validate(source), pmrResourceOrDefault(resource)) {}

    WebSocketScheme scheme;
    std::pmr::string host;
    std::optional<std::uint16_t> port;
    std::pmr::string target;
    std::pmr::vector<WebSocketClientStoredHeader> headers;
    std::pmr::string subprotocols;
    std::size_t maxMessageBytes;
    std::chrono::milliseconds connectTimeout;
    std::optional<std::chrono::milliseconds> readTimeout;
    std::optional<std::chrono::milliseconds> writeTimeout;
    std::optional<std::chrono::milliseconds> closeHandshakeTimeout;
    ClientTransportConfigStorage transport;
    std::pmr::string userAgent;

private:
    struct ValidatedConfigTag final {};

    [[nodiscard]] static const WebSocketClientConfig& validate(
        const WebSocketClientConfig& source) {
        validateWebSocketClientConfig(source);
        return source;
    }

    WebSocketClientConfigStorage(ValidatedConfigTag, const WebSocketClientConfig& source,
        std::pmr::memory_resource* resource)
        : scheme(source.scheme),
          host(source.host, resource),
          port(source.port),
          target(source.target, resource),
          headers(resource),
          subprotocols(source.subprotocols, resource),
          maxMessageBytes(source.maxMessageBytes),
          connectTimeout(source.connectTimeout),
          readTimeout(source.readTimeout),
          writeTimeout(source.writeTimeout),
          closeHandshakeTimeout(source.closeHandshakeTimeout),
          transport(clientTransportConfigView(source), resource),
          userAgent(source.userAgent, resource) {
        headers.reserve(source.headers.size());
        for (const auto& [name, value] : source.headers) {
            headers.emplace_back(name, value, resource);
        }
    }
};

}  // namespace ruvia::detail
