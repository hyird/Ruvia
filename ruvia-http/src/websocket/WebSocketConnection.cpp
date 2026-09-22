#include "ruvia/http/WebSocketConnection.h"

#include <exception>
#include <stdexcept>

#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/http/detail/websocket/WsConnection.h"

namespace ruvia {
namespace {

[[nodiscard]] detail::WsConnectionRole toInternal(WebSocketConnectionRole role) {
    switch (role) {
        case WebSocketConnectionRole::kServer:
            return detail::WsConnectionRole::kServer;
        case WebSocketConnectionRole::kClient:
            return detail::WsConnectionRole::kClient;
    }
    throw std::invalid_argument("invalid WebSocket connection role");
}

[[nodiscard]] WebSocketCompression validateCompression(WebSocketCompression compression) {
    switch (compression) {
        case WebSocketCompression::kDisabled:
        case WebSocketCompression::kPermessageDeflate:
        case WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits:
        case WebSocketCompression::kPermessageDeflateContextTakeover:
        case WebSocketCompression::kPermessageDeflateContextTakeoverWithServerMaxWindowBits:
            return compression;
    }
    throw std::invalid_argument("invalid WebSocket compression mode");
}

[[nodiscard]] WebSocketTransportDisposition toPublic(
    detail::WsTransportDisposition disposition) noexcept {
    switch (disposition) {
        case detail::WsTransportDisposition::kKeepOpen:
            return WebSocketTransportDisposition::kKeepOpen;
        case detail::WsTransportDisposition::kEndTransport:
            return WebSocketTransportDisposition::kEndTransport;
    }
    std::terminate();
}

[[nodiscard]] WebSocketOutputConsumeStatus toPublic(detail::WsOutputConsumeStatus status) noexcept {
    switch (status) {
        case detail::WsOutputConsumeStatus::kPending:
            return WebSocketOutputConsumeStatus::kPending;
        case detail::WsOutputConsumeStatus::kDrained:
            return WebSocketOutputConsumeStatus::kDrained;
        case detail::WsOutputConsumeStatus::kOutOfRange:
            return WebSocketOutputConsumeStatus::kOutOfRange;
    }
    std::terminate();
}

[[nodiscard]] WebSocketAbortDisposition toPublic(detail::WsAbortDisposition disposition) noexcept {
    switch (disposition) {
        case detail::WsAbortDisposition::kAbortTransport:
            return WebSocketAbortDisposition::kAbortTransport;
        case detail::WsAbortDisposition::kNoTransportAction:
            return WebSocketAbortDisposition::kNoTransportAction;
    }
    std::terminate();
}

[[nodiscard]] WebSocketFrameSubmitStatus toPublic(detail::WsFrameSubmitStatus status) noexcept {
    switch (status) {
        case detail::WsFrameSubmitStatus::kAccepted:
            return WebSocketFrameSubmitStatus::kAccepted;
        case detail::WsFrameSubmitStatus::kNotOpen:
            return WebSocketFrameSubmitStatus::kNotOpen;
        case detail::WsFrameSubmitStatus::kInvalidOpcode:
            return WebSocketFrameSubmitStatus::kInvalidOpcode;
        case detail::WsFrameSubmitStatus::kMessageTooLarge:
            return WebSocketFrameSubmitStatus::kMessageTooLarge;
        case detail::WsFrameSubmitStatus::kInvalidTextPayload:
            return WebSocketFrameSubmitStatus::kInvalidTextPayload;
        case detail::WsFrameSubmitStatus::kControlFrameTooLarge:
            return WebSocketFrameSubmitStatus::kControlFrameTooLarge;
    }
    std::terminate();
}

[[nodiscard]] WebSocketCloseSubmitStatus toPublic(detail::WsCloseSubmitStatus status) noexcept {
    switch (status) {
        case detail::WsCloseSubmitStatus::kAccepted:
            return WebSocketCloseSubmitStatus::kAccepted;
        case detail::WsCloseSubmitStatus::kAlreadyClosing:
            return WebSocketCloseSubmitStatus::kAlreadyClosing;
        case detail::WsCloseSubmitStatus::kClosed:
            return WebSocketCloseSubmitStatus::kClosed;
        case detail::WsCloseSubmitStatus::kInvalidCode:
            return WebSocketCloseSubmitStatus::kInvalidCode;
        case detail::WsCloseSubmitStatus::kInvalidReason:
            return WebSocketCloseSubmitStatus::kInvalidReason;
        case detail::WsCloseSubmitStatus::kReasonTooLarge:
            return WebSocketCloseSubmitStatus::kReasonTooLarge;
    }
    std::terminate();
}

}  // namespace

class WebSocketConnection::Impl final {
public:
    explicit Impl(WebSocketConnectionOptions options)
        : input(detail::httpPmrResourceOrDefault(options.resource)),
          connection(input, options.messageLimit, validateCompression(options.compression),
              toInternal(options.role), options.maskKeyGenerator, options.maskKeyContext, options.compressionLevel) {}
    std::pmr::string input;
    detail::WsConnection connection;
};

WebSocketConnection::WebSocketConnection(WebSocketConnectionOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

WebSocketConnection::~WebSocketConnection() = default;
WebSocketConnection::WebSocketConnection(
    WebSocketConnection&&) noexcept = default;
WebSocketConnection& WebSocketConnection::operator=(
    WebSocketConnection&&) noexcept = default;

WebSocketFeedStatus WebSocketConnection::feed(std::string_view input) {
    if (impl_->connection.livenessMode() == WebSocketLivenessMode::kInactive) {
        return WebSocketFeedStatus::kInactive;
    }
    impl_->input.append(input);
    return WebSocketFeedStatus::kAccepted;
}

std::optional<WebSocketEvent> WebSocketConnection::nextEvent() & {
    auto event = impl_->connection.poll();
    if (!event) {
        return std::nullopt;
    }
    if (const auto* value = event->message()) {
        return WebSocketEvent::message(value->opcode(), value->payload());
    }
    if (const auto* value = event->ping()) {
        return WebSocketEvent::ping(value->payload());
    }
    if (const auto* value = event->pong()) {
        return WebSocketEvent::pong(value->payload());
    }
    if (const auto* value = event->close()) {
        return WebSocketEvent::close(value->closeCode(), value->reason());
    }
    if (const auto* value = event->protocolError()) {
        return WebSocketEvent::protocolError(value->closeCode());
    }
    return WebSocketEvent::transportEndEvent();
}

WebSocketOutputPlan WebSocketConnection::outputPlan() const& noexcept {
    const auto plan = impl_->connection.outputPlan();
    return WebSocketOutputPlan(plan.bytes(), toPublic(plan.disposition()));
}

WebSocketOutputConsumeStatus WebSocketConnection::consumeOutput(std::size_t bytes) noexcept {
    return toPublic(impl_->connection.consumeOutput(bytes));
}

void WebSocketConnection::commitTransportEnd() noexcept {
    impl_->connection.commitTransportEnd();
}
void WebSocketConnection::notifyTransportEof() noexcept {
    impl_->connection.notifyTransportEof();
}
WebSocketAbortDisposition WebSocketConnection::abort() noexcept {
    return toPublic(impl_->connection.abort());
}
WebSocketLivenessMode WebSocketConnection::livenessMode() const noexcept {
    return impl_->connection.livenessMode();
}
WebSocketFrameSubmitStatus WebSocketConnection::submitFrame(
    WebSocketOpcode opcode, std::string_view payload, bool compress) {
    return toPublic(impl_->connection.submitFrame(opcode, payload, compress));
}
WebSocketCloseSubmitStatus WebSocketConnection::submitClose(
    std::uint16_t code, std::string_view reason) {
    return toPublic(impl_->connection.submitClose(code, reason));
}

}  // namespace ruvia
