#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/Sse.h"
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
    constexpr BodyReader(Token, void* target, detail::CallableRef<std::optional<std::string_view>>::Invoke read) noexcept
        : read_(target, read) {}

    BodyReader(const BodyReader&) = delete;
    BodyReader& operator=(const BodyReader&) = delete;

    /// Reads the next chunk of the streamed request body.
    /// @warning The returned view borrows the connection read buffer and is only valid
    /// until the NEXT read() call; copy it out if you need to retain it past then. An
    /// empty optional signals end-of-body.
    [[nodiscard]] Task<std::optional<std::string_view>> read();

private:
    detail::CallableRef<std::optional<std::string_view>> read_;
};

class ResponseStreamWriter final {
public:
    ResponseStreamWriter(const ResponseStreamWriter&) = delete;
    ResponseStreamWriter& operator=(const ResponseStreamWriter&) = delete;

    Task<void> write(std::string_view chunk);

    Task<void> writeln(std::string_view chunk);

    Task<void> sleep(std::chrono::milliseconds duration);

    [[nodiscard]] bool aborted() const noexcept {
        return aborted_(target_);
    }

    /// Atomically terminates the stream with an optional trailer section.
    /// The header views only need to remain valid until the returned task completes.
    /// A non-empty section is never silently dropped when the selected HTTP
    /// version/method/status cannot represent trailers. If the stream is still
    /// uncommitted, that rejection occurs before the response head is emitted.
    Task<void> end(std::span<const HttpHeaderView> trailers = {});

private:
    friend struct detail::StreamingAccess;

    using Write = Task<void> (*)(void*, std::string_view);
    using End = Task<void> (*)(void*, std::span<const HttpHeaderView>);
    using Sleep = Task<void> (*)(void*, std::chrono::milliseconds);
    using StreamingHeadThunk = HttpResponse (*)(Context&);
    using BindContext = void (*)(void*, Context*, StreamingHeadThunk) noexcept;
    using Scratch = std::pmr::string& (*)(void*) noexcept;
    using Committed = bool (*)(void*) noexcept;
    using Aborted = bool (*)(void*) noexcept;

    constexpr ResponseStreamWriter(
        void* target,
        Write write,
        End end,
        Sleep sleep,
        BindContext bindContext,
        Scratch scratch,
        Committed committed,
        Aborted aborted) noexcept
        : target_(target),
          write_(write),
          end_(end),
          sleep_(sleep),
          bindContext_(bindContext),
          scratch_(scratch),
          committed_(committed),
          aborted_(aborted) {}

    void bindContext(Context& context, StreamingHeadThunk streamingHead) noexcept {
        bindContext_(target_, &context, streamingHead);
    }

    [[nodiscard]] std::pmr::string& scratch() const {
        return scratch_(target_);
    }

    [[nodiscard]] bool committed() const noexcept {
        return committed_(target_);
    }

    void* target_;
    Write write_;
    End end_;
    Sleep sleep_;
    BindContext bindContext_;
    Scratch scratch_;
    Committed committed_;
    Aborted aborted_;
};

class SseWriter final {
public:
    Task<void> writeSSE(const SseMessage& message);

    Task<void> sleep(std::chrono::milliseconds duration);

    [[nodiscard]] bool aborted() const noexcept {
        return writer_.aborted();
    }

    Task<void> end(std::span<const HttpHeaderView> trailers = {});

private:
    friend class Context;
    friend struct detail::StreamingAccess;

    explicit SseWriter(ResponseStreamWriter& writer) noexcept : writer_(writer) {}

    ResponseStreamWriter& writer_;
};

}  // namespace ruvia
