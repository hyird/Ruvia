#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/Sse.h"
#include "ruvia/web/ScopedOperation.h"
#include "ruvia/web/detail/CallableRef.h"

#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ruvia {

class Context;
class HttpResponse;

namespace detail {
struct StreamingAccess;
}

class BodyReader final {
private:
    friend struct detail::StreamingAccess;

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
    ScopedOperation<void> write(std::string_view chunk);

    ScopedOperation<void> writeln(std::string_view chunk);

    ScopedOperation<void> sleep(std::chrono::milliseconds duration);

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
    using Sleep = Task<void> (*)(void*, std::chrono::milliseconds);
    using StreamingHeadThunk = HttpResponse (*)(Context&);
    using BindContext = void (*)(void*, Context*, StreamingHeadThunk);
    using ReleaseContext = void (*)(void*) noexcept;
    using Committed = bool (*)(void*) noexcept;
    using Aborted = bool (*)(void*) noexcept;

    ResponseStreamWriter(
        void* target,
        Write write,
        End end,
        Sleep sleep,
        BindContext bindContext,
        ReleaseContext releaseContext,
        Committed committed,
        Aborted aborted) noexcept
        : target_(target),
          write_(write),
          end_(end),
          sleep_(sleep),
          bindContext_(bindContext),
          releaseContext_(releaseContext),
          committed_(committed),
          aborted_(aborted) {}

    void bindContext(Context& context, StreamingHeadThunk streamingHead) {
        bindContext_(target_, &context, streamingHead);
    }

    void releaseContext() noexcept {
        operationScope_.close();
        releaseContext_(target_);
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
    bool outputActive_{false};
    detail::ScopedOperationScope operationScope_;

    friend class SseWriter;
    ScopedOperation<void> writeOwned(std::pmr::string chunk);
};

class SseWriter final {
public:
    ScopedOperation<void> write(const SseMessage& message);

    ScopedOperation<void> sleep(std::chrono::milliseconds duration);

    [[nodiscard]] bool aborted() const noexcept {
        return writer_.aborted();
    }

    ScopedOperation<void> end(std::span<const HttpHeaderView> trailers = {});

private:
    friend class Context;
    friend struct detail::StreamingAccess;

    explicit SseWriter(ResponseStreamWriter& writer) noexcept : writer_(writer) {}

    ResponseStreamWriter& writer_;
};

}  // namespace ruvia
