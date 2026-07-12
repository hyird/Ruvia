#pragma once

#include <cstdint>

#include "ruvia/http/detail/websocket/WsConnection.h"
#include "ruvia/web/WebSocket.h"

namespace ruvia::detail {

enum class WebSocketLivenessDecision : std::uint8_t {
    kIdle,
    kSendPing,
    kAbortTransport,
};

[[nodiscard]] inline WebSocketLivenessDecision webSocketLivenessDecision(
    const WebSocketLifecycleOptions& options,
    WsClosePhase closePhase,
    bool awaitingPong,
    bool writeActive,
    std::int64_t lastActiveMs,
    std::int64_t heartbeatPingSentMs,
    std::int64_t localCloseStartedMs,
    std::int64_t now) noexcept {
    if (closePhase != WsClosePhase::kOpen) {
        const bool awaitingPeerClose =
            closePhase == WsClosePhase::kLocalCloseQueued ||
            closePhase == WsClosePhase::kAwaitingPeerClose;
        return awaitingPeerClose &&
                options.closeHandshakeTimeout.has_value() &&
                localCloseStartedMs >= 0 &&
                now - localCloseStartedMs >=
                    options.closeHandshakeTimeout->count()
            ? WebSocketLivenessDecision::kAbortTransport
            : WebSocketLivenessDecision::kIdle;
    }

    if (!options.heartbeat.has_value()) {
        return WebSocketLivenessDecision::kIdle;
    }

    const auto pingInterval = options.heartbeat->pingInterval().count();
    const auto pongTimeout = options.heartbeat->pongTimeout().count();
    if (awaitingPong) {
        return now - heartbeatPingSentMs >= pongTimeout
            ? WebSocketLivenessDecision::kAbortTransport
            : WebSocketLivenessDecision::kIdle;
    }
    if (now - lastActiveMs < pingInterval || writeActive) {
        return WebSocketLivenessDecision::kIdle;
    }
    return WebSocketLivenessDecision::kSendPing;
}

}  // namespace ruvia::detail
