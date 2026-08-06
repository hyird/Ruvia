#pragma once

#include "test_harness.h"

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory_resource>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/detail/http/StreamingAccess.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamState.h"
#include "ruvia/web/detail/websocket/WebSocketAccess.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/Timer.h"
#include "ruvia/web/Streaming.h"

namespace streaming_test {

template <typename Text>
concept AcceptsSseData = requires(Text&& text) { ruvia::SseMessage{.data = std::forward<Text>(text)}; };

template <typename Text>
concept AcceptsSseEvent = requires(Text&& text) { ruvia::SseMessage{.event = std::forward<Text>(text)}; };

template <typename Text>
concept AcceptsSseId = requires(Text&& text) { ruvia::SseMessage{.id = std::forward<Text>(text)}; };

template <typename Text>
concept AcceptsAnySseTextAssignment = requires(ruvia::SseMessage& message, Text&& text) { message.data = std::forward<Text>(text); } || requires(ruvia::SseMessage& message, Text&& text) { message.event = std::forward<Text>(text); } || requires(ruvia::SseMessage& message, Text&& text) { message.id = std::forward<Text>(text); };

template <typename Text>
concept AcceptsAllSseTextAssignments = requires(ruvia::SseMessage& message, Text&& text) {
    message.data = std::forward<Text>(text);
    message.event = std::forward<Text>(text);
    message.id = std::forward<Text>(text);
};

static_assert(!AcceptsSseData<std::string>);
static_assert(!AcceptsSseData<const std::string>);
static_assert(!AcceptsSseData<std::pmr::string>);
static_assert(AcceptsSseData<std::string&>);
static_assert(AcceptsSseData<std::pmr::string&>);
static_assert(AcceptsSseData<std::string_view>);
static_assert(!AcceptsSseEvent<std::string>);
static_assert(!AcceptsSseEvent<const std::string>);
static_assert(!AcceptsSseEvent<std::pmr::string>);
static_assert(AcceptsSseEvent<std::string&>);
static_assert(AcceptsSseEvent<std::pmr::string&>);
static_assert(AcceptsSseEvent<std::string_view>);
static_assert(!AcceptsSseId<std::string>);
static_assert(!AcceptsSseId<const std::string>);
static_assert(!AcceptsSseId<std::pmr::string>);
static_assert(AcceptsSseId<std::string&>);
static_assert(AcceptsSseId<std::pmr::string&>);
static_assert(AcceptsSseId<std::string_view>);
static_assert(!AcceptsAnySseTextAssignment<std::string>);
static_assert(!AcceptsAnySseTextAssignment<const std::string>);
static_assert(!AcceptsAnySseTextAssignment<std::pmr::string>);
static_assert(AcceptsAllSseTextAssignments<std::string&>);
static_assert(AcceptsAllSseTextAssignments<std::pmr::string&>);
static_assert(AcceptsAllSseTextAssignments<std::string_view>);
static_assert(std::is_aggregate_v<ruvia::SseMessage>);
constexpr ruvia::SseMessage kLiteralSseMessage{.data = "data", .event = "event", .id = "id"};
static_assert(kLiteralSseMessage.data->view() == "data");
static_assert(kLiteralSseMessage.event == "event");
static_assert("event" == kLiteralSseMessage.event);
static_assert(kLiteralSseMessage.id->view() == "id");

class TestScopedCapability final : private ruvia::detail::ScopedCapabilityNode {
public:
    TestScopedCapability(ruvia::detail::ScopedOperationScope& scope, int& expiredCount) noexcept
        : ScopedCapabilityNode(scope, &TestScopedCapability::expire),
          expiredCount_(&expiredCount) {}

    TestScopedCapability(const TestScopedCapability& other) noexcept
        : ScopedCapabilityNode(other),
          expiredCount_(other.expiredCount_) {}

    TestScopedCapability(TestScopedCapability&& other) noexcept
        : ScopedCapabilityNode(std::move(other)),
          expiredCount_(std::exchange(other.expiredCount_, nullptr)) {}

    void use() const {
        requireActive();
    }

private:
    static void expire(ruvia::detail::ScopedCapabilityNode& node) noexcept {
        auto& capability = static_cast<TestScopedCapability&>(node);
        ++*capability.expiredCount_;
    }

    int* expiredCount_;
};

struct ColdFrameProbe final {
    bool* destroyed;
    bool armed{true};

