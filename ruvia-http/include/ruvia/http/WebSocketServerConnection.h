#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace ruvia {

enum class WebSocketCompression : std::uint8_t {
    kDisabled,
    kPermessageDeflate,
    kPermessageDeflateWithServerMaxWindowBits,
};

enum class WebSocketTransportDisposition : std::uint8_t { kKeepOpen, kEndTransport };
enum class WebSocketLivenessMode : std::uint8_t { kOpen, kAwaitingPeerClose, kInactive };
enum class WebSocketFrameSubmitStatus : std::uint8_t { kAccepted, kNotOpen, kInvalidOpcode, kMessageTooLarge, kInvalidTextPayload, kControlFrameTooLarge };
enum class WebSocketCloseSubmitStatus : std::uint8_t { kAccepted, kAlreadyClosing, kClosed, kInvalidCode, kInvalidReason, kReasonTooLarge };
enum class WebSocketAbortDisposition : std::uint8_t { kAbortTransport, kNoTransportAction };
enum class WebSocketOutputConsumeStatus : std::uint8_t { kPending, kDrained, kOutOfRange };

class WebSocketOutputPlan final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept { return bytes_; }
    [[nodiscard]] constexpr WebSocketTransportDisposition disposition() const noexcept { return disposition_; }

private:
    friend class WebSocketServerConnection;
    constexpr WebSocketOutputPlan(std::string_view bytes, WebSocketTransportDisposition disposition) noexcept
        : bytes_(bytes), disposition_(disposition) {}

    std::string_view bytes_;
    WebSocketTransportDisposition disposition_;
};

enum class WebSocketEventKind : std::uint8_t { kMessage, kPing, kPong, kClose, kProtocolError, kTransportEnd };

class WebSocketMessageEvent final {
public:
    [[nodiscard]] constexpr WebSocketOpcode opcode() const noexcept { return opcode_; }
    [[nodiscard]] constexpr std::string_view payload() const noexcept { return payload_; }
private:
    friend class WebSocketEvent;
    constexpr WebSocketMessageEvent(WebSocketOpcode opcode, std::string_view payload) noexcept : opcode_(opcode), payload_(payload) {}
    WebSocketOpcode opcode_;
    std::string_view payload_;
};

class WebSocketPingEvent final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept { return payload_; }
private:
    friend class WebSocketEvent;
    explicit constexpr WebSocketPingEvent(std::string_view payload) noexcept : payload_(payload) {}
    std::string_view payload_;
};

class WebSocketPongEvent final {
public:
    [[nodiscard]] constexpr std::string_view payload() const noexcept { return payload_; }
private:
    friend class WebSocketEvent;
    explicit constexpr WebSocketPongEvent(std::string_view payload) noexcept : payload_(payload) {}
    std::string_view payload_;
};

class WebSocketCloseEvent final {
public:
    [[nodiscard]] constexpr std::uint16_t closeCode() const noexcept { return closeCode_; }
    [[nodiscard]] constexpr std::string_view reason() const noexcept { return reason_; }
private:
    friend class WebSocketEvent;
    constexpr WebSocketCloseEvent(std::uint16_t closeCode, std::string_view reason) noexcept : closeCode_(closeCode), reason_(reason) {}
    std::uint16_t closeCode_;
    std::string_view reason_;
};

class WebSocketProtocolErrorEvent final {
public:
    [[nodiscard]] constexpr std::uint16_t closeCode() const noexcept { return closeCode_; }
private:
    friend class WebSocketEvent;
    explicit constexpr WebSocketProtocolErrorEvent(std::uint16_t closeCode) noexcept : closeCode_(closeCode) {}
    std::uint16_t closeCode_;
};

class WebSocketTransportEndEvent final {};

