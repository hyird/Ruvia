#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/WebSocketProtocol.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ruvia {

// Runtime-only liveness policy. Wire framing and close-handshake state remain in
// ruvia-http; timers and transport abort policy belong to the Web runtime.
struct WebSocketLifecycleOptions final {
    std::chrono::milliseconds pingInterval{0};
    std::chrono::milliseconds pongTimeout{0};
    // A locally initiated Close waits for the peer Close before the underlying
    // transport is ended. Zero disables this guard.
    std::chrono::milliseconds closeHandshakeTimeout{std::chrono::seconds(5)};
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
    using Read = Task<std::optional<WebSocketMessage>> (*)(void*);
    using Write = Task<void> (*)(void*, WebSocketOpcode, std::string_view);
    using Close = Task<void> (*)(void*, std::uint16_t, std::string_view);

    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    [[nodiscard]] Task<std::optional<WebSocketMessage>> read();

    Task<void> text(std::string_view payload);

    Task<void> binary(std::string_view payload);

    Task<void> pong(std::string_view payload);

    Task<void> ping(std::string_view payload = {});

    Task<void> close(std::uint16_t code = 1000, std::string_view reason = {});

private:
    friend struct detail::WebSocketAccess;

    constexpr WebSocket(void* target, Read read, Write write, Close close) noexcept
        : target_(target), read_(read), write_(write), close_(close) {}

    Task<void> write(WebSocketOpcode opcode, std::string_view payload);

    void* target_;
    Read read_;
    Write write_;
    Close close_;
};

}  // namespace ruvia
