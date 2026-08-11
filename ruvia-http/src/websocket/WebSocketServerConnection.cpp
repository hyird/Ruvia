#include "ruvia/http/WebSocketServerConnection.h"

#include <stdexcept>

#include "ruvia/http/detail/websocket/WsConnection.h"

namespace ruvia {
namespace {

[[nodiscard]] detail::WebSocketDeflateNegotiation toInternal(WebSocketCompression compression) {
    switch (compression) {
        case WebSocketCompression::kDisabled: return detail::WebSocketDeflateNegotiation::kDisabled;
        case WebSocketCompression::kPermessageDeflate: return detail::WebSocketDeflateNegotiation::kAccepted;
        case WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits: return detail::WebSocketDeflateNegotiation::kAcceptedWithServerMaxWindowBits;
    }
    throw std::invalid_argument("invalid WebSocket compression mode");
}

}  // namespace

class WebSocketServerConnection::Impl final {
public:
    Impl(std::pmr::string& input, ProtocolByteLimit limit, WebSocketCompression compression)
        : connection(input, limit, toInternal(compression)) {}
    detail::WsConnection connection;
};

WebSocketServerConnection::WebSocketServerConnection(std::pmr::string& input, ProtocolByteLimit messageLimit, WebSocketCompression compression)
    : impl_(std::make_unique<Impl>(input, messageLimit, compression)) {}

WebSocketServerConnection::~WebSocketServerConnection() = default;
WebSocketServerConnection::WebSocketServerConnection(WebSocketServerConnection&&) noexcept = default;
WebSocketServerConnection& WebSocketServerConnection::operator=(WebSocketServerConnection&&) noexcept = default;

std::optional<WebSocketEvent> WebSocketServerConnection::poll() & {
    auto event = impl_->connection.poll();
    if (!event) return std::nullopt;
    if (const auto* value = event->message()) return WebSocketEvent::message(value->opcode(), value->payload());
    if (const auto* value = event->ping()) return WebSocketEvent::ping(value->payload());
    if (const auto* value = event->pong()) return WebSocketEvent::pong(value->payload());
    if (const auto* value = event->close()) return WebSocketEvent::close(value->closeCode(), value->reason());
    if (const auto* value = event->protocolError()) return WebSocketEvent::protocolError(value->closeCode());
    return WebSocketEvent::transportEndEvent();
}

WebSocketOutputPlan WebSocketServerConnection::outputPlan() const& noexcept {
    const auto plan = impl_->connection.outputPlan();
    return WebSocketOutputPlan(plan.bytes(), static_cast<WebSocketTransportDisposition>(plan.disposition()));
}

WebSocketOutputConsumeStatus WebSocketServerConnection::consumeOutput(std::size_t bytes) noexcept {
    return static_cast<WebSocketOutputConsumeStatus>(impl_->connection.consumeOutput(bytes));
}

void WebSocketServerConnection::commitTransportEnd() noexcept { impl_->connection.commitTransportEnd(); }
void WebSocketServerConnection::notifyTransportEof() noexcept { impl_->connection.notifyTransportEof(); }
WebSocketAbortDisposition WebSocketServerConnection::abort() noexcept { return static_cast<WebSocketAbortDisposition>(impl_->connection.abort()); }
WebSocketLivenessMode WebSocketServerConnection::livenessMode() const noexcept { return static_cast<WebSocketLivenessMode>(impl_->connection.livenessMode()); }
WebSocketFrameSubmitStatus WebSocketServerConnection::submitFrame(WebSocketOpcode opcode, std::string_view payload) { return static_cast<WebSocketFrameSubmitStatus>(impl_->connection.submitFrame(opcode, payload)); }
WebSocketCloseSubmitStatus WebSocketServerConnection::submitClose(std::uint16_t code, std::string_view reason) { return static_cast<WebSocketCloseSubmitStatus>(impl_->connection.submitClose(code, reason)); }

}  // namespace ruvia
