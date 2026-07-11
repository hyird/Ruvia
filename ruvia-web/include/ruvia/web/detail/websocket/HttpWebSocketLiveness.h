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
        const auto closeTimeout = options.closeHandshakeTimeout.count();
        const bool awaitingPeerClose =
            closePhase == WsClosePhase::kLocalCloseQueued ||
            closePhase == WsClosePhase::kAwaitingPeerClose;
        return awaitingPeerClose && closeTimeout > 0 && localCloseStartedMs >= 0 &&
                now - localCloseStartedMs >= closeTimeout
            ? WebSocketLivenessDecision::kAbortTransport
            : WebSocketLivenessDecision::kIdle;
    }

    const auto pingInterval = options.pingInterval.count();
    if (pingInterval <= 0) {
        return WebSocketLivenessDecision::kIdle;
    }

    auto pongTimeout = options.pongTimeout.count();
    if (pongTimeout <= 0) {
        pongTimeout = pingInterval;
    }
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
