#include "ruvia/http/WebSocketServerProtocol.h"

#include <exception>
#include <memory_resource>
#include <new>
#include <utility>
#include <variant>

#include "ruvia/http/detail/websocket/WsConnection.h"

namespace ruvia {

struct WebSocketServerProtocol::Impl final {
    explicit Impl(std::pmr::string& input, ProtocolByteLimit messageLimit,
        WebSocketServerProtocolOptions options)
        : connection(input, messageLimit, options.compression,
              detail::WsConnectionRole::kServer, nullptr, nullptr, options.compressionLevel) {}

    detail::WsConnection connection;
};

WebSocketServerEvent::WebSocketServerEvent(Value value) noexcept
    : value_(std::move(value)) {}

WebSocketServerEventKind WebSocketServerEvent::kind() const noexcept {
    return static_cast<WebSocketServerEventKind>(value_.index());
}

const WebSocketServerMessageEvent* WebSocketServerEvent::message() const& noexcept {
    return std::get_if<WebSocketServerMessageEvent>(&value_);
}
const WebSocketServerPingEvent* WebSocketServerEvent::ping() const& noexcept {
    return std::get_if<WebSocketServerPingEvent>(&value_);
}
const WebSocketServerPongEvent* WebSocketServerEvent::pong() const& noexcept {
    return std::get_if<WebSocketServerPongEvent>(&value_);
}
const WebSocketServerCloseEvent* WebSocketServerEvent::close() const& noexcept {
    return std::get_if<WebSocketServerCloseEvent>(&value_);
}
const WebSocketServerProtocolErrorEvent* WebSocketServerEvent::protocolError() const& noexcept {
    return std::get_if<WebSocketServerProtocolErrorEvent>(&value_);
}
const WebSocketServerTransportEndEvent* WebSocketServerEvent::transportEnd() const& noexcept {
    return std::get_if<WebSocketServerTransportEndEvent>(&value_);
}

WebSocketServerProtocol::WebSocketServerProtocol(std::pmr::string& input,
    ProtocolByteLimit messageLimit, WebSocketCompression compression)
    : WebSocketServerProtocol(input, messageLimit,
          WebSocketServerProtocolOptions{compression, 6}) {}

WebSocketServerProtocol::WebSocketServerProtocol(std::pmr::string& input,
    ProtocolByteLimit messageLimit, WebSocketServerProtocolOptions options)
    : resource_(input.get_allocator().resource()),
      impl_(nullptr) {
    std::pmr::polymorphic_allocator<Impl> allocator(resource_);
    impl_ = allocator.allocate(1);
    try {
        std::allocator_traits<decltype(allocator)>::construct(
            allocator, impl_, input, messageLimit, options);
    } catch (...) {
        allocator.deallocate(impl_, 1);
        throw;
    }
}

WebSocketServerProtocol::~WebSocketServerProtocol() {
    std::pmr::polymorphic_allocator<Impl> allocator(resource_);
    std::allocator_traits<decltype(allocator)>::destroy(allocator, impl_);
    allocator.deallocate(impl_, 1);
}

std::optional<WebSocketServerEvent> WebSocketServerProtocol::poll() & {
    auto event = impl_->connection.poll();
    if (!event) {
        return std::nullopt;
    }
    switch (event->kind()) {
        case WebSocketServerEventKind::kMessage: {
            const auto* item = event->message();
            return WebSocketServerEvent(WebSocketServerEvent::Value{
                WebSocketServerMessageEvent(item->opcode(), item->payload())});
        }
        case WebSocketServerEventKind::kPing:
            return WebSocketServerEvent(WebSocketServerEvent::Value{
                WebSocketServerPingEvent(event->ping()->payload())});
        case WebSocketServerEventKind::kPong:
            return WebSocketServerEvent(WebSocketServerEvent::Value{
                WebSocketServerPongEvent(event->pong()->payload())});
        case WebSocketServerEventKind::kClose: {
            const auto* item = event->close();
            return WebSocketServerEvent(WebSocketServerEvent::Value{
                WebSocketServerCloseEvent(item->closeCode(), item->reason())});
        }
        case WebSocketServerEventKind::kProtocolError:
            return WebSocketServerEvent(WebSocketServerEvent::Value{
                WebSocketServerProtocolErrorEvent(event->protocolError()->closeCode())});
        case WebSocketServerEventKind::kTransportEnd:
            return WebSocketServerEvent(WebSocketServerEvent::Value{
                WebSocketServerTransportEndEvent()});
    }
    std::terminate();
}

WebSocketServerOutputPlan WebSocketServerProtocol::outputPlan() const& noexcept {
    return impl_->connection.outputPlan();
}
WebSocketServerOutputConsumeStatus WebSocketServerProtocol::consumeOutput(std::size_t n) noexcept {
    return impl_->connection.consumeOutput(n);
}
void WebSocketServerProtocol::commitTransportEnd() noexcept {
    impl_->connection.commitTransportEnd();
}
void WebSocketServerProtocol::notifyTransportEof() noexcept {
    impl_->connection.notifyTransportEof();
}
WebSocketServerAbortDisposition WebSocketServerProtocol::abort() noexcept {
    return impl_->connection.abort();
}
WebSocketLivenessMode WebSocketServerProtocol::livenessMode() const noexcept {
    return impl_->connection.livenessMode();
}
WebSocketServerFrameSubmitStatus WebSocketServerProtocol::submitFrame(
    WebSocketOpcode opcode, std::string_view payload, bool compress) {
    return impl_->connection.submitFrame(opcode, payload, compress);
}
WebSocketServerCloseSubmitStatus WebSocketServerProtocol::submitClose(
    std::uint16_t code, std::string_view reason) {
    return impl_->connection.submitClose(code, reason);
}

}  // namespace ruvia
