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
        : active_(&active) {
        if (*active_) {
            active_ = nullptr;
            throw std::logic_error("response stream output operation is already in progress");
        }
        *active_ = true;
    }

    ResponseStreamOutputGuard(const ResponseStreamOutputGuard&) = delete;
    ResponseStreamOutputGuard& operator=(const ResponseStreamOutputGuard&) = delete;
    ResponseStreamOutputGuard(ResponseStreamOutputGuard&& other) noexcept
        : active_(std::exchange(other.active_, nullptr)) {}
    ResponseStreamOutputGuard& operator=(ResponseStreamOutputGuard&&) = delete;

    ~ResponseStreamOutputGuard() {
        if (active_ != nullptr) {
            *active_ = false;
        }
    }

private:
    bool* active_;
};

class WebSocketActivityLease final {
public:
    explicit WebSocketActivityLease(bool& active, const char* message)
        : active_(&active) {
        if (*active_) {
            active_ = nullptr;
            throw std::logic_error(message);
        }
        *active_ = true;
    }

    WebSocketActivityLease(const WebSocketActivityLease&) = delete;
    WebSocketActivityLease& operator=(const WebSocketActivityLease&) = delete;
    WebSocketActivityLease(WebSocketActivityLease&& other) noexcept
        : active_(std::exchange(other.active_, nullptr)) {}
    WebSocketActivityLease& operator=(WebSocketActivityLease&&) = delete;

    ~WebSocketActivityLease() {
        if (active_ != nullptr) {
            *active_ = false;
        }
    }

private:
    bool* active_;
};

