#pragma once

// WebSocket sans-I/O server connection core.
//
// The caller owns one persistent PMR input buffer and appends transport bytes to
// it. poll() parses and unmasks that buffer in place, emits at most one event,
// and leaves its payload view valid until the next poll() call. This keeps the
// runtime hot path zero-copy for complete unfragmented messages; fragmented and
// compressed messages use only their protocol-required assembly/decode storage.
//
// Outbound frames, including automatic Pong and Close replies, are serialized
// into the core's pending output. The output plan also owns the orderly transport
// end decision: RFC 6455 close-handshake state and RFC 8441 END_STREAM mapping must
// not be reconstructed by a runtime from a loose `close` boolean. The runtime only
// flushes the plan and owns coroutine I/O, timeout and write-exclusion policy.

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/detail/websocket/WsEvent.h"

namespace ruvia::detail {

enum class WsClosePhase : std::uint8_t {
    kOpen,
    // A locally initiated Close is at the tail of pending output. Once flushed,
    // the connection must keep receiving until the peer's Close arrives.
    kLocalCloseQueued,
    kAwaitingPeerClose,
    // A peer Close was received (or the connection failed) and the final local
    // Close bytes still need to be flushed before ending the transport.
    kFinalCloseQueued,
    // No WebSocket bytes remain; the transport must now be ended orderly.
    kTransportEndReady,
    kClosed,
};

enum class WsTransportDisposition : std::uint8_t {
    kKeepOpen,
    kEndTransport,
};

class WsOutputPlan final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] constexpr WsTransportDisposition disposition() const noexcept {
        return disposition_;
    }

    [[nodiscard]] constexpr bool endsTransport() const noexcept {
        return disposition_ == WsTransportDisposition::kEndTransport;
    }

private:
    friend class WsConnection;

    constexpr WsOutputPlan(
        std::string_view bytes,
        WsTransportDisposition disposition) noexcept
        : bytes_(bytes), disposition_(disposition) {}

    std::string_view bytes_;
    WsTransportDisposition disposition_;
};

class WsConnection final {
public:
    explicit WsConnection(
        std::pmr::string& input,
        std::size_t maxMessageBytes = 0,
        bool permessageDeflate = false);

    // Parse buffered transport bytes until one protocol event is available or
    // more input is required (nullopt). Every materialized event contains one
    // typed payload. Application data received after a local Close is validated
    // but not delivered while the peer Close is awaited.
    [[nodiscard]] std::optional<WsEvent> poll();

    [[nodiscard]] WsOutputPlan outputPlan() const noexcept;
    void consumeOutput(std::size_t n) noexcept;
    void commitTransportEnd() noexcept;
    void notifyTransportEof() noexcept;
    void abort() noexcept;

    [[nodiscard]] WsClosePhase closePhase() const noexcept {
        return closePhase_;
    }

    [[nodiscard]] bool acceptsApplicationFrames() const noexcept {
        return closePhase_ == WsClosePhase::kOpen;
    }

    [[nodiscard]] bool transportEndPending() const noexcept {
        return closePhase_ == WsClosePhase::kFinalCloseQueued ||
            closePhase_ == WsClosePhase::kTransportEndReady;
    }

    [[nodiscard]] bool closed() const noexcept {
        return closePhase_ == WsClosePhase::kClosed;
    }

    // Submit one already-formed logical frame payload. Server masking, optional
    // data-message compression and wire header encoding stay inside the core.
    void submitFrame(WebSocketOpcode opcode, std::string_view payload);
    void submitMessage(WebSocketOpcode opcode, std::string_view payload);
    void submitPing(std::string_view payload);
    void submitPong(std::string_view payload);
    void submitClose(std::uint16_t code, std::string_view reason);

private:
    void appendFrame(WebSocketOpcode opcode, std::string_view payload, bool rsv1 = false);
    void fail(std::uint16_t code, std::string_view reason = {});
    void receivePeerClose() noexcept;

    std::pmr::string* input_;
    std::size_t maxMessageBytes_;
    std::size_t inputOffset_{0};
    std::size_t pendingCompactUntil_{0};

    std::pmr::string outBuffer_;
    std::size_t outOffset_{0};

    WebSocketInboundAssembler assembler_;

    std::optional<WebSocketDeflate> deflate_;
    std::pmr::string inboundInflated_;
    std::pmr::string outboundDeflated_;

    WsClosePhase closePhase_{WsClosePhase::kOpen};
};

}  // namespace ruvia::detail
