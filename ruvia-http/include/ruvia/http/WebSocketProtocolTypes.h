#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/WebSocketProtocol.h"

namespace ruvia {

namespace detail {
class WsConnection;
}  // namespace detail

enum class WebSocketTransportDisposition : std::uint8_t { kKeepOpen,
    kEndTransport };
enum class WebSocketFrameSubmitStatus : std::uint8_t {
    kAccepted,
    kNotOpen,
    kInvalidOpcode,
    kMessageTooLarge,
    kInvalidTextPayload,
    kControlFrameTooLarge
};
enum class WebSocketCloseSubmitStatus : std::uint8_t {
    kAccepted,
    kAlreadyClosing,
    kClosed,
    kInvalidCode,
    kInvalidReason,
    kReasonTooLarge
};
enum class WebSocketAbortDisposition : std::uint8_t { kAbortTransport,
    kNoTransportAction };
enum class WebSocketOutputConsumeStatus : std::uint8_t { kPending,
    kDrained,
    kOutOfRange };
enum class WebSocketConnectionRole : std::uint8_t { kServer,
    kClient };
using WebSocketMaskKey = std::array<char, 4>;
// Client transports must supply a fresh cryptographically random key per call.
// The context is borrowed and must outlive the connection. Failure is fatal to
// the transport: operations may throw and the caller must abort the connection.
using WebSocketMaskKeyGenerator = bool (*)(void*, WebSocketMaskKey&) noexcept;

class WebSocketOutputPlan final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] constexpr WebSocketTransportDisposition disposition() const noexcept {
        return disposition_;
    }

private:
    friend class detail::WsConnection;
    constexpr WebSocketOutputPlan(std::string_view bytes, WebSocketTransportDisposition disposition) noexcept
        : bytes_(bytes),
          disposition_(disposition) {}
    std::string_view bytes_;
    WebSocketTransportDisposition disposition_;
};

enum class WebSocketEventKind : std::uint8_t { kMessage,
    kPing,
    kPong,
    kClose,
    kProtocolError,
    kTransportEnd };

class WebSocketMessageEvent final {
public:
    [[nodiscard]] constexpr WebSocketOpcode opcode() const noexcept {
        return opcode_;
    }
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class WebSocketEvent;
    constexpr WebSocketMessageEvent(WebSocketOpcode opcode, std::string_view payload) noexcept
        : opcode_(opcode),
          payload_(payload) {}
    WebSocketOpcode opcode_;
    std::string_view payload_;
};

class WebSocketPingEvent final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class WebSocketEvent;
    explicit constexpr WebSocketPingEvent(std::string_view payload) noexcept
        : payload_(payload) {}
    std::string_view payload_;
};

class WebSocketPongEvent final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class WebSocketEvent;
    explicit constexpr WebSocketPongEvent(std::string_view payload) noexcept
        : payload_(payload) {}
    std::string_view payload_;
};

class WebSocketCloseEvent final {
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
    friend class WebSocketEvent;
    constexpr WebSocketCloseEvent(std::uint16_t closeCode, std::string_view reason) noexcept
        : closeCode_(closeCode),
          reason_(reason) {}
    std::uint16_t closeCode_;
    std::string_view reason_;
};

class WebSocketProtocolErrorEvent final {
public:
    [[nodiscard]] constexpr std::uint16_t closeCode() const noexcept {
        return closeCode_;
    }

private:
    friend class WebSocketEvent;
    explicit constexpr WebSocketProtocolErrorEvent(std::uint16_t closeCode) noexcept
        : closeCode_(closeCode) {}
    std::uint16_t closeCode_;
};

class WebSocketTransportEndEvent final {
private:
    friend class WebSocketEvent;
    constexpr WebSocketTransportEndEvent() noexcept = default;
};

// One protocol value shared by both public drivers and the sans-I/O core.
// Payload/reason views borrow the driver's input or message assembly storage;
// the driver's next poll()/nextEvent() or feed() may invalidate those views.
class WebSocketEvent final {
public:
    [[nodiscard]] WebSocketEventKind kind() const noexcept {
        return static_cast<WebSocketEventKind>(value_.index());
    }
    [[nodiscard]] const WebSocketMessageEvent* message() const& noexcept {
        return std::get_if<WebSocketMessageEvent>(&value_);
    }
    const WebSocketMessageEvent* message() const&& = delete;
    [[nodiscard]] const WebSocketPingEvent* ping() const& noexcept {
        return std::get_if<WebSocketPingEvent>(&value_);
    }
    const WebSocketPingEvent* ping() const&& = delete;
    [[nodiscard]] const WebSocketPongEvent* pong() const& noexcept {
        return std::get_if<WebSocketPongEvent>(&value_);
    }
    const WebSocketPongEvent* pong() const&& = delete;
    [[nodiscard]] const WebSocketCloseEvent* close() const& noexcept {
        return std::get_if<WebSocketCloseEvent>(&value_);
    }
    const WebSocketCloseEvent* close() const&& = delete;
    [[nodiscard]] const WebSocketProtocolErrorEvent* protocolError() const& noexcept {
        return std::get_if<WebSocketProtocolErrorEvent>(&value_);
    }
    const WebSocketProtocolErrorEvent* protocolError() const&& = delete;
    [[nodiscard]] const WebSocketTransportEndEvent* transportEnd() const& noexcept {
        return std::get_if<WebSocketTransportEndEvent>(&value_);
    }
    const WebSocketTransportEndEvent* transportEnd() const&& = delete;

private:
    friend class detail::WsConnection;
    using Value = std::variant<WebSocketMessageEvent, WebSocketPingEvent, WebSocketPongEvent,
        WebSocketCloseEvent, WebSocketProtocolErrorEvent, WebSocketTransportEndEvent>;
    static_assert(std::to_underlying(WebSocketEventKind::kTransportEnd) + 1 == std::variant_size_v<Value>);
    template <typename Event>
    explicit WebSocketEvent(Event event) noexcept
        : value_(std::move(event)) {}
    [[nodiscard]] static WebSocketEvent message(WebSocketOpcode opcode, std::string_view payload) noexcept {
        return WebSocketEvent(WebSocketMessageEvent(opcode, payload));
    }
    [[nodiscard]] static WebSocketEvent ping(std::string_view payload) noexcept {
        return WebSocketEvent(WebSocketPingEvent(payload));
    }
    [[nodiscard]] static WebSocketEvent pong(std::string_view payload) noexcept {
        return WebSocketEvent(WebSocketPongEvent(payload));
    }
    [[nodiscard]] static WebSocketEvent close(std::uint16_t code, std::string_view reason) noexcept {
        return WebSocketEvent(WebSocketCloseEvent(code, reason));
    }
    [[nodiscard]] static WebSocketEvent protocolError(std::uint16_t code) noexcept {
        return WebSocketEvent(WebSocketProtocolErrorEvent(code));
    }
    [[nodiscard]] static WebSocketEvent makeTransportEnd() noexcept {
        return WebSocketEvent(WebSocketTransportEndEvent());
    }
    Value value_;
};

}  // namespace ruvia
