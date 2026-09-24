#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>

// Stable HTTP protocol-layer entry point for runtimes driving an RFC 6455
// server connection. The runtime owns transport I/O; this object owns all
// WebSocket parsing, validation, compression and close-handshake decisions.
#include "ruvia/http/detail/websocket/WsConnection.h"

namespace ruvia {

// Public, server-role façade over the sans-I/O engine. Keeping this as a real
// owning type leaves the implementation core private to HTTP while preserving
// its borrowed poll-event and output-plan lifetimes.
using WebSocketServerEvent = detail::WsEvent;
using WebSocketServerTransportDisposition = detail::WsTransportDisposition;
using WebSocketServerFrameSubmitStatus = detail::WsFrameSubmitStatus;
using WebSocketServerCloseSubmitStatus = detail::WsCloseSubmitStatus;
using WebSocketServerAbortDisposition = detail::WsAbortDisposition;
using WebSocketServerOutputConsumeStatus = detail::WsOutputConsumeStatus;
using WebSocketServerOutputPlan = detail::WsOutputPlan;

class WebSocketServerProtocol final {
public:
    explicit WebSocketServerProtocol(std::pmr::string& input,
        ProtocolByteLimit messageLimit = ProtocolByteLimit::unlimited(),
        WebSocketCompression compression = WebSocketCompression::kDisabled)
        : core_(input, messageLimit, compression, detail::WsConnectionRole::kServer) {}

    WebSocketServerProtocol(const WebSocketServerProtocol&) = delete;
    WebSocketServerProtocol& operator=(const WebSocketServerProtocol&) = delete;
    WebSocketServerProtocol(WebSocketServerProtocol&&) = delete;
    WebSocketServerProtocol& operator=(WebSocketServerProtocol&&) = delete;

    [[nodiscard]] std::optional<WebSocketServerEvent> poll() & { return core_.poll(); }
    [[nodiscard]] std::optional<WebSocketServerEvent> poll() && = delete;
    [[nodiscard]] WebSocketServerOutputPlan outputPlan() const& noexcept {
        return core_.outputPlan();
    }
    [[nodiscard]] WebSocketServerOutputPlan outputPlan() const&& = delete;
    [[nodiscard]] WebSocketServerOutputConsumeStatus consumeOutput(std::size_t n) noexcept {
        return core_.consumeOutput(n);
    }
    void commitTransportEnd() noexcept { core_.commitTransportEnd(); }
    void notifyTransportEof() noexcept { core_.notifyTransportEof(); }
    [[nodiscard]] WebSocketServerAbortDisposition abort() noexcept { return core_.abort(); }
    [[nodiscard]] WebSocketLivenessMode livenessMode() const noexcept {
        return core_.livenessMode();
    }
    [[nodiscard]] WebSocketServerFrameSubmitStatus submitFrame(
        WebSocketOpcode opcode, std::string_view payload) {
        return core_.submitFrame(opcode, payload);
    }
    [[nodiscard]] WebSocketServerCloseSubmitStatus submitClose(
        std::uint16_t code, std::string_view reason) {
        return core_.submitClose(code, reason);
    }

private:
    detail::WsConnection core_;
};

}  // namespace ruvia
