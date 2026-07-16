#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/WebSocketProtocol.h"
#include "ruvia/web/ScopedOperation.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace ruvia {

class WebSocketHeartbeatPolicy final {
public:
    [[nodiscard]] static WebSocketHeartbeatPolicy periodic(
        std::chrono::milliseconds pingInterval,
        std::chrono::milliseconds pongTimeout) {
        if (pingInterval.count() <= 0 || pongTimeout.count() <= 0) {
            throw std::invalid_argument(
                "websocket heartbeat intervals must be greater than zero");
        }
        return WebSocketHeartbeatPolicy(pingInterval, pongTimeout);
    }

    [[nodiscard]] static WebSocketHeartbeatPolicy periodic(
        std::chrono::milliseconds interval) {
        return periodic(interval, interval);
    }

    [[nodiscard]] std::chrono::milliseconds pingInterval() const noexcept {
        return pingInterval_;
    }

    [[nodiscard]] std::chrono::milliseconds pongTimeout() const noexcept {
        return pongTimeout_;
    }

private:
    WebSocketHeartbeatPolicy(
        std::chrono::milliseconds pingInterval,
        std::chrono::milliseconds pongTimeout) noexcept
        : pingInterval_(pingInterval),
          pongTimeout_(pongTimeout) {}

    std::chrono::milliseconds pingInterval_;
    std::chrono::milliseconds pongTimeout_;
};

// Runtime-only liveness policy. Wire framing and close-handshake state remain in
// ruvia-http; timers and transport abort policy belong to the Web runtime.
struct WebSocketLifecycleOptions final {
    std::optional<WebSocketHeartbeatPolicy> heartbeat;
    // A locally initiated Close waits for the peer Close before the underlying
    // transport is ended. nullopt disables this guard.
    std::optional<std::chrono::milliseconds> closeHandshakeTimeout{
        std::chrono::seconds(5)};
};

struct WebSocketRouteOptions final {
    std::string_view subprotocols;
    WebSocketLifecycleOptions lifecycle{};
};

namespace detail {
struct WebSocketAccess;
}  // namespace detail

class WebSocket final {
public:
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    [[nodiscard]] ScopedOperation<std::optional<WebSocketMessage>> read();

    ScopedOperation<void> text(std::string_view payload);

    ScopedOperation<void> binary(std::string_view payload);

    ScopedOperation<void> pong(std::string_view payload);

    ScopedOperation<void> ping(std::string_view payload = {});

    ScopedOperation<void> close(std::uint16_t code = 1000, std::string_view reason = {});
    void abort() noexcept;

private:
    friend struct detail::WebSocketAccess;

    using Read = Task<std::optional<WebSocketMessage>> (*)(void*);
    using Write = Task<void> (*)(void*, WebSocketOpcode, std::string_view);
    using Close = Task<void> (*)(void*, std::uint16_t, std::string_view);
    using Abort = void (*)(void*) noexcept;

    WebSocket(void* target, Read read, Write write, Close close, Abort abort) noexcept
        : target_(target), read_(read), write_(write), close_(close), abort_(abort) {}

    ScopedOperation<void> write(WebSocketOpcode opcode, std::string_view payload);

    void* target_;
    Read read_;
    Write write_;
    Close close_;
    Abort abort_;
    detail::ScopedOperationScope operationScope_;
};

}  // namespace ruvia