ruvia::Task<void> writeTransferredChunk(void* target,
    ruvia::Task<void> (*write)(void*, std::string_view), std::pmr::string chunk,
    ResponseStreamOutputGuard guard) {
    static_cast<void>(guard);
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

    explicit OwnedTrailers(
        std::span<const ruvia::HttpHeaderView> source, std::pmr::memory_resource* resource)
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

ruvia::Task<void> endOwned(void* target,
    ruvia::Task<void> (*end)(void*, std::span<const ruvia::HttpHeaderView>), OwnedTrailers trailers,
    ResponseStreamOutputGuard guard) {
    static_cast<void>(guard);
    co_await end(target, trailers.views);
}

ruvia::Task<std::optional<ruvia::WebSocketMessage>> readWebSocket(void* target,
    ruvia::Task<std::optional<ruvia::WebSocketMessage>> (*read)(void*),
    WebSocketActivityLease activity) {
    static_cast<void>(activity);
    co_return co_await read(target);
}

ruvia::Task<void> writeWebSocketPayload(void* target,
    ruvia::Task<void> (*write)(void*, ruvia::WebSocketOpcode, std::string_view),
    ruvia::WebSocketOpcode opcode, std::pmr::string payload, WebSocketActivityLease activity) {
    static_cast<void>(activity);
    co_await write(target, opcode, payload);
}

ruvia::Task<void> closeWebSocketWithReason(void* target,
    ruvia::Task<void> (*close)(void*, ruvia::WebSocketCloseOptions),
    ruvia::WebSocketCloseOptions options, std::pmr::string reason,
    WebSocketActivityLease readActivity, WebSocketActivityLease writeActivity,
    WebSocketActivityLease closeActivity) {
    static_cast<void>(readActivity);
    static_cast<void>(writeActivity);
    static_cast<void>(closeActivity);
    options.reason = reason;
    co_await close(target, options);
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

Task<std::optional<std::string_view>> readBody(
    detail::CallableRef<std::optional<std::string_view>> read) {
    co_return co_await read();
}

}  // namespace

ScopedOperation<std::optional<std::string_view>> BodyReader::read() & {
    if (operationScope_.hasPendingOperations()) {
        throw std::logic_error("request body read is already in progress");
    }
    return detail::makeScopedOperation(operationScope_, readBody(read_));
}

ScopedOperation<void> ResponseStreamWriter::write(std::string_view chunk) & {
    requireActive();
    std::pmr::string owned(chunk, detail::processResource());
    return write(std::move(owned));
}

ScopedOperation<void> ResponseStreamWriter::write(std::pmr::string&& chunk) & {
    requireActive();
    ResponseStreamOutputGuard guard(outputActive_);
    return detail::makeScopedOperation(operationScope_,
        writeTransferredChunk(target_, write_, std::move(chunk), std::move(guard)));
}

ScopedOperation<void> ResponseStreamWriter::writeln(std::string_view chunk) & {
    requireActive();
    std::pmr::string owned(chunk, detail::processResource());
    owned.push_back('\n');
    return write(std::move(owned));
}

ScopedOperation<TimerSleepResult> ResponseStreamWriter::sleep(
    std::chrono::milliseconds duration) & {
    requireActive();
    return detail::makeScopedOperation(operationScope_, sleep_(target_, duration, stopToken_));
}

ScopedOperation<void> ResponseStreamWriter::end(std::span<const HttpHeaderView> trailers) & {
    requireActive();
    auto ownedTrailers = OwnedTrailers(trailers, detail::processResource());
    ResponseStreamOutputGuard guard(outputActive_);
    return detail::makeScopedOperation(
        operationScope_, endOwned(target_, end_, std::move(ownedTrailers), std::move(guard)));
}

ScopedOperation<TimerSleepResult> SseWriter::sleep(std::chrono::milliseconds duration) {
    return writer().sleep(duration);
}

ScopedOperation<void> SseWriter::end(std::span<const HttpHeaderView> trailers) {
    return writer().end(trailers);
}

ScopedOperation<std::optional<WebSocketMessage>> WebSocket::read() & {
    requireActive();
    WebSocketActivityLease activity(readActive_, "concurrent websocket reads are not supported");
    return detail::makeScopedOperation(
        operationScope_, readWebSocket(target_, read_, std::move(activity)));
}

ScopedOperation<void> WebSocket::text(std::string_view payload) & {
    return write(WebSocketOpcode::kText, payload);
}

ScopedOperation<void> WebSocket::binary(std::string_view payload) & {
    return write(WebSocketOpcode::kBinary, payload);
}

ScopedOperation<void> WebSocket::pong(std::string_view payload) & {
    return write(WebSocketOpcode::kPong, payload);
}

ScopedOperation<void> WebSocket::ping(std::string_view payload) & {
    return write(WebSocketOpcode::kPing, payload);
}

ScopedOperation<void> WebSocket::text(std::pmr::string&& payload) & {
    return write(WebSocketOpcode::kText, std::move(payload));
}

ScopedOperation<void> WebSocket::binary(std::pmr::string&& payload) & {
    return write(WebSocketOpcode::kBinary, std::move(payload));
}

ScopedOperation<void> WebSocket::pong(std::pmr::string&& payload) & {
    return write(WebSocketOpcode::kPong, std::move(payload));
}

ScopedOperation<void> WebSocket::ping(std::pmr::string&& payload) & {
    return write(WebSocketOpcode::kPing, std::move(payload));
}

ScopedOperation<void> WebSocket::close(WebSocketCloseOptions options) & {
    requireActive();
    std::pmr::string owned(options.reason.view(), detail::processResource());
    WebSocketActivityLease readActivity(readActive_, "websocket close cannot overlap a read");
    WebSocketActivityLease writeActivity(
        writeActive_, "websocket close cannot overlap an output operation");
    WebSocketActivityLease closeActivity(closeActive_, "websocket close is already in progress");
    return detail::makeScopedOperation(operationScope_,
        closeWebSocketWithReason(target_, close_, options, std::move(owned),
            std::move(readActivity), std::move(writeActivity), std::move(closeActivity)));
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
    return write(opcode, std::move(owned));
}

ScopedOperation<void> WebSocket::write(WebSocketOpcode opcode, std::pmr::string&& payload) {
    requireActive();
    WebSocketActivityLease activity(
        writeActive_, "concurrent websocket output operations are not supported");
    return detail::makeScopedOperation(operationScope_,
        writeWebSocketPayload(target_, write_, opcode, std::move(payload), std::move(activity)));
}

ScopedOperation<void> SseWriter::write(const SseMessage& message) {
    auto& streamWriter = writer();
    auto frame = formatSseMessage(message, {.resource = detail::processResource()});
    return streamWriter.write(std::move(frame));
}

}  // namespace ruvia
