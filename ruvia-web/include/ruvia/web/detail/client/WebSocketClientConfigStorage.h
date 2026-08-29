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

struct WebSocketClientSubprotocolRange final {
    std::size_t offset;
    std::size_t length;
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
    std::pmr::string subprotocolHeader;
    std::pmr::vector<WebSocketClientSubprotocolRange> subprotocolRanges;
    std::size_t maxMessageBytes;
    std::chrono::milliseconds connectTimeout;
    std::optional<std::chrono::milliseconds> readTimeout;
    std::optional<std::chrono::milliseconds> writeTimeout;
    std::optional<std::chrono::milliseconds> closeHandshakeTimeout;
    WebSocketHeartbeatConfig heartbeat;
    ClientTransportConfigStorage transport;
    std::pmr::string userAgent;

    [[nodiscard]] bool offersSubprotocol(std::string_view selected) const noexcept {
        const std::string_view header = subprotocolHeader;
        for (const auto range : subprotocolRanges) {
            if (header.substr(range.offset, range.length) == selected) {
                return true;
            }
        }
        return false;
    }

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
          subprotocolHeader(resource),
          subprotocolRanges(resource),
          maxMessageBytes(source.maxMessageBytes),
          connectTimeout(source.connectTimeout),
          readTimeout(source.readTimeout),
          writeTimeout(source.writeTimeout),
          closeHandshakeTimeout(source.closeHandshakeTimeout),
          heartbeat(normalizeWebSocketHeartbeatConfig(source.heartbeat)),
          transport(clientTransportConfigView(source), resource),
          userAgent(source.userAgent, resource) {
        headers.reserve(source.headers.size());
        for (const auto& [name, value] : source.headers) {
            headers.emplace_back(name, value, resource);
        }
        subprotocolRanges.reserve(source.subprotocols.size());
        for (const auto& subprotocol : source.subprotocols) {
            if (!subprotocolHeader.empty()) {
                subprotocolHeader.append(", ");
            }
            const auto offset = subprotocolHeader.size();
            subprotocolHeader.append(subprotocol);
            subprotocolRanges.push_back({.offset = offset, .length = subprotocol.size()});
        }
    }
};

}  // namespace ruvia::detail
