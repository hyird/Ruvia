#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ruvia {

enum class WebSocketLivenessMode : std::uint8_t { kOpen,
    kAwaitingPeerClose,
    kInactive };

enum class WebSocketOpcode : std::uint8_t {
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA
};
// Exact RFC 7692 negotiation, including independent sender dictionaries and
// windows. Omitted max-window parameters imply 15; presence also controls the
// negotiated extension's wire metadata.
struct WebSocketCompression final {
    bool enabled{false};
    bool serverNoContextTakeover{true};
    bool clientNoContextTakeover{true};
    std::optional<int> serverMaxWindowBits{};
    std::optional<int> clientMaxWindowBits{};
    bool operator==(const WebSocketCompression&) const = default;
};

struct WebSocketDeflateConfig final {
    bool enabled{true};
    int compressionLevel{6};
    // Opt-in: never mix secrets and attacker-controlled content in one dictionary.
    // A peer requesting no-context-takeover still receives independent messages.
    bool contextTakeover{false};
};

namespace detail {
class WebSocketCompressionExtension final {
public:
    explicit constexpr WebSocketCompressionExtension(WebSocketCompression compression) noexcept {
        if (!compression.enabled) {
            return;
        }
        append("permessage-deflate");
        if (compression.serverNoContextTakeover) {
            append("; server_no_context_takeover");
        }
        if (compression.clientNoContextTakeover) {
            append("; client_no_context_takeover");
        }
        if (compression.serverMaxWindowBits) {
            append("; server_max_window_bits=");
            number(*compression.serverMaxWindowBits);
        }
        if (compression.clientMaxWindowBits) {
            append("; client_max_window_bits=");
            number(*compression.clientMaxWindowBits);
        }
    }
    [[nodiscard]] constexpr std::string_view view() const& noexcept {
        return {bytes_.data(), size_};
    }
    std::string_view view() const&& = delete;
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }

private:
    constexpr void append(std::string_view text) noexcept {
        for (char ch : text) {
            bytes_[size_++] = ch;
        }
    }
    constexpr void number(int value) noexcept {
        if (value >= 10) {
            bytes_[size_++] = '1';
        }
        bytes_[size_++] = static_cast<char>('0' + value % 10);
    }
    std::array<char, 160> bytes_{};
    std::size_t size_{0};
};
[[nodiscard]] constexpr WebSocketCompressionExtension webSocketCompressionExtension(WebSocketCompression compression) noexcept {
    return WebSocketCompressionExtension(compression);
}

struct WebSocketMessageAccess;
}  // namespace detail

class WebSocketMessage final {
public:
    // Construct a message view over caller-owned payload storage. The payload
    // must remain valid until the next read on its connection or until the
    // caller otherwise stops using this view.
    [[nodiscard]] static constexpr WebSocketMessage borrow(
        WebSocketOpcode opcode, std::string_view payload) noexcept {
        return WebSocketMessage(opcode, payload);
    }

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
