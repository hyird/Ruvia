#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/Sse.h"
#include "ruvia/web/detail/CallableRef.h"

#include <chrono>
#include <memory_resource>
#include <optional>
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

    Task<void> end();

    void addTrailer(std::string_view name, std::string_view value) {
        addTrailer_(target_, name, value);
    }

private:
    friend struct detail::StreamingAccess;

    using Write = Task<void> (*)(void*, std::string_view);
    using End = Task<void> (*)(void*);
    using Sleep = Task<void> (*)(void*, std::chrono::milliseconds);
    using StreamingHeadThunk = HttpResponse (*)(Context&);
    using BindContext = void (*)(void*, Context*, StreamingHeadThunk) noexcept;
    using Scratch = std::pmr::string& (*)(void*) noexcept;
    using AddTrailer = void (*)(void*, std::string_view, std::string_view);
    using Committed = bool (*)(void*) noexcept;
    using Aborted = bool (*)(void*) noexcept;

    constexpr ResponseStreamWriter(
        void* target,
        Write write,
        End end,
        Sleep sleep,
        BindContext bindContext,
        Scratch scratch,
        AddTrailer addTrailer,
        Committed committed,
        Aborted aborted) noexcept
        : target_(target),
          write_(write),
          end_(end),
          sleep_(sleep),
          bindContext_(bindContext),
          scratch_(scratch),
          addTrailer_(addTrailer),
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
    AddTrailer addTrailer_;
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

    void addTrailer(std::string_view name, std::string_view value) {
        writer_.addTrailer(name, value);
    }

    Task<void> end();

private:
    friend class Context;
    friend struct detail::StreamingAccess;

    explicit SseWriter(ResponseStreamWriter& writer) noexcept : writer_(writer) {}

    ResponseStreamWriter& writer_;
};

}  // namespace ruvia
