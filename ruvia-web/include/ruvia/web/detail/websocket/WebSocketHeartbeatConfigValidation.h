#pragma once

#include <stdexcept>

#include "ruvia/web/WebSocket.h"

namespace ruvia::detail {

inline void validateWebSocketHeartbeatConfig(const WebSocketHeartbeatConfig& config) {
    if (!config.pingInterval.has_value()) {
        if (config.pongTimeout.has_value()) {
            throw std::invalid_argument("websocket pong timeout requires a ping interval");
        }
        return;
    }
    if (config.pingInterval->count() <= 0 || (config.pongTimeout.has_value() && config.pongTimeout->count() <= 0)) {
        throw std::invalid_argument("websocket heartbeat intervals must be greater than zero");
    }
}

[[nodiscard]] inline WebSocketHeartbeatConfig normalizeWebSocketHeartbeatConfig(WebSocketHeartbeatConfig config) {
    validateWebSocketHeartbeatConfig(config);
    if (config.pingInterval.has_value() && !config.pongTimeout.has_value()) {
        config.pongTimeout = config.pingInterval;
    }
    return config;
}

}  // namespace ruvia::detail
