#pragma once

#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ruvia/http/Http1WebSocketClientHandshake.h"
#include "ruvia/http/HttpClientRequestTarget.h"
#include "ruvia/web/WebSocketClient.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/websocket/WebSocketHeartbeatConfigValidation.h"

namespace ruvia::detail {

inline void validateWebSocketClientConfig(const WebSocketClientConfig& config) {
    if (config.scheme != WebSocketScheme::kWs && config.scheme != WebSocketScheme::kWss) {
        throw std::invalid_argument("WebSocket client scheme is invalid");
    }
    validateClientOriginHost(
        config.host, "WebSocket client host must not be empty", "WebSocket client host is invalid");
    if (config.port.has_value() && config.port.value() == 0) {
        throw std::invalid_argument("WebSocket client port must be greater than zero");
    }
    if (!isValidHttpClientOriginTarget(config.target)) {
        throw std::invalid_argument("WebSocket client target must use origin-form");
    }
    if (config.maxMessageBytes == 0) {
        throw std::invalid_argument("WebSocket client max message bytes must be greater than zero");
    }
    if (config.connectTimeout.count() <= 0) {
        throw std::invalid_argument("WebSocket client connect timeout must be greater than zero");
    }
    for (const std::optional<std::chrono::milliseconds> timeout :
        {config.readTimeout, config.writeTimeout, config.closeHandshakeTimeout}) {
        if (timeout.has_value() && timeout->count() <= 0) {
            throw std::invalid_argument("WebSocket client timeout must be greater than zero");
        }
    }
    validateWebSocketHeartbeatConfig(config.heartbeat);
    validateClientTransportConfig(clientTransportConfigView(config));

    std::pmr::vector<HttpHeaderView> headers(std::pmr::get_default_resource());
    headers.reserve(config.headers.size());
    for (const auto& [name, value] : config.headers) {
        headers.emplace_back(name, value);
    }
    std::pmr::vector<std::string_view> subprotocols(std::pmr::get_default_resource());
    subprotocols.reserve(config.subprotocols.size());
    for (const auto& subprotocol : config.subprotocols) {
        subprotocols.push_back(subprotocol);
    }
    Http1WebSocketClientHandshake::validateConfiguration(headers, subprotocols, config.userAgent);
}

}  // namespace ruvia::detail
