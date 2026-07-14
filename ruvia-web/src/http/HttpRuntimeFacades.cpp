#include "ruvia/web/Context.h"

#include <stdexcept>

#include "ruvia/core/Task.h"

namespace ruvia {

WebSocket& Context::webSocket() const {
    const auto* output = responseOutput_.webSocket();
    if (output == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return output->webSocket();
}

ResponseStreamWriter& Context::stream() const {
    const auto* output = responseOutput_.responseStream();
    if (output == nullptr) {
        throw std::logic_error("response body is not streamable");
    }
    return output->writer();
}

ResponseStreamWriter& Context::streamText() {
    setStableResponseHeader("Content-Type", "text/plain; charset=UTF-8");
    setStableResponseHeader("X-Content-Type-Options", "nosniff");
    return stream();
}

SseWriter Context::streamSse() {
    setStableResponseHeader("Content-Type", "text/event-stream");
    setStableResponseHeader("Cache-Control", "no-cache");
    return SseWriter(stream());
}

Task<std::optional<std::string_view>> BodyReader::read() {
    return read_();
}

Task<void> ResponseStreamWriter::write(std::string_view chunk) {
    return write_(target_, chunk);
}

Task<void> ResponseStreamWriter::writeln(std::string_view chunk) {
    auto& buffer = scratch_(target_);
    buffer.clear();
    buffer.reserve(chunk.size() + 1);
    if (!chunk.empty()) {
        buffer.append(chunk.data(), chunk.size());
    }
    buffer.push_back('\n');
    return write_(target_, std::string_view(buffer.data(), buffer.size()));
}

Task<void> ResponseStreamWriter::sleep(std::chrono::milliseconds duration) {
    return sleep_(target_, duration);
}

Task<void> ResponseStreamWriter::end(std::span<const HttpHeaderView> trailers) {
    return end_(target_, trailers);
}

Task<void> SseWriter::sleep(std::chrono::milliseconds duration) {
    return writer_.sleep(duration);
}

Task<void> SseWriter::end(std::span<const HttpHeaderView> trailers) {
    return writer_.end(trailers);
}

Task<std::optional<WebSocketMessage>> WebSocket::read() {
    return read_(target_);
}

Task<void> WebSocket::text(std::string_view payload) {
    return write(WebSocketOpcode::kText, payload);
}

Task<void> WebSocket::binary(std::string_view payload) {
    return write(WebSocketOpcode::kBinary, payload);
}

Task<void> WebSocket::pong(std::string_view payload) {
    return write(WebSocketOpcode::kPong, payload);
}

Task<void> WebSocket::ping(std::string_view payload) {
    return write(WebSocketOpcode::kPing, payload);
}

Task<void> WebSocket::close(std::uint16_t code, std::string_view reason) {
    return close_(target_, code, reason);
}

void WebSocket::abort() noexcept {
    abort_(target_);
}

Task<void> WebSocket::write(WebSocketOpcode opcode, std::string_view payload) {
    return write_(target_, opcode, payload);
}

}  // namespace ruvia
