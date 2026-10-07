#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/Bytes.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextCapabilities.h"
#include "ruvia/web/detail/util/operation_lane_lease.h"

namespace {

ruvia::detail::operation_lane_lease claim_output_lane(bool& active) {
    ruvia::detail::operation_lane_lease lease(active);
    if (!lease) {
        throw std::logic_error("response stream output operation is already in progress");
    }
    return lease;
}

ruvia::detail::operation_lane_lease claim_websocket_lane(bool& active, const char* message) {
    ruvia::detail::operation_lane_lease lease(active);
    if (!lease) {
        throw std::logic_error(message);
    }
    return lease;
}

ruvia::Task<void> writeTransferredChunk(void* target,
    ruvia::Task<void> (*write)(void*, std::string_view), std::pmr::string chunk,
    ruvia::detail::operation_lane_lease guard) {
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
    ruvia::detail::operation_lane_lease guard) {
    static_cast<void>(guard);
    co_await end(target, trailers.views);
}

void requireWebSocketWorker(void* target) noexcept {
    const auto* worker = static_cast<const ruvia::WorkerHandle*>(target);
    if (worker != nullptr && !worker->isCurrent()) {
        std::terminate();
    }
}

ruvia::Task<std::optional<ruvia::WebSocketMessage>> readWebSocket(void* target,
    ruvia::Task<std::optional<ruvia::WebSocketMessage>> (*read)(void*),
    ruvia::detail::operation_lane_lease activity) {
    static_cast<void>(activity);
    co_return co_await read(target);
}

ruvia::Task<void> writeWebSocketPayload(void* target,
    ruvia::Task<void> (*write)(void*, ruvia::WebSocketOpcode, std::string_view, bool),
    ruvia::WebSocketOpcode opcode, std::pmr::string payload, ruvia::detail::operation_lane_lease activity, bool compress) {
    static_cast<void>(activity);
    co_await write(target, opcode, payload, compress);
}

ruvia::Task<void> closeWebSocketWithReason(void* target,
    ruvia::Task<void> (*close)(void*, ruvia::WebSocketCloseOptions),
    ruvia::WebSocketCloseOptions options, std::pmr::string reason,
    ruvia::detail::operation_lane_lease readActivity, ruvia::detail::operation_lane_lease writeActivity,
    ruvia::detail::operation_lane_lease closeActivity) {
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
    : writer_(other.writer_),
      registration_(other.registration_, this) {}

SseWriter::SseWriter(SseWriter&& other) noexcept
    : writer_(std::exchange(other.writer_, nullptr)),
      registration_(std::move(other.registration_), this) {}

SseWriter::SseWriter(ResponseStreamWriter& writer) noexcept
    : writer_(writer.operationScope_.active() ? &writer : nullptr),
      registration_(writer.operationScope_, this, &SseWriter::expire_capability) {}

ResponseStreamWriter& SseWriter::writer() const {
    registration_.require_active();
    return *writer_;
}

void SseWriter::expire_capability(void* target) noexcept {
    static_cast<SseWriter*>(target)->writer_ = nullptr;
}

HttpTunnel& Context::tunnel() const {
    const auto* output = responseOutput().tunnel();
    if (output == nullptr) {
        throw std::logic_error("HTTP tunnel is available only in an established CONNECT route");
    }
    return output->tunnel();
}

WebSocket& Context::webSocket() const {
    const auto* output = responseOutput().webSocket();
    if (output == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return output->webSocket();
}

ResponseStreamWriter& Context::stream() {
    const auto* output = responseOutput().responseStream();
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

template <typename View>
Task<std::optional<View>> readBody(
    detail::CallableRef<std::optional<std::span<const std::byte>>> read) {
    const auto chunk = co_await read();
    if (!chunk) {
        co_return std::nullopt;
    }
    if constexpr (std::same_as<View, std::string_view>) {
        co_return asChars(*chunk);
    } else {
        co_return *chunk;
    }
}

}  // namespace

ScopedOperation<std::optional<std::span<const std::byte>>> BodyReader::read() & {
    if (operationScope_.has_pending_operations()) {
        throw std::logic_error("request body read is already in progress");
    }
    return ::ruvia::make_scoped_operation(operationScope_, readBody<std::span<const std::byte>>(read_));
}

ScopedOperation<std::optional<std::string_view>> BodyReader::text() & {
    if (operationScope_.has_pending_operations()) {
        throw std::logic_error("request body read is already in progress");
    }
    return ::ruvia::make_scoped_operation(operationScope_, readBody<std::string_view>(read_));
}

ScopedOperation<void> ResponseStreamWriter::write(std::span<const std::byte> chunk) & {
    return write(asChars(chunk));
}

ScopedOperation<void> ResponseStreamWriter::write(std::string_view chunk) & {
    requireActive();
    std::pmr::string owned(chunk, resource_);
    return write(std::move(owned));
}

ScopedOperation<void> ResponseStreamWriter::write(std::pmr::string&& chunk) & {
    requireActive();
    auto guard = claim_output_lane(outputActive_);
    std::pmr::string owned(std::move(chunk), resource_);
    return ::ruvia::make_scoped_operation(operationScope_,
        writeTransferredChunk(target_, write_, std::move(owned), std::move(guard)));
}

ScopedOperation<void> ResponseStreamWriter::writeln(std::string_view chunk) & {
    requireActive();
    std::pmr::string owned(chunk, resource_);
    owned.push_back('\n');
    return write(std::move(owned));
}

ScopedOperation<TimerSleepResult> ResponseStreamWriter::sleep(
    std::chrono::milliseconds duration) & {
    requireActive();
    return ::ruvia::make_scoped_operation(operationScope_, sleep_(target_, duration, stopToken_));
}

ScopedOperation<void> ResponseStreamWriter::end(std::span<const HttpHeaderView> trailers) & {
    requireActive();
    auto ownedTrailers = OwnedTrailers(trailers, resource_);
    auto guard = claim_output_lane(outputActive_);
    return ::ruvia::make_scoped_operation(
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
    auto activity = claim_websocket_lane(readActive_, "concurrent websocket reads are not supported");
    return ::ruvia::make_scoped_operation(operationScope_,
        readWebSocket(target_, read_, std::move(activity)), &requireWebSocketWorker,
        const_cast<WorkerHandle*>(worker_));
}

ScopedOperation<void> WebSocket::text(std::string_view payload, WebSocketSendOptions options) & {
    return write(WebSocketOpcode::kText, payload, options.compress);
}

ScopedOperation<void> WebSocket::binary(std::string_view payload, WebSocketSendOptions options) & {
    return write(WebSocketOpcode::kBinary, payload, options.compress);
}

ScopedOperation<void> WebSocket::pong(std::string_view payload) & {
    return write(WebSocketOpcode::kPong, payload);
}

ScopedOperation<void> WebSocket::ping(std::string_view payload) & {
    return write(WebSocketOpcode::kPing, payload);
}

ScopedOperation<void> WebSocket::text(std::pmr::string&& payload, WebSocketSendOptions options) & {
    return write(WebSocketOpcode::kText, std::move(payload), options.compress);
}

ScopedOperation<void> WebSocket::binary(std::pmr::string&& payload, WebSocketSendOptions options) & {
    return write(WebSocketOpcode::kBinary, std::move(payload), options.compress);
}

ScopedOperation<void> WebSocket::pong(std::pmr::string&& payload) & {
    return write(WebSocketOpcode::kPong, std::move(payload));
}

ScopedOperation<void> WebSocket::ping(std::pmr::string&& payload) & {
    return write(WebSocketOpcode::kPing, std::move(payload));
}

ScopedOperation<void> WebSocket::close(WebSocketCloseOptions options) & {
    requireActive();
    std::pmr::string owned(options.reason.view(), resource_);
    auto readActivity = claim_websocket_lane(readActive_, "websocket close cannot overlap a read");
    auto writeActivity = claim_websocket_lane(
        writeActive_, "websocket close cannot overlap an output operation");
    auto closeActivity = claim_websocket_lane(closeActive_, "websocket close is already in progress");
    return ::ruvia::make_scoped_operation(operationScope_,
        closeWebSocketWithReason(target_, close_, options, std::move(owned),
            std::move(readActivity), std::move(writeActivity), std::move(closeActivity)),
        &requireWebSocketWorker, const_cast<WorkerHandle*>(worker_));
}

void WebSocket::abort() noexcept {
    if (worker_ != nullptr && !worker_->isCurrent()) {
        std::terminate();
    }
    if (!operationScope_.active()) {
        return;
    }
    abort_(target_);
}

ScopedOperation<void> WebSocket::write(WebSocketOpcode opcode, std::string_view payload, bool compress) {
    requireActive();
    std::pmr::string owned(payload, resource_);
    return write(opcode, std::move(owned), compress);
}

ScopedOperation<void> WebSocket::write(WebSocketOpcode opcode, std::pmr::string&& payload, bool compress) {
    requireActive();
    auto activity = claim_websocket_lane(
        writeActive_, "concurrent websocket output operations are not supported");
    std::pmr::string owned(std::move(payload), resource_);
    return ::ruvia::make_scoped_operation(operationScope_,
        writeWebSocketPayload(target_, write_, opcode, std::move(owned), std::move(activity), compress),
        &requireWebSocketWorker, const_cast<WorkerHandle*>(worker_));
}

ScopedOperation<void> SseWriter::write(const SseMessage& message) {
    auto& streamWriter = writer();
    auto frame = formatSseMessage(message, {.resource = streamWriter.resource_});
    return streamWriter.write(std::move(frame));
}

}  // namespace ruvia
