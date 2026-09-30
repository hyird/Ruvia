#include "ruvia/http/WebSocketConnection.h"

#include "ruvia/http/detail/util/HttpPmrObject.h"
#include "ruvia/http/detail/websocket/WsConnection.h"

namespace ruvia {
class WebSocketConnection::Impl final {
public:
    Impl(std::pmr::memory_resource* memory, WebSocketConnectionOptions options)
        : resource(memory),
          input(memory),
          connection(input, options.messageLimit, options.compression,
              options.role, options.maskKeyGenerator, options.maskKeyContext, options.compressionLevel) {}
    std::pmr::memory_resource* resource;
    std::pmr::string input;
    detail::WsConnection connection;
};

void WebSocketConnection::ImplDeleter::operator()(Impl* value) const noexcept {
    if (value != nullptr) {
        auto* resource = value->resource;
        detail::destroyHttpPmrObject(value, resource);
    }
}

WebSocketConnection::WebSocketConnection(WebSocketConnectionOptions options) {
    auto* resource = detail::httpPmrResourceOrDefault(options.resource);
    impl_.reset(detail::constructHttpPmrObject<Impl>(resource, resource, options));
}

WebSocketConnection::~WebSocketConnection() = default;
WebSocketConnection::WebSocketConnection(WebSocketConnection&&) noexcept = default;
WebSocketConnection& WebSocketConnection::operator=(WebSocketConnection&&) noexcept = default;

WebSocketFeedStatus WebSocketConnection::feed(std::string_view input) {
    if (impl_->connection.livenessMode() == WebSocketLivenessMode::kInactive) {
        return WebSocketFeedStatus::kInactive;
    }
    impl_->input.append(input);
    return WebSocketFeedStatus::kAccepted;
}

std::optional<WebSocketEvent> WebSocketConnection::nextEvent() & {
    return impl_->connection.poll();
}

WebSocketOutputPlan WebSocketConnection::outputPlan() const& noexcept {
    return impl_->connection.outputPlan();
}

WebSocketOutputConsumeStatus WebSocketConnection::consumeOutput(std::size_t bytes) noexcept {
    return impl_->connection.consumeOutput(bytes);
}

void WebSocketConnection::commitTransportEnd() noexcept {
    impl_->connection.commitTransportEnd();
}
void WebSocketConnection::notifyTransportEof() noexcept {
    impl_->connection.notifyTransportEof();
}
WebSocketAbortDisposition WebSocketConnection::abort() noexcept {
    return impl_->connection.abort();
}
WebSocketLivenessMode WebSocketConnection::livenessMode() const noexcept {
    return impl_->connection.livenessMode();
}
WebSocketFrameSubmitStatus WebSocketConnection::submitFrame(WebSocketOpcode opcode, std::string_view payload, bool compress) {
    return impl_->connection.submitFrame(opcode, payload, compress);
}
WebSocketCloseSubmitStatus WebSocketConnection::submitClose(std::uint16_t code, std::string_view reason) {
    return impl_->connection.submitClose(code, reason);
}

}  // namespace ruvia
