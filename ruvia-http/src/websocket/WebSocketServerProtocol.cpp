#include "ruvia/http/WebSocketServerProtocol.h"

#include "ruvia/http/detail/util/HttpPmrObject.h"
#include "ruvia/http/detail/websocket/WsConnection.h"

namespace ruvia {

struct WebSocketServerProtocol::Impl final {
    explicit Impl(std::pmr::string& input, ProtocolByteLimit messageLimit,
        WebSocketServerProtocolOptions options)
        : resource(input.get_allocator().resource()),
          connection(input, messageLimit, options.compression,
              WebSocketConnectionRole::kServer, nullptr, nullptr, options.compressionLevel) {}

    std::pmr::memory_resource* resource;
    detail::WsConnection connection;
};

void WebSocketServerProtocol::ImplDeleter::operator()(Impl* value) const noexcept {
    if (value != nullptr) {
        auto* resource = value->resource;
        detail::destroyHttpPmrObject(value, resource);
    }
}

WebSocketServerProtocol::WebSocketServerProtocol(std::pmr::string& input,
    ProtocolByteLimit messageLimit, WebSocketCompression compression)
    : WebSocketServerProtocol(input, messageLimit, WebSocketServerProtocolOptions{compression, 6}) {}

WebSocketServerProtocol::WebSocketServerProtocol(std::pmr::string& input,
    ProtocolByteLimit messageLimit, WebSocketServerProtocolOptions options)
    : impl_(detail::constructHttpPmrObject<Impl>(input.get_allocator().resource(), input, messageLimit, options)) {}

WebSocketServerProtocol::~WebSocketServerProtocol() = default;

std::optional<WebSocketEvent> WebSocketServerProtocol::poll() & {
    return impl_->connection.poll();
}

WebSocketOutputPlan WebSocketServerProtocol::outputPlan() const& noexcept {
    return impl_->connection.outputPlan();
}
WebSocketOutputConsumeStatus WebSocketServerProtocol::consumeOutput(std::size_t n) noexcept {
    return impl_->connection.consumeOutput(n);
}
void WebSocketServerProtocol::commitTransportEnd() noexcept {
    impl_->connection.commitTransportEnd();
}
void WebSocketServerProtocol::notifyTransportEof() noexcept {
    impl_->connection.notifyTransportEof();
}
WebSocketAbortDisposition WebSocketServerProtocol::abort() noexcept {
    return impl_->connection.abort();
}
WebSocketLivenessMode WebSocketServerProtocol::livenessMode() const noexcept {
    return impl_->connection.livenessMode();
}
WebSocketFrameSubmitStatus WebSocketServerProtocol::submitFrame(WebSocketOpcode opcode, std::string_view payload, bool compress) {
    return impl_->connection.submitFrame(opcode, payload, compress);
}
WebSocketCloseSubmitStatus WebSocketServerProtocol::submitClose(std::uint16_t code, std::string_view reason) {
    return impl_->connection.submitClose(code, reason);
}

}  // namespace ruvia
