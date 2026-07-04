#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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

namespace detail {
struct WebSocketMessageAccess;
}  // namespace detail

class WebSocketMessage final {
public:
    WebSocketMessage(const WebSocketMessage&) noexcept = default;
    WebSocketMessage& operator=(const WebSocketMessage&) noexcept = default;
    WebSocketMessage(WebSocketMessage&&) noexcept = default;
    WebSocketMessage& operator=(WebSocketMessage&&) noexcept = default;

    [[nodiscard]] constexpr WebSocketOpcode opcode() const noexcept {
        return opcode_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] bool text() const noexcept {
        return opcode_ == WebSocketOpcode::kText;
    }

    [[nodiscard]] bool binary() const noexcept {
        return opcode_ == WebSocketOpcode::kBinary;
    }

private:
    friend struct detail::WebSocketMessageAccess;

    constexpr WebSocketMessage() noexcept = default;

    constexpr WebSocketMessage(WebSocketOpcode opcode, std::string_view payload) noexcept
        : opcode_(opcode), payload_(payload) {}

    WebSocketOpcode opcode_{WebSocketOpcode::kText};
    std::string_view payload_;
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

    [[nodiscard]] Task<std::optional<WebSocketMessage>> read() {
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
        return close_(target_, code, reason);
    }

private:
    Task<void> write(WebSocketOpcode opcode, std::string_view payload) {
        return write_(target_, opcode, payload);
    }

    void* target_;
    Read read_;
    Write write_;
    Close close_;
};

}  // namespace ruvia
