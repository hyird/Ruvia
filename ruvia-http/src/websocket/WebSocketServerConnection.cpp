#include "ruvia/http/WebSocketServerConnection.h"

#include <exception>
#include <stdexcept>
#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/http/detail/websocket/WsConnection.h"

namespace ruvia {
namespace {

[[nodiscard]] WebSocketCompression validateCompression(WebSocketCompression compression) {
    switch (compression) {
        case WebSocketCompression::kDisabled:
        case WebSocketCompression::kPermessageDeflate:
        case WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits:
            return compression;
    }
    throw std::invalid_argument("invalid WebSocket compression mode");
}

[[nodiscard]] WebSocketTransportDisposition toPublic(detail::WsTransportDisposition disposition) noexcept {
    switch (disposition) {
        case detail::WsTransportDisposition::kKeepOpen: return WebSocketTransportDisposition::kKeepOpen;
        case detail::WsTransportDisposition::kEndTransport: return WebSocketTransportDisposition::kEndTransport;
    }
    std::terminate();
}

[[nodiscard]] WebSocketOutputConsumeStatus toPublic(detail::WsOutputConsumeStatus status) noexcept {
    switch (status) {
        case detail::WsOutputConsumeStatus::kPending: return WebSocketOutputConsumeStatus::kPending;
        case detail::WsOutputConsumeStatus::kDrained: return WebSocketOutputConsumeStatus::kDrained;
        case detail::WsOutputConsumeStatus::kOutOfRange: return WebSocketOutputConsumeStatus::kOutOfRange;
    }
    std::terminate();
}

[[nodiscard]] WebSocketAbortDisposition toPublic(detail::WsAbortDisposition disposition) noexcept {
    switch (disposition) {
        case detail::WsAbortDisposition::kAbortTransport: return WebSocketAbortDisposition::kAbortTransport;
        case detail::WsAbortDisposition::kNoTransportAction: return WebSocketAbortDisposition::kNoTransportAction;
    }
    std::terminate();
}

[[nodiscard]] WebSocketLivenessMode toPublic(detail::WsLivenessMode mode) noexcept {
    switch (mode) {
        case detail::WsLivenessMode::kOpen: return WebSocketLivenessMode::kOpen;
        case detail::WsLivenessMode::kAwaitingPeerClose: return WebSocketLivenessMode::kAwaitingPeerClose;
        case detail::WsLivenessMode::kInactive: return WebSocketLivenessMode::kInactive;
    }
    std::terminate();
}

[[nodiscard]] WebSocketFrameSubmitStatus toPublic(detail::WsFrameSubmitStatus status) noexcept {
    switch (status) {
        case detail::WsFrameSubmitStatus::kAccepted: return WebSocketFrameSubmitStatus::kAccepted;
        case detail::WsFrameSubmitStatus::kNotOpen: return WebSocketFrameSubmitStatus::kNotOpen;
        case detail::WsFrameSubmitStatus::kInvalidOpcode: return WebSocketFrameSubmitStatus::kInvalidOpcode;
        case detail::WsFrameSubmitStatus::kMessageTooLarge: return WebSocketFrameSubmitStatus::kMessageTooLarge;
        case detail::WsFrameSubmitStatus::kInvalidTextPayload: return WebSocketFrameSubmitStatus::kInvalidTextPayload;
        case detail::WsFrameSubmitStatus::kControlFrameTooLarge: return WebSocketFrameSubmitStatus::kControlFrameTooLarge;
    }
    std::terminate();
}

[[nodiscard]] WebSocketCloseSubmitStatus toPublic(detail::WsCloseSubmitStatus status) noexcept {
    switch (status) {
        case detail::WsCloseSubmitStatus::kAccepted: return WebSocketCloseSubmitStatus::kAccepted;
        case detail::WsCloseSubmitStatus::kAlreadyClosing: return WebSocketCloseSubmitStatus::kAlreadyClosing;
        case detail::WsCloseSubmitStatus::kClosed: return WebSocketCloseSubmitStatus::kClosed;
        case detail::WsCloseSubmitStatus::kInvalidCode: return WebSocketCloseSubmitStatus::kInvalidCode;
        case detail::WsCloseSubmitStatus::kInvalidReason: return WebSocketCloseSubmitStatus::kInvalidReason;
        case detail::WsCloseSubmitStatus::kReasonTooLarge: return WebSocketCloseSubmitStatus::kReasonTooLarge;
    }
    std::terminate();
}

}  // namespace

class WebSocketServerConnection::Impl final {
public:
    Impl(std::pmr::memory_resource* requested, WebSocketServerOptions options)
        : input(detail::httpPmrResourceOrDefault(requested)),
          connection(input, options.messageLimit, validateCompression(options.compression)) {}
    std::pmr::string input;
    detail::WsConnection connection;
};

WebSocketServerConnection::WebSocketServerConnection(std::pmr::memory_resource* resource, WebSocketServerOptions options)
    : impl_(std::make_unique<Impl>(resource, options)) {}

WebSocketServerConnection::~WebSocketServerConnection() = default;
WebSocketServerConnection::WebSocketServerConnection(WebSocketServerConnection&&) noexcept = default;
WebSocketServerConnection& WebSocketServerConnection::operator=(WebSocketServerConnection&&) noexcept = default;

WebSocketFeedStatus WebSocketServerConnection::feed(std::string_view input) {
    if (impl_->connection.livenessMode() == detail::WsLivenessMode::kInactive) {
        return WebSocketFeedStatus::kInactive;
    }
    impl_->input.append(input);
    return WebSocketFeedStatus::kAccepted;
}

std::optional<WebSocketEvent> WebSocketServerConnection::nextEvent() & {
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
    return WebSocketOutputPlan(plan.bytes(), toPublic(plan.disposition()));
}

WebSocketOutputConsumeStatus WebSocketServerConnection::consumeOutput(std::size_t bytes) noexcept {
    return toPublic(impl_->connection.consumeOutput(bytes));
}

void WebSocketServerConnection::commitTransportEnd() noexcept { impl_->connection.commitTransportEnd(); }
void WebSocketServerConnection::notifyTransportEof() noexcept { impl_->connection.notifyTransportEof(); }
WebSocketAbortDisposition WebSocketServerConnection::abort() noexcept { return toPublic(impl_->connection.abort()); }
WebSocketLivenessMode WebSocketServerConnection::livenessMode() const noexcept { return toPublic(impl_->connection.livenessMode()); }
WebSocketFrameSubmitStatus WebSocketServerConnection::submitFrame(WebSocketOpcode opcode, std::string_view payload) { return toPublic(impl_->connection.submitFrame(opcode, payload)); }
WebSocketCloseSubmitStatus WebSocketServerConnection::submitClose(std::uint16_t code, std::string_view reason) { return toPublic(impl_->connection.submitClose(code, reason)); }

}  // namespace ruvia
