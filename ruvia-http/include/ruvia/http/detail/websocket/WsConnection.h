#pragma once

// WebSocket sans-I/O connection core shared by server and client runtimes.
//
// The caller owns one persistent PMR input buffer and appends transport bytes to
// it. poll() parses that buffer (unmasking client-to-server frames in place),
// emits at most one event,
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
#include "ruvia/http/WebSocketProtocolTypes.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketInboundAssembler.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"

namespace ruvia::detail {

class WsConnection final {
public:
    explicit WsConnection(std::pmr::string& input,
        ProtocolByteLimit messageLimit = ProtocolByteLimit::unlimited(),
        WebSocketCompression compression = WebSocketCompression::kDisabled,
        WebSocketConnectionRole role = WebSocketConnectionRole::kServer,
        WebSocketMaskKeyGenerator maskKeyGenerator = nullptr, void* maskKeyContext = nullptr,
        int compressionLevel = 6);

    // Parse buffered transport bytes until one protocol event is available or
    // more input is required (nullopt). Every materialized event contains one
    // typed payload. Application data received after a local Close is validated
    // but not delivered while the peer Close is awaited.
    [[nodiscard]] std::optional<WebSocketEvent> poll() &;
    [[nodiscard]] std::optional<WebSocketEvent> poll() && = delete;

    [[nodiscard]] WebSocketOutputPlan outputPlan() const& noexcept;
    [[nodiscard]] WebSocketOutputPlan outputPlan() const&& = delete;
    [[nodiscard]] WebSocketOutputConsumeStatus consumeOutput(std::size_t n) noexcept;
    void commitTransportEnd() noexcept;
    void notifyTransportEof() noexcept;
    [[nodiscard]] WebSocketAbortDisposition abort() noexcept;
    [[nodiscard]] WebSocketLivenessMode livenessMode() const noexcept;

    // Submit one complete logical message/control payload. Role-correct masking,
    // outbound text UTF-8 validation, optional data-message compression and
    // wire header encoding stay inside the core.
    // Close has a separate typed entry because it owns code/reason validation
    // and close-handshake state rather than accepting a pre-encoded payload.
    [[nodiscard]] WebSocketFrameSubmitStatus submitFrame(WebSocketOpcode opcode, std::string_view payload, bool compress = true);
    [[nodiscard]] WebSocketCloseSubmitStatus submitClose(std::uint16_t code, std::string_view reason);

private:
    enum class ClosePhase : std::uint8_t {
        kOpen,
        // A locally initiated Close is at the tail of pending output. Once
        // flushed, the connection keeps receiving until the peer Close.
        kLocalCloseQueued,
        kAwaitingPeerClose,
        // A peer Close was received (or the connection failed) and the final
        // queued output still needs to be flushed before transport end. It may
        // be a Pong queued for an earlier Ping, not another Close.
        kFinalOutputQueued,
        kTransportEndReady,
        kClosed,
    };

    void appendFrame(WebSocketOpcode opcode, std::string_view payload, bool rsv1 = false);
    void fail(std::uint16_t code, std::string_view reason = {});
    void receivePeerClose() noexcept;
    [[nodiscard]] std::optional<WebSocketEvent> pollImpl() &;

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

    WebSocketConnectionRole role_{WebSocketConnectionRole::kServer};
    WebSocketMaskKeyGenerator maskKeyGenerator_{nullptr};
    void* maskKeyContext_{nullptr};

    ClosePhase closePhase_{ClosePhase::kOpen};
};

}  // namespace ruvia::detail
