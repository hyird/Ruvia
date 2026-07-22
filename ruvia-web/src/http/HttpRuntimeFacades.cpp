#include "ruvia/web/Context.h"

#include <stdexcept>

#include "ruvia/core/Task.h"
#include "ruvia/core/memory/PmrResource.h"

#include <memory_resource>
#include <utility>
#include <vector>

namespace {

class ResponseStreamOutputGuard final {
public:
    explicit ResponseStreamOutputGuard(bool& active) : active_(active) {
        if (active_) {
            throw std::logic_error(
                "response stream output operation is already in progress");
        }
        active_ = true;
    }

    ~ResponseStreamOutputGuard() { active_ = false; }

    ResponseStreamOutputGuard(const ResponseStreamOutputGuard&) = delete;
    ResponseStreamOutputGuard& operator=(
        const ResponseStreamOutputGuard&) = delete;

private:
    bool& active_;
};

ruvia::Task<void> writeOwned(
    void* target,
    ruvia::Task<void> (*write)(void*, std::string_view),
    std::pmr::string chunk,
    bool& outputActive) {
    ResponseStreamOutputGuard guard(outputActive);
    co_await write(target, chunk);
}

struct OwnedTrailers final {
    explicit OwnedTrailers(
        std::span<const ruvia::HttpHeaderView> source,
        std::pmr::memory_resource* resource)
        : strings(resource), views(resource) {
        strings.reserve(source.size() * 2);
        views.reserve(source.size());
        for (const auto& header : source) {
            strings.emplace_back(header.name());
            strings.emplace_back(header.value());
        }
        for (std::size_t index = 0; index < source.size(); ++index) {
            views.emplace_back(strings[index * 2], strings[index * 2 + 1]);
        }
    }

    std::pmr::vector<std::pmr::string> strings;
    std::pmr::vector<ruvia::HttpHeaderView> views;
};

ruvia::Task<void> endOwned(
    void* target,
    ruvia::Task<void> (*end)(void*, std::span<const ruvia::HttpHeaderView>),
    OwnedTrailers trailers,
    bool& outputActive) {
    ResponseStreamOutputGuard guard(outputActive);
    co_await end(target, trailers.views);
}

ruvia::Task<void> writeWebSocketOwned(
    void* target,
    ruvia::Task<void> (*write)(void*, ruvia::WebSocketOpcode, std::string_view),
    ruvia::WebSocketOpcode opcode,
    std::pmr::string payload) {
    co_await write(target, opcode, payload);
}

ruvia::Task<void> closeWebSocketOwned(
    void* target,
    ruvia::Task<void> (*close)(void*, std::uint16_t, std::string_view),
    std::uint16_t code,
    std::pmr::string reason) {
    co_await close(target, code, reason);
}

}  // namespace

#include "ruvia/web/detail/http/StreamingAccess.h"

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

namespace {

Task<std::optional<std::string_view>> readBody(
    detail::CallableRef<std::optional<std::string_view>> read,
    bool& readActive) {
    if (readActive) {
        throw std::logic_error("request body read is already in progress");
    }
    readActive = true;
    struct ReadGuard final {
        bool& active;
        ~ReadGuard() { active = false; }
    } guard{readActive};
    co_return co_await read();
}

}  // namespace

ScopedOperation<std::optional<std::string_view>> BodyReader::read() {
    return detail::makeScopedOperation(operationScope_, readBody(read_, readActive_));
}

ScopedOperation<void> ResponseStreamWriter::write(std::string_view chunk) {
    std::pmr::string owned(chunk, detail::processResource());
    return writeOwned(std::move(owned));
}

ScopedOperation<void> ResponseStreamWriter::writeOwned(std::pmr::string chunk) {
    return detail::makeScopedOperation(
        operationScope_,
        ::writeOwned(target_, write_, std::move(chunk), outputActive_));
}

ScopedOperation<void> ResponseStreamWriter::writeln(std::string_view chunk) {
    std::pmr::string owned(chunk, detail::processResource());
    owned.push_back('\n');
    return writeOwned(std::move(owned));
}

ScopedOperation<void> ResponseStreamWriter::sleep(std::chrono::milliseconds duration) {
    return detail::makeScopedOperation(operationScope_, sleep_(target_, duration));
}

ScopedOperation<void> ResponseStreamWriter::end(std::span<const HttpHeaderView> trailers) {
    return detail::makeScopedOperation(
        operationScope_,
        endOwned(
            target_,
            end_,
            OwnedTrailers(trailers, detail::processResource()),
            outputActive_));
}

ScopedOperation<void> SseWriter::sleep(std::chrono::milliseconds duration) {
    return writer_.sleep(duration);
}

ScopedOperation<void> SseWriter::end(std::span<const HttpHeaderView> trailers) {
    return writer_.end(trailers);
}

ScopedOperation<std::optional<WebSocketMessage>> WebSocket::read() {
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

ScopedOperation<void> WebSocket::close(std::uint16_t code, std::string_view reason) {
    std::pmr::string owned(reason, detail::processResource());
    return detail::makeScopedOperation(
        operationScope_,
        closeWebSocketOwned(target_, close_, code, std::move(owned)));
}

void WebSocket::abort() noexcept {
    abort_(target_);
}

ScopedOperation<void> WebSocket::write(WebSocketOpcode opcode, std::string_view payload) {
    std::pmr::string owned(payload, detail::processResource());
    return detail::makeScopedOperation(
        operationScope_,
        writeWebSocketOwned(target_, write_, opcode, std::move(owned)));
}

ScopedOperation<void> SseWriter::write(const SseMessage& message) {
    std::pmr::string frame(detail::processResource());
    detail::formatSseMessage(frame, message);
    return writer_.writeOwned(std::move(frame));
}

}  // namespace ruvia
