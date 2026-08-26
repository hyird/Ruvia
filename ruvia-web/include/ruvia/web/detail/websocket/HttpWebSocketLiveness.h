#pragma once

#include <cstdint>
#include <variant>

#include "ruvia/http/detail/websocket/WsConnection.h"
#include "ruvia/web/WebSocket.h"

namespace ruvia::detail {

enum class WebSocketLivenessDecision : std::uint8_t {
    kIdle,
    kSendPing,
    kAbortTransport,
};

class WebSocketLivenessIdle final {};

// The ping write and its Pong wait are separate states. A Pong may arrive as
// soon as the transport accepts the bytes and before the write coroutine is
// resumed, so the in-flight state must be visible to the reader without
// starting the timeout prematurely.
class WebSocketSendingPing final {};

class WebSocketAwaitingPong final {
public:
    explicit WebSocketAwaitingPong(std::int64_t sentAtMs) noexcept
        : sentAtMs_(sentAtMs) {}

    [[nodiscard]] std::int64_t sentAtMs() const noexcept {
        return sentAtMs_;
    }

private:
    std::int64_t sentAtMs_;
};

class WebSocketAwaitingPeerClose final {
public:
    explicit WebSocketAwaitingPeerClose(std::int64_t startedAtMs) noexcept
        : startedAtMs_(startedAtMs) {}

    [[nodiscard]] std::int64_t startedAtMs() const noexcept {
        return startedAtMs_;
    }

private:
    std::int64_t startedAtMs_;
};

using WebSocketLivenessState = std::variant<WebSocketLivenessIdle, WebSocketSendingPing,
    WebSocketAwaitingPong, WebSocketAwaitingPeerClose>;

[[nodiscard]] inline WebSocketLivenessDecision webSocketLivenessDecision(
    const WebSocketLifecycleOptions& options, WsLivenessMode livenessMode,
    const WebSocketLivenessState& state, bool writeActive, std::int64_t lastActiveMs,
    std::int64_t now) noexcept {
    if (livenessMode != WsLivenessMode::kOpen) {
        const auto* close = std::get_if<WebSocketAwaitingPeerClose>(&state);
        return livenessMode == WsLivenessMode::kAwaitingPeerClose && close != nullptr &&
                       options.closeHandshakeTimeout.has_value() &&
                       now - close->startedAtMs() >= options.closeHandshakeTimeout->count()
                   ? WebSocketLivenessDecision::kAbortTransport
                   : WebSocketLivenessDecision::kIdle;
    }

    if (!options.heartbeat.pingInterval.has_value()) {
        return WebSocketLivenessDecision::kIdle;
    }

    const auto pingInterval = options.heartbeat.pingInterval->count();
    const auto pongTimeout = options.heartbeat.pongTimeout->count();
    if (const auto* pong = std::get_if<WebSocketAwaitingPong>(&state)) {
        return now - pong->sentAtMs() >= pongTimeout ? WebSocketLivenessDecision::kAbortTransport
                                                     : WebSocketLivenessDecision::kIdle;
    }
    if (!std::holds_alternative<WebSocketLivenessIdle>(state)) {
        return WebSocketLivenessDecision::kIdle;
    }
    if (now - lastActiveMs < pingInterval || writeActive) {
        return WebSocketLivenessDecision::kIdle;
    }
    return WebSocketLivenessDecision::kSendPing;
}

}  // namespace ruvia::detail
