#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/WebSocketProtocol.h"

namespace ruvia::detail {

class WsConnection;

enum class WsEventKind : std::uint8_t {
    kMessage,
    kPing,
    kPong,
    kClose,
    kProtocolError,
    kTransportEnd,
};

class WsMessageEvent final {
public:
    [[nodiscard]] constexpr WebSocketOpcode opcode() const noexcept {
        return opcode_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class WsEvent;

    constexpr WsMessageEvent(
        WebSocketOpcode opcode,
        std::string_view payload) noexcept
        : opcode_(opcode), payload_(payload) {}

    WebSocketOpcode opcode_;
    std::string_view payload_;
};

class WsPingEvent final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class WsEvent;

    explicit constexpr WsPingEvent(std::string_view payload) noexcept
        : payload_(payload) {}

    std::string_view payload_;
};

class WsPongEvent final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class WsEvent;

    explicit constexpr WsPongEvent(std::string_view payload) noexcept
        : payload_(payload) {}

    std::string_view payload_;
};

class WsCloseEvent final {
public:
    // RFC 6455 section 7.1.5 uses 1005 when the peer supplied no status code;
    // section 7.4.1 reserves it from ever being emitted on wire.
    [[nodiscard]] constexpr std::uint16_t closeCode() const noexcept {
        return closeCode_;
    }

    [[nodiscard]] constexpr std::string_view reason() const noexcept {
        return reason_;
    }

private:
    friend class WsEvent;

    constexpr WsCloseEvent(
        std::uint16_t closeCode,
        std::string_view reason) noexcept
        : closeCode_(closeCode), reason_(reason) {}

    std::uint16_t closeCode_;
    std::string_view reason_;
};

class WsProtocolErrorEvent final {
public:
    // The RFC 6455 close code queued by the protocol core for this failure.
    [[nodiscard]] constexpr std::uint16_t closeCode() const noexcept {
        return closeCode_;
    }

private:
    friend class WsEvent;

    explicit constexpr WsProtocolErrorEvent(std::uint16_t closeCode) noexcept
        : closeCode_(closeCode) {}

    std::uint16_t closeCode_;
};

class WsTransportEndEvent final {
private:
    friend class WsEvent;
    constexpr WsTransportEndEvent() noexcept = default;
};

// A zero-allocation discriminated event. poll() uses std::optional for the
// need-input result, so every materialized WsEvent has exactly one valid payload.
// Borrowed payload/reason views remain valid until the next poll() call.
class WsEvent final {
public:
    [[nodiscard]] WsEventKind kind() const noexcept {
        return static_cast<WsEventKind>(value_.index());
    }

    [[nodiscard]] const WsMessageEvent* message() const & noexcept {
        return std::get_if<WsMessageEvent>(&value_);
    }
    [[nodiscard]] const WsMessageEvent* message() const && = delete;

    [[nodiscard]] const WsPingEvent* ping() const & noexcept {
        return std::get_if<WsPingEvent>(&value_);
    }
    [[nodiscard]] const WsPingEvent* ping() const && = delete;

    [[nodiscard]] const WsPongEvent* pong() const & noexcept {
        return std::get_if<WsPongEvent>(&value_);
    }
    [[nodiscard]] const WsPongEvent* pong() const && = delete;

    [[nodiscard]] const WsCloseEvent* close() const & noexcept {
        return std::get_if<WsCloseEvent>(&value_);
    }
    [[nodiscard]] const WsCloseEvent* close() const && = delete;

    [[nodiscard]] const WsProtocolErrorEvent*
    protocolError() const & noexcept {
        return std::get_if<WsProtocolErrorEvent>(&value_);
    }
    [[nodiscard]] const WsProtocolErrorEvent*
    protocolError() const && = delete;

    [[nodiscard]] const WsTransportEndEvent* transportEnd() const & noexcept {
        return std::get_if<WsTransportEndEvent>(&value_);
    }
    [[nodiscard]] const WsTransportEndEvent* transportEnd() const && = delete;

private:
    friend class WsConnection;

    using Value = std::variant<
        WsMessageEvent,
        WsPingEvent,
        WsPongEvent,
        WsCloseEvent,
        WsProtocolErrorEvent,
        WsTransportEndEvent>;

    static_assert(
        static_cast<std::size_t>(WsEventKind::kTransportEnd) + 1 ==
        std::variant_size_v<Value>);

    template <typename Event>
    explicit WsEvent(Event event) noexcept
        : value_(std::move(event)) {}

    [[nodiscard]] static WsEvent message(
        WebSocketOpcode opcode,
        std::string_view payload) noexcept {
        return WsEvent(WsMessageEvent(opcode, payload));
    }

    [[nodiscard]] static WsEvent ping(std::string_view payload) noexcept {
        return WsEvent(WsPingEvent(payload));
    }

    [[nodiscard]] static WsEvent pong(std::string_view payload) noexcept {
        return WsEvent(WsPongEvent(payload));
    }

    [[nodiscard]] static WsEvent close(
        std::uint16_t closeCode,
        std::string_view reason) noexcept {
        return WsEvent(WsCloseEvent(closeCode, reason));
    }

    [[nodiscard]] static WsEvent protocolError(std::uint16_t closeCode) noexcept {
        return WsEvent(WsProtocolErrorEvent(closeCode));
    }

    [[nodiscard]] static WsEvent makeTransportEnd() noexcept {
        return WsEvent(WsTransportEndEvent());
    }

    Value value_;
};

}  // namespace ruvia::detail
