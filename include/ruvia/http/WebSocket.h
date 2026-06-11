#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/app/Task.h"

namespace ruvia {

enum class WebSocketOpcode : std::uint8_t {
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA
};

struct WebSocketMessage final {
    WebSocketOpcode opcode{WebSocketOpcode::kText};
    std::string_view payload;

    [[nodiscard]] bool text() const noexcept {
        return opcode == WebSocketOpcode::kText;
    }

    [[nodiscard]] bool binary() const noexcept {
        return opcode == WebSocketOpcode::kBinary;
    }
};

struct WebSocketHeartbeatOptions final {
    std::chrono::milliseconds pingInterval{0};
    std::chrono::milliseconds pongTimeout{0};
};

struct WebSocketRouteOptions final {
    std::string_view subprotocols;
    WebSocketHeartbeatOptions heartbeat{};
};

class WebSocket final {
public:
    using Read = Task<std::optional<WebSocketMessage>> (*)(void*);
    using Write = Task<void> (*)(void*, WebSocketOpcode, std::string_view);
    using Close = Task<void> (*)(void*, std::uint16_t, std::string_view);

    constexpr WebSocket(void* target, Read read, Write write, Close close) noexcept
        : target_(target), read_(read), write_(write), close_(close) {}

    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return read_ != nullptr && write_ != nullptr && close_ != nullptr;
    }

    [[nodiscard]] Task<std::optional<WebSocketMessage>> read() {
        if (read_ == nullptr) {
            throw std::logic_error("websocket is not available");
        }
        return read_(target_);
    }

    Task<void> text(std::string_view payload) {
        return write(WebSocketOpcode::kText, payload);
    }

    Task<void> binary(std::string_view payload) {
        return write(WebSocketOpcode::kBinary, payload);
    }

    Task<void> pong(std::string_view payload) {
        return write(WebSocketOpcode::kPong, payload);
    }

    Task<void> ping(std::string_view payload = {}) {
        return write(WebSocketOpcode::kPing, payload);
    }

    Task<void> close(std::uint16_t code = 1000, std::string_view reason = {}) {
        if (close_ == nullptr) {
            throw std::logic_error("websocket is not available");
        }
        return close_(target_, code, reason);
    }

private:
    Task<void> write(WebSocketOpcode opcode, std::string_view payload) {
        if (write_ == nullptr) {
            throw std::logic_error("websocket is not available");
        }
        return write_(target_, opcode, payload);
    }

    void* target_{nullptr};
    Read read_{nullptr};
    Write write_{nullptr};
    Close close_{nullptr};
};

}  // namespace ruvia
