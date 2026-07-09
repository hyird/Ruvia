#pragma once

// WebSocket sans-I/O connection core (server side).
//
// A pure protocol state machine mirroring the HTTP/2 Http2Connection design: you
// feed it inbound bytes and it advances the protocol and emits events (a complete
// message, a ping/pong, a close); you submit outbound messages/pongs/closes and it
// produces bytes for you to write. No socket, coroutine, timer or asio -- the I/O
//
// ROLE NOTE: this is the embeddable pure ws core for external runtimes and future
// edge/client drivers. Ruvia's own server path currently drives the transport-
// agnostic WebSocketConnection<Transport> template (ruvia-web) instead, whose
// heartbeat/write coordination is coroutine-shaped; both build on the same pure
// frame codec, assembler, validation and RFC 7692 deflate in this directory.
// loop and timeouts live entirely in the caller.
//
// Scope of this first slice: the unextended RFC 6455 protocol. permessage-deflate
// (RFC 7692) is enabled per connection via the constructor; when it is off, RSV1
// (compressed) frames are rejected exactly as required. Control-frame auto-responses
// (Pong on Ping, Close echo on peer Close) are produced into the outbound buffer,
// matching the coroutine connection's behaviour, and also surfaced as events.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "HttpWebSocketPermessageDeflate.h"
#include "HttpWebSocketUtils.h"

namespace ruvia::detail {

enum class WsFeedStatus : std::uint8_t {
    kOk,     // consumed some bytes, may have emitted events
    kClosed, // a Close was seen/sent; the connection is finished
};

struct WsFeedResult final {
    std::size_t consumed{0};
    WsFeedStatus status{WsFeedStatus::kOk};
};

// Events pulled by the core's owner after each feed.
struct WsEvent final {
    enum class Kind : std::uint8_t {
        kNone,
        kMessage,        // a complete data message (opcode = kText/kBinary, payload set)
        kPing,           // peer Ping (a Pong was already queued); payload = ping data
        kPong,           // peer Pong (clears an awaiting-pong state)
        kClose,          // peer Close (a Close echo was queued); closeCode/payload set
        kProtocolError,  // a protocol violation (a Close was queued); closeCode set
    };
    Kind kind{Kind::kNone};
    WebSocketOpcode opcode{WebSocketOpcode::kText};
    std::string_view payload{};
    std::uint16_t closeCode{0};
};

class WsConnection final {
public:
    explicit WsConnection(
        std::pmr::memory_resource* resource,
        std::size_t maxMessageBytes = 0,
        bool permessageDeflate = false);

    // --- inbound ---------------------------------------------------------------
    [[nodiscard]] WsFeedResult feed(std::string_view in);
    [[nodiscard]] WsEvent nextEvent();

    // --- outbound --------------------------------------------------------------
    [[nodiscard]] std::string_view pendingOutput() const noexcept;
    void consumeOutput(std::size_t n) noexcept;
    [[nodiscard]] bool wantsWrite() const noexcept { return outOffset_ < outBuffer_.size(); }

    void submitMessage(WebSocketOpcode opcode, std::string_view payload);
    void submitPing(std::string_view payload);
    void submitPong(std::string_view payload);
    void submitClose(std::uint16_t code, std::string_view reason);

    [[nodiscard]] bool closing() const noexcept { return closing_; }

private:
    enum class WsReadStatus : std::uint8_t { kFrame, kNeedMore };
    // Synchronous, sans-I/O frame reader matching webSocketTryReadFrame: parse one masked
    // frame from input_ at inputOffset_. Throws WebSocketProtocolError on a violation.
    [[nodiscard]] WsReadStatus readFrame(WebSocketFrameView& out);
    // Encode a frame header + payload into the outbound buffer (server frames are
    // unmasked, RFC 6455 §5.1).
    void appendFrame(WebSocketOpcode opcode, std::string_view payload, bool rsv1 = false);
    // Queue a Close frame (code + reason) and mark the connection closing (once).
    void appendClose(std::uint16_t code, std::string_view reason);

    std::pmr::memory_resource* resource_;
    std::size_t maxMessageBytes_;

    std::pmr::string input_;
    std::size_t inputOffset_{0};

    std::pmr::string outBuffer_;
    std::size_t outOffset_{0};

    WebSocketInboundAssembler assembler_;

    std::pmr::vector<WsEvent> events_;
    std::size_t eventOffset_{0};
    // Stable storage for delivered message payloads: the assembler reuses one buffer
    // across messages, so a message view handed out as an event is copied here (a
    // deque keeps element references stable as more messages land in the same feed).
    std::pmr::deque<std::pmr::string> messageStore_;

    // permessage-deflate (RFC 7692), present only when negotiated for this connection.
    std::optional<WebSocketDeflate> deflate_;
    std::pmr::string outboundDeflated_;

    bool closing_{false};
};

}  // namespace ruvia::detail
