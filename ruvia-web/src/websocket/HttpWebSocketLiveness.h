#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>

#include "ruvia/http/WebSocketProtocol.h"
#include "ruvia/web/WebSocket.h"

namespace ruvia::detail {

[[nodiscard]] inline std::int64_t webSocketSteadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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
class WebSocketSendingPing final {
public:
    explicit WebSocketSendingPing(std::uint64_t challenge) noexcept
        : challenge_(challenge) {}

    [[nodiscard]] std::uint64_t challenge() const noexcept {
        return challenge_;
    }

private:
    std::uint64_t challenge_;
};

class WebSocketAwaitingPong final {
public:
    explicit WebSocketAwaitingPong(std::int64_t sentAtMs, std::uint64_t challenge) noexcept
        : sentAtMs_(sentAtMs),
          challenge_(challenge) {}

    [[nodiscard]] std::int64_t sentAtMs() const noexcept {
        return sentAtMs_;
    }

    [[nodiscard]] std::uint64_t challenge() const noexcept {
        return challenge_;
    }

private:
    std::int64_t sentAtMs_;
    std::uint64_t challenge_;
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

[[nodiscard]] inline std::array<char, 8> webSocketHeartbeatPayload(std::uint64_t challenge) noexcept {
    std::array<char, 8> payload{};
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(challenge >> ((payload.size() - 1 - i) * 8));
    }
    return payload;
}

[[nodiscard]] inline bool webSocketHeartbeatPongMatches(
    const WebSocketLivenessState& state, std::string_view payload) noexcept {
    std::uint64_t challenge;
    if (const auto* sending = std::get_if<WebSocketSendingPing>(&state)) {
        challenge = sending->challenge();
    } else if (const auto* awaiting = std::get_if<WebSocketAwaitingPong>(&state)) {
        challenge = awaiting->challenge();
    } else {
        return false;
    }
    const auto expected = webSocketHeartbeatPayload(challenge);
    return payload == std::string_view(expected.data(), expected.size());
}

[[nodiscard]] inline WebSocketLivenessDecision webSocketLivenessDecision(
    const WebSocketLifecycleOptions& options, WebSocketLivenessMode livenessMode,
    const WebSocketLivenessState& state, bool writeActive, std::int64_t lastActiveMs,
    std::int64_t now) noexcept {
    if (livenessMode != WebSocketLivenessMode::kOpen) {
        const auto* close = std::get_if<WebSocketAwaitingPeerClose>(&state);
        return livenessMode == WebSocketLivenessMode::kAwaitingPeerClose && close != nullptr &&
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
