#include "ruvia/web/Context.h"

#include <stdexcept>

#include "ruvia/core/Task.h"
#include <memory_resource>
#include <utility>
#include <vector>

namespace {

class ResponseStreamOutputGuard final {
public:
    explicit ResponseStreamOutputGuard(bool& active)
        : active_(active) {
        if (active_) {
            throw std::logic_error("response stream output operation is already in progress");
        }
        active_ = true;
    }

    ~ResponseStreamOutputGuard() {
        active_ = false;
    }

    ResponseStreamOutputGuard(const ResponseStreamOutputGuard&) = delete;
    ResponseStreamOutputGuard& operator=(const ResponseStreamOutputGuard&) = delete;

private:
    bool& active_;
};

ruvia::Task<void> writeOwned(void* target, ruvia::Task<void> (*write)(void*, std::string_view), std::pmr::string chunk, bool& outputActive) {
    ResponseStreamOutputGuard guard(outputActive);
    co_await write(target, chunk);
}

struct OwnedTrailers final {
    struct OwnedTrailer final {
        OwnedTrailer(ruvia::HttpHeaderView header, std::pmr::memory_resource* resource)
            : name(header.name(), resource),
              value(header.value(), resource) {}

        std::pmr::string name;
        std::pmr::string value;
    };

    explicit OwnedTrailers(std::span<const ruvia::HttpHeaderView> source, std::pmr::memory_resource* resource)
        : fields(resource),
          views(resource) {
        fields.reserve(source.size());
        views.reserve(source.size());
        for (const auto& header : source) {
            fields.emplace_back(header, resource);
        }
        for (const auto& field : fields) {
            views.emplace_back(field.name, field.value);
        }
    }

    std::pmr::vector<OwnedTrailer> fields;
    std::pmr::vector<ruvia::HttpHeaderView> views;
};

ruvia::Task<void> endOwned(void* target, ruvia::Task<void> (*end)(void*, std::span<const ruvia::HttpHeaderView>), OwnedTrailers trailers, bool& outputActive) {
    ResponseStreamOutputGuard guard(outputActive);
    co_await end(target, trailers.views);
}

ruvia::Task<void> writeWebSocketOwned(void* target, ruvia::Task<void> (*write)(void*, ruvia::WebSocketOpcode, std::string_view), ruvia::WebSocketOpcode opcode, std::pmr::string payload) {
    co_await write(target, opcode, payload);
}

ruvia::Task<void> closeWebSocketOwned(void* target, ruvia::Task<void> (*close)(void*, std::uint16_t, std::string_view), std::uint16_t code, std::pmr::string reason) {
    co_await close(target, code, reason);
}

}  // namespace

#include "ruvia/web/detail/http/StreamingAccess.h"

namespace ruvia {

SseWriter::SseWriter(const SseWriter& other) noexcept
    : detail::ScopedCapabilityNode(other),
      writer_(other.writer_) {}

SseWriter::SseWriter(SseWriter&& other) noexcept
    : detail::ScopedCapabilityNode(std::move(other)),
      writer_(std::exchange(other.writer_, nullptr)) {}

SseWriter::SseWriter(ResponseStreamWriter& writer) noexcept
    : detail::ScopedCapabilityNode(writer.operationScope_, &SseWriter::expireCapability),
      writer_(writer.operationScope_.active() ? &writer : nullptr) {}

ResponseStreamWriter& SseWriter::writer() const {
    requireActive();
    return *writer_;
}

void SseWriter::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    static_cast<SseWriter&>(capability).writer_ = nullptr;
}

