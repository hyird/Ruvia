#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/Timer.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/Sse.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/web/detail/util/CallableRef.h"

#include <chrono>
#include <concepts>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ruvia {

class Context;
class HttpResponse;

namespace detail {
struct StreamingAccess;
}

class BodyReader final {
private:
    friend struct detail::StreamingAccess;
    friend class MultipartReader;

    struct Token final {};

public:
    BodyReader(Token, void* target, detail::CallableRef<std::optional<std::string_view>>::Invoke read) noexcept
        : read_(target, read) {}

    BodyReader(const BodyReader&) = delete;
    BodyReader& operator=(const BodyReader&) = delete;

    /// Reads the next chunk of the streamed request body.
    /// @warning The returned view borrows the connection read buffer and is only valid
    /// until the NEXT read() call; copy it out if you need to retain it past then. An
    /// empty optional signals end-of-body. Only one read may be in flight; concurrent
    /// consumers are rejected because they cannot safely share the borrowed buffer.
    [[nodiscard]] ScopedOperation<std::optional<std::string_view>> read();

private:
    detail::CallableRef<std::optional<std::string_view>> read_;
    bool readActive_{false};
    detail::ScopedOperationScope operationScope_;
};

class ResponseStreamWriter final {
public:
    ResponseStreamWriter(const ResponseStreamWriter&) = delete;
    ResponseStreamWriter& operator=(const ResponseStreamWriter&) = delete;

    /// Writes one body chunk. write(), writeln(), and end() share one linear
    /// output lane; starting another output operation before the current one
    /// completes throws std::logic_error.
    ///
    /// The string_view overload copies the chunk into process-owned PMR storage
    /// before returning. Hot-path producers that already hold a buffer in
    /// request-owned storage can move it into the PMR-string overload to skip
    /// that copy.
    ScopedOperation<void> write(std::string_view chunk);

    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                 std::constructible_from<std::string_view, Text&&>)
    ScopedOperation<void> write(Text&& chunk) {
        return write(std::string_view(std::forward<Text>(chunk)));
    }

    /// Zero-copy write: takes ownership of an already-allocated chunk and
    /// transfers it into the output lane without copying. Build the chunk with
    /// a request-owned arena (Context::resource()) for hot-path streaming.
    ScopedOperation<void> write(std::pmr::string&& chunk);

    ScopedOperation<void> writeln(std::string_view chunk);

    /// Suspends the stream producer. The result is kElapsed for a normal
    /// delay, or kStopRequested when the owning worker is shutting down or the
    /// request's stop token trips. HTTP/2 peer termination remains reported as
    /// its transport error.
    ScopedOperation<TimerSleepResult> sleep(std::chrono::milliseconds duration);

    /// Whether the response stream can no longer be delivered. The signal is
    /// transport-dependent: on HTTP/2 it also turns true when the peer resets or
    /// terminates the stream, so a long-lived producer can observe a passive
    /// client disconnect; on HTTP/1 it reflects only a prior failed write, so a
    /// passive disconnect is not seen until the next write() fails. Treat a false
    /// result as "no failure observed yet", not a guarantee the client is still
    /// connected.
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_(target_);
    }

    /// Atomically terminates the stream with an optional trailer section.
    /// The header views only need to remain valid until the returned task completes.
    /// A non-empty section is never silently dropped when the selected HTTP
    /// version/method/status cannot represent trailers. If the stream is still
    /// uncommitted, that rejection occurs before the response head is emitted.
    ScopedOperation<void> end(std::span<const HttpHeaderView> trailers = {});

private:
    friend struct detail::StreamingAccess;

    using Write = Task<void> (*)(void*, std::string_view);
    using End = Task<void> (*)(void*, std::span<const HttpHeaderView>);
    using Sleep = Task<TimerSleepResult> (*)(void*, std::chrono::milliseconds, const StopToken&);
    using StreamingHeadThunk = HttpResponse (*)(Context&);
    using BindContext = void (*)(void*, Context*, StreamingHeadThunk);
    using ReleaseContext = void (*)(void*) noexcept;
    using Committed = bool (*)(void*) noexcept;
    using Aborted = bool (*)(void*) noexcept;

    ResponseStreamWriter(void* target, Write write, End end, Sleep sleep, BindContext bindContext, ReleaseContext releaseContext, Committed committed, Aborted aborted) noexcept
        : target_(target),
          write_(write),
          end_(end),
          sleep_(sleep),
          bindContext_(bindContext),
          releaseContext_(releaseContext),
          committed_(committed),
          aborted_(aborted) {}

    // The request's stop token travels with the binding so sleep() can observe
    // it. A framework-provided wait that ignores it would be a hole in every
    // deadline built on that token -- which is exactly what this used to be.
    void bindContext(Context& context, StopToken stopToken, StreamingHeadThunk streamingHead) {
        stopToken_ = stopToken;
        bindContext_(target_, &context, streamingHead);
    }

    void releaseContext() noexcept {
        operationScope_.close();
        releaseContext_(target_);
    }

    void requireActive() const {
        if (!operationScope_.active()) {
            throw std::logic_error("response stream lifetime has expired");
        }
    }

    [[nodiscard]] bool committed() const noexcept {
        return committed_(target_);
    }

    void* target_;
    Write write_;
    End end_;
    Sleep sleep_;
    BindContext bindContext_;
    ReleaseContext releaseContext_;
    Committed committed_;
    Aborted aborted_;
    StopToken stopToken_;
    bool outputActive_{false};
    detail::ScopedOperationScope operationScope_;

    friend class SseWriter;
};

class SseWriter final : private detail::ScopedCapabilityNode {
public:
    SseWriter(const SseWriter& other) noexcept;
    SseWriter& operator=(const SseWriter&) = delete;
    SseWriter(SseWriter&& other) noexcept;
    SseWriter& operator=(SseWriter&&) = delete;

    ScopedOperation<void> write(const SseMessage& message);

    ScopedOperation<TimerSleepResult> sleep(std::chrono::milliseconds duration);

    [[nodiscard]] bool aborted() const noexcept {
        return writer_ == nullptr || writer_->aborted();
    }

    ScopedOperation<void> end(std::span<const HttpHeaderView> trailers = {});

private:
    friend class Context;
    friend struct detail::StreamingAccess;

    explicit SseWriter(ResponseStreamWriter& writer) noexcept;
    [[nodiscard]] ResponseStreamWriter& writer() const;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;

    ResponseStreamWriter* writer_;
};

}  // namespace ruvia
