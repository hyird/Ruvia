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

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketInboundAssembler.h"
#include "ruvia/http/detail/websocket/WsEvent.h"

namespace ruvia::detail {

enum class WsTransportDisposition : std::uint8_t {
    kKeepOpen,
    kEndTransport,
};

// Runtime timeout policy needs only this protocol-owned classification, not the
// internal close-handshake state machine.
enum class WsLivenessMode : std::uint8_t {
    kOpen,
    kAwaitingPeerClose,
    kInactive,
};

enum class WsFrameSubmitStatus : std::uint8_t {
    kAccepted,
    kNotOpen,
    kInvalidOpcode,
    kMessageTooLarge,
    kInvalidTextPayload,
    kControlFrameTooLarge,
};

enum class WsCloseSubmitStatus : std::uint8_t {
    kAccepted,
    kAlreadyClosing,
    kClosed,
    kInvalidCode,
    kInvalidReason,
    kReasonTooLarge,
};

enum class WsAbortDisposition : std::uint8_t {
    kAbortTransport,
    kNoTransportAction,
};

enum class WsOutputConsumeStatus : std::uint8_t {
    kPending,
    kDrained,
    kOutOfRange,
};

class WsOutputPlan final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] constexpr WsTransportDisposition disposition() const noexcept {
        return disposition_;
    }

private:
    friend class WsConnection;

    constexpr WsOutputPlan(std::string_view bytes, WsTransportDisposition disposition) noexcept
        : bytes_(bytes),
          disposition_(disposition) {}

    std::string_view bytes_;
    WsTransportDisposition disposition_;
};

class WsConnection final {
public:
    explicit WsConnection(std::pmr::string& input, ProtocolByteLimit messageLimit = ProtocolByteLimit::unlimited(), WebSocketDeflateNegotiation deflate = WebSocketDeflateNegotiation::kDisabled);

    // Parse buffered transport bytes until one protocol event is available or
    // more input is required (nullopt). Every materialized event contains one
    // typed payload. Application data received after a local Close is validated
    // but not delivered while the peer Close is awaited.
    [[nodiscard]] std::optional<WsEvent> poll() &;
    [[nodiscard]] std::optional<WsEvent> poll() && = delete;

    [[nodiscard]] WsOutputPlan outputPlan() const& noexcept;
    [[nodiscard]] WsOutputPlan outputPlan() const&& = delete;
    [[nodiscard]] WsOutputConsumeStatus consumeOutput(std::size_t n) noexcept;
    void commitTransportEnd() noexcept;
    void notifyTransportEof() noexcept;
    [[nodiscard]] WsAbortDisposition abort() noexcept;
    [[nodiscard]] WsLivenessMode livenessMode() const noexcept;

    // Submit one complete logical message/control payload. Server masking,
    // outbound text UTF-8 validation, optional data-message compression and
    // wire header encoding stay inside the core.
    // Close has a separate typed entry because it owns code/reason validation
    // and close-handshake state rather than accepting a pre-encoded payload.
    [[nodiscard]] WsFrameSubmitStatus submitFrame(WebSocketOpcode opcode, std::string_view payload);
    [[nodiscard]] WsCloseSubmitStatus submitClose(std::uint16_t code, std::string_view reason);

private:
    enum class ClosePhase : std::uint8_t {
        kOpen,
        // A locally initiated Close is at the tail of pending output. Once
        // flushed, the connection keeps receiving until the peer Close.
        kLocalCloseQueued,
        kAwaitingPeerClose,
        // A peer Close was received (or the connection failed) and the final
        // local Close bytes still need to be flushed before transport end.
        kFinalCloseQueued,
        kTransportEndReady,
        kClosed,
    };

    void appendFrame(WebSocketOpcode opcode, std::string_view payload, bool rsv1 = false);
    void fail(std::uint16_t code, std::string_view reason = {});
    void receivePeerClose() noexcept;
    [[nodiscard]] std::optional<WsEvent> pollImpl() &;

    std::pmr::string* input_;
    ProtocolByteLimit messageLimit_;
    std::size_t inputOffset_{0};
    std::size_t pendingCompactUntil_{0};

    std::pmr::string outBuffer_;
    std::size_t outOffset_{0};

    WebSocketInboundAssembler assembler_;

    std::optional<WebSocketDeflate> deflate_;
    std::pmr::string inboundInflated_;
    std::pmr::string outboundDeflated_;

    ClosePhase closePhase_{ClosePhase::kOpen};
};

}  // namespace ruvia::detail
