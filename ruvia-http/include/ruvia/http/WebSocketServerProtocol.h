#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/WebSocketServerProtocolTypes.h"

namespace ruvia {

// Payload views remain valid until the next WebSocketServerProtocol::poll().
class WebSocketServerEvent final {
public:
    [[nodiscard]] WebSocketServerEventKind kind() const noexcept;
    [[nodiscard]] const WebSocketServerMessageEvent* message() const& noexcept;
    const WebSocketServerMessageEvent* message() const&& = delete;
    [[nodiscard]] const WebSocketServerPingEvent* ping() const& noexcept;
    const WebSocketServerPingEvent* ping() const&& = delete;
    [[nodiscard]] const WebSocketServerPongEvent* pong() const& noexcept;
    const WebSocketServerPongEvent* pong() const&& = delete;
    [[nodiscard]] const WebSocketServerCloseEvent* close() const& noexcept;
    const WebSocketServerCloseEvent* close() const&& = delete;
    [[nodiscard]] const WebSocketServerProtocolErrorEvent* protocolError() const& noexcept;
    const WebSocketServerProtocolErrorEvent* protocolError() const&& = delete;
    [[nodiscard]] const WebSocketServerTransportEndEvent* transportEnd() const& noexcept;
    const WebSocketServerTransportEndEvent* transportEnd() const&& = delete;

private:
    friend class WebSocketServerProtocol;
    using Value = std::variant<WebSocketServerMessageEvent, WebSocketServerPingEvent,
        WebSocketServerPongEvent, WebSocketServerCloseEvent,
        WebSocketServerProtocolErrorEvent, WebSocketServerTransportEndEvent>;
    static_assert(static_cast<std::size_t>(WebSocketServerEventKind::kTransportEnd) + 1 ==
                  std::variant_size_v<Value>);
    explicit WebSocketServerEvent(Value value) noexcept;
    Value value_;
};

// Sans-I/O WebSocket server protocol. The input buffer is borrowed and must
// outlive this object; its memory resource must also outlive this object.
class WebSocketServerProtocol final {
public:
    explicit WebSocketServerProtocol(std::pmr::string& input,
        ProtocolByteLimit messageLimit = ProtocolByteLimit::unlimited(),
        WebSocketCompression compression = WebSocketCompression::kDisabled);
    WebSocketServerProtocol(std::pmr::string& input, ProtocolByteLimit messageLimit,
        WebSocketServerProtocolOptions options);
    ~WebSocketServerProtocol();

    WebSocketServerProtocol(const WebSocketServerProtocol&) = delete;
    WebSocketServerProtocol& operator=(const WebSocketServerProtocol&) = delete;
    WebSocketServerProtocol(WebSocketServerProtocol&&) = delete;
    WebSocketServerProtocol& operator=(WebSocketServerProtocol&&) = delete;

    [[nodiscard]] std::optional<WebSocketServerEvent> poll() &;
    [[nodiscard]] std::optional<WebSocketServerEvent> poll() && = delete;
    [[nodiscard]] WebSocketServerOutputPlan outputPlan() const& noexcept;
    [[nodiscard]] WebSocketServerOutputPlan outputPlan() const&& = delete;
    [[nodiscard]] WebSocketServerOutputConsumeStatus consumeOutput(std::size_t n) noexcept;
    void commitTransportEnd() noexcept;
    void notifyTransportEof() noexcept;
    [[nodiscard]] WebSocketServerAbortDisposition abort() noexcept;
    [[nodiscard]] WebSocketLivenessMode livenessMode() const noexcept;
    [[nodiscard]] WebSocketServerFrameSubmitStatus submitFrame(
        WebSocketOpcode opcode, std::string_view payload, bool compress = true);
    [[nodiscard]] WebSocketServerCloseSubmitStatus submitClose(
        std::uint16_t code, std::string_view reason);

private:
    struct Impl;
    std::pmr::memory_resource* resource_;
    Impl* impl_;
};

}  // namespace ruvia
