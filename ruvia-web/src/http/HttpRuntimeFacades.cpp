#include "ruvia/http/HttpClientRuntime.h"
#include "ruvia/http/Streaming.h"
#include "ruvia/http/WebSocket.h"

#include "ruvia/app/Task.h"
#include "ruvia/web/detail/http/HttpBodyStreamAccess.h"

namespace ruvia {

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

Task<void> ResponseStreamWriter::end() {
    return end_(target_);
}

Task<void> SseWriter::sleep(std::chrono::milliseconds duration) {
    return writer_.sleep(duration);
}

Task<void> SseWriter::end() {
    return writer_.end();
}

Task<std::string_view> RequestBodyStream::nextChunk() const {
    if (nextChunk_ == nullptr) {
        co_return std::string_view{};
    }
    co_return co_await nextChunk_(target_);
}

Task<std::string_view> FetchResponseStream::readChunk() {
    return detail::HttpBodyStreamAccess::nextChunk(body_);
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

Task<void> WebSocket::write(WebSocketOpcode opcode, std::string_view payload) {
    return write_(target_, opcode, payload);
}

}  // namespace ruvia
