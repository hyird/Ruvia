#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/WebSocketServerProtocolTypes.h"

namespace ruvia::detail {

class WsConnection;

using WsEventKind = ::ruvia::WebSocketServerEventKind;
using WsMessageEvent = ::ruvia::WebSocketServerMessageEvent;
using WsPingEvent = ::ruvia::WebSocketServerPingEvent;
using WsPongEvent = ::ruvia::WebSocketServerPongEvent;
using WsCloseEvent = ::ruvia::WebSocketServerCloseEvent;
using WsProtocolErrorEvent = ::ruvia::WebSocketServerProtocolErrorEvent;
using WsTransportEndEvent = ::ruvia::WebSocketServerTransportEndEvent;

// A zero-allocation discriminated event. poll() uses std::optional for the
// need-input result, so every materialized WsEvent has exactly one valid payload.
// Borrowed payload/reason views remain valid until the next poll() call.
class WsEvent final {
public:
    [[nodiscard]] WsEventKind kind() const noexcept {
        return static_cast<WsEventKind>(value_.index());
    }

    [[nodiscard]] const WsMessageEvent* message() const& noexcept {
        return std::get_if<WsMessageEvent>(&value_);
    }
    [[nodiscard]] const WsMessageEvent* message() const&& = delete;

    [[nodiscard]] const WsPingEvent* ping() const& noexcept {
        return std::get_if<WsPingEvent>(&value_);
    }
    [[nodiscard]] const WsPingEvent* ping() const&& = delete;

    [[nodiscard]] const WsPongEvent* pong() const& noexcept {
        return std::get_if<WsPongEvent>(&value_);
    }
    [[nodiscard]] const WsPongEvent* pong() const&& = delete;

    [[nodiscard]] const WsCloseEvent* close() const& noexcept {
        return std::get_if<WsCloseEvent>(&value_);
    }
    [[nodiscard]] const WsCloseEvent* close() const&& = delete;

    [[nodiscard]] const WsProtocolErrorEvent* protocolError() const& noexcept {
        return std::get_if<WsProtocolErrorEvent>(&value_);
    }
    [[nodiscard]] const WsProtocolErrorEvent* protocolError() const&& = delete;

    [[nodiscard]] const WsTransportEndEvent* transportEnd() const& noexcept {
        return std::get_if<WsTransportEndEvent>(&value_);
    }
    [[nodiscard]] const WsTransportEndEvent* transportEnd() const&& = delete;

private:
    friend class WsConnection;

    using Value = std::variant<WsMessageEvent, WsPingEvent, WsPongEvent, WsCloseEvent,
        WsProtocolErrorEvent, WsTransportEndEvent>;

    static_assert(
        std::to_underlying(WsEventKind::kTransportEnd) + 1 == std::variant_size_v<Value>);

    template <typename Event>
    explicit WsEvent(Event event) noexcept
        : value_(std::move(event)) {}

    [[nodiscard]] static WsEvent message(
        WebSocketOpcode opcode, std::string_view payload) noexcept {
        return WsEvent(WsMessageEvent(opcode, payload));
    }

    [[nodiscard]] static WsEvent ping(std::string_view payload) noexcept {
        return WsEvent(WsPingEvent(payload));
    }

    [[nodiscard]] static WsEvent pong(std::string_view payload) noexcept {
        return WsEvent(WsPongEvent(payload));
    }

    [[nodiscard]] static WsEvent close(std::uint16_t closeCode, std::string_view reason) noexcept {
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
