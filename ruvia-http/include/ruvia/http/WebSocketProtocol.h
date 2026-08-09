#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia {

enum class WebSocketOpcode : std::uint8_t { kText = 0x1, kBinary = 0x2, kClose = 0x8, kPing = 0x9, kPong = 0xA };

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

    // The payload view borrows the connection read buffer: it is valid only
    // until the next WebSocket read() on the same connection (the same rule as
    // BodyReader::read). Copy the payload before the next read if it must
    // outlive the current message.
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
        : opcode_(opcode),
          payload_(payload) {}

    WebSocketOpcode opcode_{WebSocketOpcode::kText};
    std::string_view payload_;
};

}  // namespace ruvia