class WebSocketEvent final {
public:
    [[nodiscard]] WebSocketEventKind kind() const noexcept { return static_cast<WebSocketEventKind>(value_.index()); }
    [[nodiscard]] const WebSocketMessageEvent* message() const& noexcept { return std::get_if<WebSocketMessageEvent>(&value_); }
    const WebSocketMessageEvent* message() const&& = delete;
    [[nodiscard]] const WebSocketPingEvent* ping() const& noexcept { return std::get_if<WebSocketPingEvent>(&value_); }
    const WebSocketPingEvent* ping() const&& = delete;
    [[nodiscard]] const WebSocketPongEvent* pong() const& noexcept { return std::get_if<WebSocketPongEvent>(&value_); }
    const WebSocketPongEvent* pong() const&& = delete;
    [[nodiscard]] const WebSocketCloseEvent* close() const& noexcept { return std::get_if<WebSocketCloseEvent>(&value_); }
    const WebSocketCloseEvent* close() const&& = delete;
    [[nodiscard]] const WebSocketProtocolErrorEvent* protocolError() const& noexcept { return std::get_if<WebSocketProtocolErrorEvent>(&value_); }
    const WebSocketProtocolErrorEvent* protocolError() const&& = delete;
    [[nodiscard]] const WebSocketTransportEndEvent* transportEnd() const& noexcept { return std::get_if<WebSocketTransportEndEvent>(&value_); }
    const WebSocketTransportEndEvent* transportEnd() const&& = delete;

private:
    friend class WebSocketServerConnection;
    using Value = std::variant<WebSocketMessageEvent, WebSocketPingEvent, WebSocketPongEvent, WebSocketCloseEvent, WebSocketProtocolErrorEvent, WebSocketTransportEndEvent>;
    template <typename Event>
    explicit WebSocketEvent(Event event) noexcept : value_(std::move(event)) {}
    [[nodiscard]] static WebSocketEvent message(WebSocketOpcode opcode, std::string_view payload) noexcept { return WebSocketEvent(WebSocketMessageEvent(opcode, payload)); }
    [[nodiscard]] static WebSocketEvent ping(std::string_view payload) noexcept { return WebSocketEvent(WebSocketPingEvent(payload)); }
    [[nodiscard]] static WebSocketEvent pong(std::string_view payload) noexcept { return WebSocketEvent(WebSocketPongEvent(payload)); }
    [[nodiscard]] static WebSocketEvent close(std::uint16_t code, std::string_view reason) noexcept { return WebSocketEvent(WebSocketCloseEvent(code, reason)); }
    [[nodiscard]] static WebSocketEvent protocolError(std::uint16_t code) noexcept { return WebSocketEvent(WebSocketProtocolErrorEvent(code)); }
    [[nodiscard]] static WebSocketEvent transportEndEvent() noexcept { return WebSocketEvent(WebSocketTransportEndEvent()); }
    Value value_;
};

// Stable server-side sans-I/O WebSocket driver. It accepts RFC 6455 client
// frames (which must be masked) and emits server frames (which are never
// masked). `input` remains caller-owned and must outlive the connection; event
// views remain valid until the next poll().
class WebSocketServerConnection final {
public:
    explicit WebSocketServerConnection(std::pmr::string& input, ProtocolByteLimit messageLimit = ProtocolByteLimit::unlimited(), WebSocketCompression compression = WebSocketCompression::kDisabled);
    ~WebSocketServerConnection();
    WebSocketServerConnection(const WebSocketServerConnection&) = delete;
    WebSocketServerConnection& operator=(const WebSocketServerConnection&) = delete;
    WebSocketServerConnection(WebSocketServerConnection&&) noexcept;
    WebSocketServerConnection& operator=(WebSocketServerConnection&&) noexcept;

    [[nodiscard]] std::optional<WebSocketEvent> poll() &;
    std::optional<WebSocketEvent> poll() && = delete;
    [[nodiscard]] WebSocketOutputPlan outputPlan() const& noexcept;
    WebSocketOutputPlan outputPlan() const&& = delete;
    [[nodiscard]] WebSocketOutputConsumeStatus consumeOutput(std::size_t bytes) noexcept;
    void commitTransportEnd() noexcept;
    void notifyTransportEof() noexcept;
    [[nodiscard]] WebSocketAbortDisposition abort() noexcept;
    [[nodiscard]] WebSocketLivenessMode livenessMode() const noexcept;
    [[nodiscard]] WebSocketFrameSubmitStatus submitFrame(WebSocketOpcode opcode, std::string_view payload);
    [[nodiscard]] WebSocketCloseSubmitStatus submitClose(std::uint16_t code, std::string_view reason);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia
