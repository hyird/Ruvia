#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/WebSocketProtocol.h"

namespace ruvia::detail {
class WsConnection;
class WsEvent;
}  // namespace ruvia::detail

namespace ruvia {

enum class WebSocketServerTransportDisposition : std::uint8_t { kKeepOpen,
    kEndTransport };
enum class WebSocketServerFrameSubmitStatus : std::uint8_t {
    kAccepted,
    kNotOpen,
    kInvalidOpcode,
    kMessageTooLarge,
    kInvalidTextPayload,
    kControlFrameTooLarge
};
enum class WebSocketServerCloseSubmitStatus : std::uint8_t {
    kAccepted,
    kAlreadyClosing,
    kClosed,
    kInvalidCode,
    kInvalidReason,
    kReasonTooLarge
};
enum class WebSocketServerAbortDisposition : std::uint8_t { kAbortTransport,
    kNoTransportAction };
enum class WebSocketServerOutputConsumeStatus : std::uint8_t { kPending,
    kDrained,
    kOutOfRange };

enum class WebSocketServerEventKind : std::uint8_t {
    kMessage,
    kPing,
    kPong,
    kClose,
    kProtocolError,
    kTransportEnd,
};

class WebSocketServerMessageEvent final {
public:
    [[nodiscard]] constexpr WebSocketOpcode opcode() const noexcept {
        return opcode_;
    }
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class detail::WsEvent;
    constexpr WebSocketServerMessageEvent(WebSocketOpcode opcode, std::string_view payload) noexcept
        : opcode_(opcode),
          payload_(payload) {}
    WebSocketOpcode opcode_;
    std::string_view payload_;
};

class WebSocketServerPingEvent final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class detail::WsEvent;
    explicit constexpr WebSocketServerPingEvent(std::string_view payload) noexcept
        : payload_(payload) {}
    std::string_view payload_;
};

class WebSocketServerPongEvent final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class detail::WsEvent;
    explicit constexpr WebSocketServerPongEvent(std::string_view payload) noexcept
        : payload_(payload) {}
    std::string_view payload_;
};

class WebSocketServerCloseEvent final {
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
    friend class detail::WsEvent;
    constexpr WebSocketServerCloseEvent(std::uint16_t closeCode, std::string_view reason) noexcept
        : closeCode_(closeCode),
          reason_(reason) {}
    std::uint16_t closeCode_;
    std::string_view reason_;
};

class WebSocketServerProtocolErrorEvent final {
public:
    [[nodiscard]] constexpr std::uint16_t closeCode() const noexcept {
        return closeCode_;
    }

private:
    friend class detail::WsEvent;
    explicit constexpr WebSocketServerProtocolErrorEvent(std::uint16_t closeCode) noexcept
        : closeCode_(closeCode) {}
    std::uint16_t closeCode_;
};

class WebSocketServerTransportEndEvent final {
private:
    friend class detail::WsEvent;
    constexpr WebSocketServerTransportEndEvent() noexcept = default;
};

struct WebSocketServerProtocolOptions final {
    WebSocketCompression compression{WebSocketCompression::kDisabled};
    int compressionLevel{6};
};

class WebSocketServerOutputPlan final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] constexpr WebSocketServerTransportDisposition disposition() const noexcept {
        return disposition_;
    }

private:
    friend class detail::WsConnection;
    constexpr WebSocketServerOutputPlan(std::string_view bytes, WebSocketServerTransportDisposition disposition) noexcept
        : bytes_(bytes),
          disposition_(disposition) {}
    std::string_view bytes_;
    WebSocketServerTransportDisposition disposition_;
};

}  // namespace ruvia
