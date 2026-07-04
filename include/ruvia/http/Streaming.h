#pragma once

#include "ruvia/app/Task.h"
#include "ruvia/http/detail/CallableRef.h"

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia {

class Context;

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

    [[nodiscard]] Task<std::optional<std::string_view>> read() {
        return read_();
    }

private:
    detail::CallableRef<std::optional<std::string_view>> read_;
};

class ResponseStreamWriter final {
public:
    ResponseStreamWriter(const ResponseStreamWriter&) = delete;
    ResponseStreamWriter& operator=(const ResponseStreamWriter&) = delete;

    Task<void> write(std::string_view chunk) {
        return write_(target_, chunk);
    }

    // Writes chunk followed by "\n" as a single chunk.
    Task<void> writeln(std::string_view chunk) {
        auto& buffer = scratch_(target_);
        buffer.clear();
        buffer.reserve(chunk.size() + 1);
        if (!chunk.empty()) {
            buffer.append(chunk.data(), chunk.size());
        }
        buffer.push_back('\n');
        return write_(target_, std::string_view(buffer.data(), buffer.size()));
    }

    // Suspends the handler on the connection's worker without blocking it.
    Task<void> sleep(std::chrono::milliseconds duration) {
        return sleep_(target_, duration);
    }

    // True once the peer is known gone: an HTTP/2 RST_STREAM, or a failed
    // write on HTTP/1.1. HTTP/1.1 cannot observe a disconnect until a write
    // fails, so long-lived producers should keep writing (heartbeats) instead
    // of expecting this to flip on its own.
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_(target_);
    }

    Task<void> end() {
        return end_(target_);
    }

    // Queues a trailer field to be emitted when the stream ends: an HTTP/1.1
    // chunked trailer or an HTTP/2 trailing HEADERS field, depending on the
    // transport. Must be called before the stream is ended; throws on a field
    // name/value that is not a valid or permissible trailer.
    void addTrailer(std::string_view name, std::string_view value) {
        addTrailer_(target_, name, value);
    }

private:
    friend struct detail::StreamingAccess;

    using Write = Task<void> (*)(void*, std::string_view);
    using End = Task<void> (*)(void*);
    using Sleep = Task<void> (*)(void*, std::chrono::milliseconds);
    using BindContext = void (*)(void*, Context*) noexcept;
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

    void bindContext(Context& context) noexcept {
        bindContext_(target_, &context);
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

struct SseMessage final {
    std::string_view data;
    std::string_view event;
    std::string_view id;
    std::optional<std::uint32_t> retry;
};

class SseWriter final {
public:
    Task<void> writeSSE(const SseMessage& message);

    Task<void> sleep(std::chrono::milliseconds duration) {
        return writer_.sleep(duration);
    }

    [[nodiscard]] bool aborted() const noexcept {
        return writer_.aborted();
    }

    void addTrailer(std::string_view name, std::string_view value) {
        writer_.addTrailer(name, value);
    }

    Task<void> end() {
        return writer_.end();
    }

private:
    friend class Context;
    friend struct detail::StreamingAccess;

    explicit SseWriter(ResponseStreamWriter& writer) noexcept : writer_(writer) {}

    static void appendData(std::pmr::string& frame, std::string_view data);
    static void appendUnsigned(std::pmr::string& frame, std::uint32_t value);

    ResponseStreamWriter& writer_;
};

}  // namespace ruvia
