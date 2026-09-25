#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/WebSocketServerProtocolTypes.h"

// Stable HTTP protocol-layer entry point for runtimes driving an RFC 6455
// server connection. The runtime owns transport I/O; this object owns all
// WebSocket parsing, validation, compression and close-handshake decisions.
#include "ruvia/http/detail/websocket/WsConnection.h"

namespace ruvia {

// Public protocol event with payload views valid until the next poll().
class WebSocketServerEvent final {
public:
    [[nodiscard]] WebSocketServerEventKind kind() const noexcept {
        return event_.kind();
    }
    [[nodiscard]] const WebSocketServerMessageEvent* message() const& noexcept {
        return event_.message();
    }
    const WebSocketServerMessageEvent* message() const&& = delete;
    [[nodiscard]] const WebSocketServerPingEvent* ping() const& noexcept {
        return event_.ping();
    }
    const WebSocketServerPingEvent* ping() const&& = delete;
    [[nodiscard]] const WebSocketServerPongEvent* pong() const& noexcept {
        return event_.pong();
    }
    const WebSocketServerPongEvent* pong() const&& = delete;
    [[nodiscard]] const WebSocketServerCloseEvent* close() const& noexcept {
        return event_.close();
    }
    const WebSocketServerCloseEvent* close() const&& = delete;
    [[nodiscard]] const WebSocketServerProtocolErrorEvent* protocolError() const& noexcept {
        return event_.protocolError();
    }
    const WebSocketServerProtocolErrorEvent* protocolError() const&& = delete;
    [[nodiscard]] const WebSocketServerTransportEndEvent* transportEnd() const& noexcept {
        return event_.transportEnd();
    }
    const WebSocketServerTransportEndEvent* transportEnd() const&& = delete;

private:
    friend class WebSocketServerProtocol;
    explicit WebSocketServerEvent(detail::WsEvent event) noexcept
        : event_(std::move(event)) {}
    detail::WsEvent event_;
};

class WebSocketServerProtocol final {
public:
    explicit WebSocketServerProtocol(std::pmr::string& input,
        ProtocolByteLimit messageLimit = ProtocolByteLimit::unlimited(),
        WebSocketCompression compression = WebSocketCompression::kDisabled)
        : WebSocketServerProtocol(input, messageLimit, WebSocketServerProtocolOptions{compression, 6}) {}

    WebSocketServerProtocol(std::pmr::string& input, ProtocolByteLimit messageLimit,
        WebSocketServerProtocolOptions options)
        : core_(input, messageLimit, options.compression, detail::WsConnectionRole::kServer,
              nullptr, nullptr, options.compressionLevel) {}

    WebSocketServerProtocol(const WebSocketServerProtocol&) = delete;
    WebSocketServerProtocol& operator=(const WebSocketServerProtocol&) = delete;
    WebSocketServerProtocol(WebSocketServerProtocol&&) = delete;
    WebSocketServerProtocol& operator=(WebSocketServerProtocol&&) = delete;

    [[nodiscard]] std::optional<WebSocketServerEvent> poll() & {
        auto event = core_.poll();
        if (!event) {
            return std::nullopt;
        }
        return WebSocketServerEvent(std::move(*event));
    }
    [[nodiscard]] std::optional<WebSocketServerEvent> poll() && = delete;
    [[nodiscard]] WebSocketServerOutputPlan outputPlan() const& noexcept {
        return core_.outputPlan();
    }
    [[nodiscard]] WebSocketServerOutputPlan outputPlan() const&& = delete;
    [[nodiscard]] WebSocketServerOutputConsumeStatus consumeOutput(std::size_t n) noexcept {
        return core_.consumeOutput(n);
    }
    void commitTransportEnd() noexcept {
        core_.commitTransportEnd();
    }
    void notifyTransportEof() noexcept {
        core_.notifyTransportEof();
    }
    [[nodiscard]] WebSocketServerAbortDisposition abort() noexcept {
        return core_.abort();
    }
    [[nodiscard]] WebSocketLivenessMode livenessMode() const noexcept {
        return core_.livenessMode();
    }
    [[nodiscard]] WebSocketServerFrameSubmitStatus submitFrame(
        WebSocketOpcode opcode, std::string_view payload, bool compress = true) {
        return core_.submitFrame(opcode, payload, compress);
    }
    [[nodiscard]] WebSocketServerCloseSubmitStatus submitClose(
        std::uint16_t code, std::string_view reason) {
        return core_.submitClose(code, reason);
    }

private:
    detail::WsConnection core_;
};

}  // namespace ruvia