WebSocket& Context::webSocket() const {
    const auto* output = responseOutput_.webSocket();
    if (output == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return output->webSocket();
}

ResponseStreamWriter& Context::stream() {
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

namespace {

Task<std::optional<std::string_view>> readBody(detail::CallableRef<std::optional<std::string_view>> read, bool& readActive) {
    if (readActive) {
        throw std::logic_error("request body read is already in progress");
    }
    readActive = true;
    struct ReadGuard final {
        bool& active;
        ~ReadGuard() {
            active = false;
        }
    } guard{readActive};
    co_return co_await read();
}

}  // namespace

ScopedOperation<std::optional<std::string_view>> BodyReader::read() {
    return detail::makeScopedOperation(operationScope_, readBody(read_, readActive_));
}

ScopedOperation<void> ResponseStreamWriter::write(std::string_view chunk) {
    requireActive();
    std::pmr::string owned(chunk, detail::processResource());
    return writeOwned(std::move(owned));
}

ScopedOperation<void> ResponseStreamWriter::writeOwned(std::pmr::string chunk) {
    requireActive();
    return detail::makeScopedOperation(operationScope_, ::writeOwned(target_, write_, std::move(chunk), outputActive_));
}

ScopedOperation<void> ResponseStreamWriter::writeln(std::string_view chunk) {
    requireActive();
    std::pmr::string owned(chunk, detail::processResource());
    owned.push_back('\n');
    return writeOwned(std::move(owned));
}

ScopedOperation<TimerSleepResult> ResponseStreamWriter::sleep(std::chrono::milliseconds duration) {
    requireActive();
    return detail::makeScopedOperation(operationScope_, sleep_(target_, duration));
}

ScopedOperation<void> ResponseStreamWriter::end(std::span<const HttpHeaderView> trailers) {
    requireActive();
    return detail::makeScopedOperation(operationScope_, endOwned(target_, end_, OwnedTrailers(trailers, detail::processResource()), outputActive_));
}

ScopedOperation<TimerSleepResult> SseWriter::sleep(std::chrono::milliseconds duration) {
    return writer().sleep(duration);
}

ScopedOperation<void> SseWriter::end(std::span<const HttpHeaderView> trailers) {
    return writer().end(trailers);
}

ScopedOperation<std::optional<WebSocketMessage>> WebSocket::read() {
    requireActive();
    return detail::makeScopedOperation(operationScope_, read_(target_));
}

ScopedOperation<void> WebSocket::text(std::string_view payload) {
    return write(WebSocketOpcode::kText, payload);
}

ScopedOperation<void> WebSocket::binary(std::string_view payload) {
    return write(WebSocketOpcode::kBinary, payload);
}

ScopedOperation<void> WebSocket::pong(std::string_view payload) {
    return write(WebSocketOpcode::kPong, payload);
}

ScopedOperation<void> WebSocket::ping(std::string_view payload) {
    return write(WebSocketOpcode::kPing, payload);
}

ScopedOperation<void> WebSocket::textOwned(std::pmr::string payload) {
    return writeOwned(WebSocketOpcode::kText, std::move(payload));
}

ScopedOperation<void> WebSocket::binaryOwned(std::pmr::string payload) {
    return writeOwned(WebSocketOpcode::kBinary, std::move(payload));
}

ScopedOperation<void> WebSocket::pongOwned(std::pmr::string payload) {
    return writeOwned(WebSocketOpcode::kPong, std::move(payload));
}

ScopedOperation<void> WebSocket::pingOwned(std::pmr::string payload) {
    return writeOwned(WebSocketOpcode::kPing, std::move(payload));
}

ScopedOperation<void> WebSocket::close(std::uint16_t code, std::string_view reason) {
    requireActive();
    std::pmr::string owned(reason, detail::processResource());
    return detail::makeScopedOperation(operationScope_, closeWebSocketOwned(target_, close_, code, std::move(owned)));
}

void WebSocket::abort() noexcept {
    if (!operationScope_.active()) {
        return;
    }
    abort_(target_);
}

ScopedOperation<void> WebSocket::write(WebSocketOpcode opcode, std::string_view payload) {
    requireActive();
    std::pmr::string owned(payload, detail::processResource());
    return writeOwned(opcode, std::move(owned));
}

ScopedOperation<void> WebSocket::writeOwned(WebSocketOpcode opcode, std::pmr::string payload) {
    requireActive();
    return detail::makeScopedOperation(operationScope_, writeWebSocketOwned(target_, write_, opcode, std::move(payload)));
}

ScopedOperation<void> SseWriter::write(const SseMessage& message) {
    auto& streamWriter = writer();
    std::pmr::string frame(detail::processResource());
    detail::formatSseMessage(frame, message);
    return streamWriter.writeOwned(std::move(frame));
}

}  // namespace ruvia
