#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/WebSocketClient.h"

#include "client/ClientTransport.h"
#include "client/WebSocketClientConfigValidation.h"

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
    WebSocketClientProtocol protocol;
    std::pmr::string host;
    std::optional<std::uint16_t> port;
    std::pmr::string target;
    std::pmr::vector<WebSocketClientStoredHeader> headers;
    std::pmr::vector<std::pmr::string> subprotocols;
    WebSocketClientDeflateOffer deflate{};
    int compressionLevel{6};
    Http3QpackConfig qpack{};
    std::size_t maxMessageBytes;
    std::chrono::milliseconds connectTimeout;
    std::optional<std::chrono::milliseconds> readTimeout;
    std::optional<std::chrono::milliseconds> write_timeout;
    std::optional<std::chrono::milliseconds> closeHandshakeTimeout;
    WebSocketHeartbeatConfig heartbeat;
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
          protocol(source.protocol),
          host(source.host, resource),
          port(source.port),
          target(source.target, resource),
          headers(resource),
          subprotocols(resource),
          deflate(source.deflate),
          compressionLevel(source.compressionLevel),
          qpack(source.qpack),
          maxMessageBytes(source.maxMessageBytes),
          connectTimeout(source.connectTimeout),
          readTimeout(source.readTimeout),
          write_timeout(source.write_timeout),
          closeHandshakeTimeout(source.closeHandshakeTimeout),
          heartbeat(normalizeWebSocketHeartbeatConfig(source.heartbeat)),
          transport(clientTransportConfigView(source), resource),
          userAgent(source.userAgent, resource) {
        headers.reserve(source.headers.size());
        for (const auto& [name, value] : source.headers) {
            headers.emplace_back(name, value, resource);
        }
        subprotocols.reserve(source.subprotocols.size());
        for (const auto& subprotocol : source.subprotocols) {
            subprotocols.emplace_back(subprotocol);
        }
        std::pmr::vector<HttpHeaderView> header_views(resource);
        header_views.reserve(headers.size());
        for (const auto& header : headers) {
            header_views.emplace_back(header.name, header.value);
        }
        std::pmr::vector<std::string_view> protocol_views(resource);
        protocol_views.reserve(subprotocols.size());
        for (const auto& subprotocol : subprotocols) {
            protocol_views.emplace_back(subprotocol);
        }
        validate_web_socket_client_protocols_and_headers(header_views, protocol_views, userAgent, deflate);
    }
};

}  // namespace ruvia::detail
