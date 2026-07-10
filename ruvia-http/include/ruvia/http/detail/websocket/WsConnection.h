#pragma once

// WebSocket sans-I/O server connection core.
//
// The caller owns one persistent PMR input buffer and appends transport bytes to
// it. feed() parses and unmasks that buffer in place, emits at most one event,
// and leaves its payload view valid until the next feed() call. This keeps the
// runtime hot path zero-copy for complete unfragmented messages; fragmented and
// compressed messages use only their protocol-required assembly/decode storage.
//
// Outbound frames, including automatic Pong and Close replies, are serialized
// into the core's pending output. The runtime only flushes those bytes and owns
// coroutine I/O, timeout and write-exclusion policy.

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"

namespace ruvia::detail {

enum class WsFeedStatus : std::uint8_t {
    kOk,
    kClosed,
};

struct WsEvent final {
    enum class Kind : std::uint8_t {
        kNone,
        kMessage,
        kPing,
        kPong,
        kClose,
        kProtocolError,
    };
    Kind kind{Kind::kNone};
    WebSocketOpcode opcode{WebSocketOpcode::kText};
    std::string_view payload{};
    std::uint16_t closeCode{0};
};

class WsConnection final {
public:
    explicit WsConnection(
        std::pmr::string& input,
        std::size_t maxMessageBytes = 0,
        bool permessageDeflate = false);

    [[nodiscard]] WsFeedStatus feed();
    [[nodiscard]] WsEvent nextEvent();

    [[nodiscard]] std::string_view pendingOutput() const noexcept;
    void consumeOutput(std::size_t n) noexcept;
    [[nodiscard]] bool wantsWrite() const noexcept { return outOffset_ < outBuffer_.size(); }

    // Submit one already-formed logical frame payload. Server masking, optional
    // data-message compression and wire header encoding stay inside the core.
    void submitFrame(WebSocketOpcode opcode, std::string_view payload);
    void submitMessage(WebSocketOpcode opcode, std::string_view payload);
    void submitPing(std::string_view payload);
    void submitPong(std::string_view payload);
    void submitClose(std::uint16_t code, std::string_view reason);

    [[nodiscard]] bool closing() const noexcept { return closing_; }

private:
    void appendFrame(WebSocketOpcode opcode, std::string_view payload, bool rsv1 = false);
    void appendClose(std::uint16_t code, std::string_view reason);
    void emit(WsEvent event) noexcept;

    std::pmr::string* input_;
    std::size_t maxMessageBytes_;
    std::size_t inputOffset_{0};
    std::size_t pendingCompactUntil_{0};

    std::pmr::string outBuffer_;
    std::size_t outOffset_{0};

    WebSocketInboundAssembler assembler_;
    WsEvent event_{};
    bool eventPending_{false};

    std::optional<WebSocketDeflate> deflate_;
    std::pmr::string inboundInflated_;
    std::pmr::string outboundDeflated_;

    bool closing_{false};
};

}  // namespace ruvia::detail
