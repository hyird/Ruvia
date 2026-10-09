#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ruvia {

enum class websocket_liveness_mode : std::uint8_t { open,
    awaiting_peer_close,
    inactive };

enum class websocket_opcode : std::uint8_t {
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA
};
// Exact RFC 7692 negotiation, including independent sender dictionaries and
// windows. Omitted max-window parameters imply 15; presence also controls the
// negotiated extension's wire metadata.
struct websocket_compression final {
    bool enabled_{false};
    bool server_no_context_takeover_{true};
    bool client_no_context_takeover_{true};
    std::optional<int> server_max_window_bits_{};
    std::optional<int> client_max_window_bits_{};
    bool operator==(const websocket_compression&) const = default;
};

struct websocket_deflate_config final {
    bool enabled_{true};
    int compression_level_{6};
    // Opt-in: never mix secrets and attacker-controlled content in one dictionary.
    // A peer requesting no-context-takeover still receives independent messages.
    bool context_takeover_{false};
};

namespace detail {
class websocket_compression_extension final {
public:
    explicit constexpr websocket_compression_extension(websocket_compression compression) noexcept {
        if (!compression.enabled_) {
            return;
        }
        append("permessage-deflate");
        if (compression.server_no_context_takeover_) {
            append("; server_no_context_takeover");
        }
        if (compression.client_no_context_takeover_) {
            append("; client_no_context_takeover");
        }
        if (compression.server_max_window_bits_) {
            append("; server_max_window_bits=");
            number(*compression.server_max_window_bits_);
        }
        if (compression.client_max_window_bits_) {
            append("; client_max_window_bits=");
            number(*compression.client_max_window_bits_);
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
[[nodiscard]] constexpr websocket_compression_extension get_websocket_compression_extension(websocket_compression compression) noexcept {
    return websocket_compression_extension(compression);
}

struct websocket_message_access;
}  // namespace detail

class websocket_message final {
public:
    // Construct a message view over caller-owned payload storage. The payload
    // must remain valid until the next read on its connection or until the
    // caller otherwise stops using this view.
    [[nodiscard]] static constexpr websocket_message borrow(
        websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_message(opcode, payload_value);
    }

    websocket_message(const websocket_message&) noexcept = default;
    websocket_message& operator=(const websocket_message&) noexcept = default;
    websocket_message(websocket_message&&) noexcept = default;
    websocket_message& operator=(websocket_message&&) noexcept = default;

    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }

    // The payload view borrows the connection read buffer: it is valid only
    // until the next websocket read() on the same connection (the same rule as
    // body_reader::read). Copy the payload before the next read if it must
    // outlive the current message.
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] bool text() const noexcept {
        return opcode_ == websocket_opcode::text;
    }

    [[nodiscard]] bool binary() const noexcept {
        return opcode_ == websocket_opcode::binary;
    }

private:
    friend struct detail::websocket_message_access;

    constexpr websocket_message() noexcept = default;

    constexpr websocket_message(websocket_opcode opcode, std::string_view payload_value) noexcept
        : opcode_(opcode),
          payload_(payload_value) {}

    websocket_opcode opcode_{websocket_opcode::text};
    std::string_view payload_;
};

}  // namespace ruvia
