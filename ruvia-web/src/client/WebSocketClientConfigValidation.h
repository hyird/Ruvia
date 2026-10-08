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
#include "ruvia/http/HttpRequestTarget.h"
#include "ruvia/web/WebSocketClient.h"

#include "client/ClientTransport.h"
#include "http3/Http3QpackConfigValidation.h"
#include "websocket/WebSocketHeartbeatConfigValidation.h"

namespace ruvia::detail {

inline void validateWebSocketClientConfig(const WebSocketClientConfig& config) {
    if (config.scheme != WebSocketScheme::kWs && config.scheme != WebSocketScheme::kWss) {
        throw std::invalid_argument("WebSocket client scheme is invalid");
    }
    if (config.protocol != WebSocketClientProtocol::kHttp1 && config.protocol != WebSocketClientProtocol::kHttp2 && config.protocol != WebSocketClientProtocol::kHttp3) {
        throw std::invalid_argument("WebSocket client HTTP protocol is invalid");
    }
    if (config.protocol == WebSocketClientProtocol::kHttp3 && config.scheme != WebSocketScheme::kWss) {
        throw std::invalid_argument("HTTP/3 WebSocket requires wss");
    }
    validateHttp3QpackConfig(config.qpack);
    validateClientOriginHost(
        config.host, "WebSocket client host must not be empty", "WebSocket client host is invalid");
    if (config.port.has_value() && config.port.value() == 0) {
        throw std::invalid_argument("WebSocket client port must be greater than zero");
    }
    if (!isValidHttpOriginFormTarget(config.target)) {
        throw std::invalid_argument("WebSocket client target must use origin-form");
    }
    if (config.maxMessageBytes == 0) {
        throw std::invalid_argument("WebSocket client max message bytes must be greater than zero");
    }
    if (config.connectTimeout.count() <= 0) {
        throw std::invalid_argument("WebSocket client connect timeout must be greater than zero");
    }
    for (const std::optional<std::chrono::milliseconds> timeout :
        {config.readTimeout, config.write_timeout, config.closeHandshakeTimeout}) {
        if (timeout.has_value() && timeout->count() <= 0) {
            throw std::invalid_argument("WebSocket client timeout must be greater than zero");
        }
    }
    validateWebSocketHeartbeatConfig(config.heartbeat);
    validateClientTransportConfig(clientTransportConfigView(config));

    if (config.compressionLevel < 0 || config.compressionLevel > 9) {
        throw std::invalid_argument("WebSocket client compression level must be between 0 and 9");
    }
}

inline void validate_web_socket_client_protocols_and_headers(
    std::span<const HttpHeaderView> headers, std::span<const std::string_view> subprotocols,
    std::string_view userAgent, const WebSocketClientDeflateOffer& deflate) {
    Http1WebSocketClientHandshake::validateConfiguration(headers, subprotocols, userAgent);
    WebSocketClientNegotiation::validate_configuration(
        {.headers = headers, .subprotocols = subprotocols, .deflate = deflate});
}

}  // namespace ruvia::detail