    explicit ColdFrameProbe(bool& value) noexcept
        : destroyed(&value) {}
    ColdFrameProbe(ColdFrameProbe&& other) noexcept
        : destroyed(other.destroyed),
          armed(std::exchange(other.armed, false)) {}
    ~ColdFrameProbe() {
        if (armed) {
            *destroyed = true;
        }
    }
};

inline ruvia::Task<void> coldFrameTask(ColdFrameProbe) {
    co_return;
}

struct CaptureStreamSink final {
    std::vector<std::string> writes;
    std::vector<std::string> trailers;
};

inline ruvia::Task<void> writeChunk(void* target, std::string_view chunk) {
    static_cast<CaptureStreamSink*>(target)->writes.emplace_back(chunk);
    co_return;
}

inline ruvia::Task<void> endStream(void* target, std::span<const ruvia::HttpHeaderView> trailers) {
    auto& captured = static_cast<CaptureStreamSink*>(target)->trailers;
    for (const auto& trailer : trailers) {
        captured.emplace_back(std::string(trailer.name()) + "=" + std::string(trailer.value()));
    }
    co_return;
}

inline ruvia::Task<ruvia::TimerSleepResult> sleepStream(void*, std::chrono::milliseconds) {
    co_return ruvia::TimerSleepResult::kElapsed;
}

inline void bindContext(void*, ruvia::Context*, ruvia::HttpResponse (*)(ruvia::Context&)) noexcept {}
inline void releaseContext(void*) noexcept {}

inline bool committed(void*) noexcept {
    return false;
}

inline bool aborted(void*) noexcept {
    return false;
}

inline ruvia::HttpResponse unusedStreamingHead(ruvia::Context&) {
    return ruvia::HttpResponse(std::pmr::get_default_resource());
}

inline ruvia::ResponseStreamWriter makeWriter(CaptureStreamSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(&sink, &writeChunk, &endStream, &sleepStream, &bindContext, &releaseContext, &committed, &aborted);
}

inline ruvia::Task<void> writeLines(ruvia::ResponseStreamWriter& writer) {
    co_await writer.writeln("first");
    co_await writer.writeln("second");
}

inline ruvia::Task<void> writeStoredLines(ruvia::ResponseStreamWriter& writer) {
    auto first = writer.writeln(std::string("stored-first"));
    auto second = writer.writeln(std::string("stored-second"));
    co_await std::move(first);
    co_await std::move(second);
}

inline ruvia::ScopedOperation<void> makeExpiredWrite(CaptureStreamSink& sink) {
    auto writer = makeWriter(sink);
    return writer.write(std::string("must-not-run"));
}

inline ruvia::Task<void> awaitExpiredWrite(ruvia::ScopedOperation<void>& operation, bool& rejected) {
    try {
        co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

struct CaptureWebSocket final {
    std::vector<std::string> writes;
};

inline ruvia::Task<std::optional<ruvia::WebSocketMessage>> readSocket(void*) {
    co_return std::nullopt;
}

inline ruvia::Task<void> writeSocket(void* target, ruvia::WebSocketOpcode, std::string_view payload) {
    static_cast<CaptureWebSocket*>(target)->writes.emplace_back(payload);
    co_return;
}

inline ruvia::Task<void> closeSocket(void*, std::uint16_t, std::string_view) {
    co_return;
}

inline ruvia::ScopedOperation<void> makeExpiredWebSocketWrite(CaptureWebSocket& capture) {
    auto socket = ruvia::detail::WebSocketAccess::make(&capture, &readSocket, &writeSocket, &closeSocket);
    return socket.text(std::string("expired-payload"));
}

inline ruvia::Task<void> writeStoredTemporaryWebSocketPayload(ruvia::WebSocket& socket) {
    auto operation = socket.text(std::string("owned-payload"));
    co_await std::move(operation);
}

struct ImmediateBodySource final {
    ruvia::Task<std::optional<std::string_view>> read() {
        co_return std::string_view("must-not-read");
    }
};

inline ruvia::ScopedOperation<std::optional<std::string_view>> makeExpiredBodyRead() {
    ruvia::detail::BodyReaderBinding<ImmediateBodySource> binding;
    return binding.facade().read();
}

inline ruvia::Task<void> awaitExpiredBodyRead(ruvia::ScopedOperation<std::optional<std::string_view>>& operation, bool& rejected) {
    try {
        (void)co_await std::move(operation);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline ruvia::Task<void> endWithTrailers(ruvia::ResponseStreamWriter& writer) {
    const std::array<ruvia::HttpHeaderView, 2> trailers{ruvia::HttpHeaderView{"Digest", "sha-256=value"}, ruvia::HttpHeaderView{"Server-Timing", "db;dur=7"}};
    co_await writer.end(trailers);
}

inline ruvia::Task<void> endWithExpiredTrailerSources(ruvia::ResponseStreamWriter& writer) {
    auto operation = [&] {
        std::string name = "X-Owned-Trailer";
        std::string value = "temporary-value";
        const std::array<ruvia::HttpHeaderView, 1> trailers{ruvia::HttpHeaderView{name, value}};
        return writer.end(trailers);
    }();
    co_await std::move(operation);
}

struct SuspendedBodySource final {
    struct Awaiter final {
        SuspendedBodySource& source;

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> continuation) noexcept {
            source.continuation = continuation;
            source.readSuspended = true;
        }
        void await_resume() const noexcept {}
    };

    ruvia::Task<std::optional<std::string_view>> read() {
        co_await Awaiter{*this};
        co_return std::nullopt;
    }

    void resume() {
        const auto suspended = std::exchange(continuation, {});
        if (suspended) {
            suspended.resume();
        }
    }

    std::coroutine_handle<> continuation{};
    bool readSuspended{false};
};

struct SuspendedStreamSink final {
    struct Awaiter final {
        SuspendedStreamSink& sink;

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> continuation) noexcept {
            sink.continuation = continuation;
            sink.writeSuspended = true;
        }
        void await_resume() const noexcept {}
    };

    void resume() {
        const auto suspended = std::exchange(continuation, {});
        if (suspended) {
            suspended.resume();
        }
    }

    std::coroutine_handle<> continuation{};
    std::vector<std::string> writes;
    std::size_t ends{0};
    bool writeSuspended{false};
    bool suspendNextWrite{true};
    bool failNextWrite{false};
};

inline ruvia::Task<void> writeSuspendedStream(void* target, std::string_view chunk) {
    auto& sink = *static_cast<SuspendedStreamSink*>(target);
    sink.writes.emplace_back(chunk);
    if (std::exchange(sink.failNextWrite, false)) {
        throw std::runtime_error("stream write failed");
    }
    if (std::exchange(sink.suspendNextWrite, false)) {
        co_await SuspendedStreamSink::Awaiter{sink};
    }
}

inline ruvia::Task<void> endSuspendedStream(void* target, std::span<const ruvia::HttpHeaderView>) {
    ++static_cast<SuspendedStreamSink*>(target)->ends;
    co_return;
}

inline ruvia::ResponseStreamWriter makeSuspendedWriter(SuspendedStreamSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(&sink, &writeSuspendedStream, &endSuspendedStream, &sleepStream, &bindContext, &releaseContext, &committed, &aborted);
}

inline ruvia::Task<void> completeBodyRead(ruvia::BodyReader& reader, bool& completed) {
    (void)co_await reader.read();
    completed = true;
}

inline ruvia::Task<void> rejectConcurrentBodyRead(ruvia::BodyReader& reader, bool& rejected) {
    try {
        (void)co_await reader.read();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline ruvia::Task<void> completeStreamWrite(ruvia::ResponseStreamWriter& writer, std::string_view chunk, bool& completed) {
    co_await writer.write(chunk);
    completed = true;
}

inline ruvia::Task<void> rejectConcurrentStreamWrite(ruvia::ResponseStreamWriter& writer, bool& rejected) {
    try {
        co_await writer.write("overlap");
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline ruvia::Task<void> rejectConcurrentStreamEnd(ruvia::ResponseStreamWriter& writer, bool& rejected) {
    try {
        co_await writer.end();
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

inline ruvia::Task<void> observeStreamWriteFailure(ruvia::ResponseStreamWriter& writer, bool& failed) {
    try {
        co_await writer.write("failed");
    } catch (const std::runtime_error&) {
        failed = true;
    }
}

}  // namespace streaming_test

namespace streaming_test {

inline ruvia::Task<void> writeOneSse(ruvia::SseWriter& sse, ruvia::SseMessage message) {
    co_await sse.write(message);
}

}  // namespace streaming_test

using namespace streaming_test;  // NOLINT(google-build-using-namespace)
