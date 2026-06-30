#pragma once

#include "ruvia/app/Task.h"
#include "ruvia/http/detail/CallableRef.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
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
    using BindContext = void (*)(void*, Context*) noexcept;
    using Scratch = std::pmr::string& (*)(void*) noexcept;
    using AddTrailer = void (*)(void*, std::string_view, std::string_view);

    constexpr ResponseStreamWriter(
        void* target,
        Write write,
        End end,
        BindContext bindContext,
        Scratch scratch,
        AddTrailer addTrailer) noexcept
        : target_(target),
          write_(write),
          end_(end),
          bindContext_(bindContext),
          scratch_(scratch),
          addTrailer_(addTrailer) {}

    void bindContext(Context& context) noexcept {
        bindContext_(target_, &context);
    }

    [[nodiscard]] std::pmr::string& scratch() const {
        return scratch_(target_);
    }

    void* target_;
    Write write_;
    End end_;
    BindContext bindContext_;
    Scratch scratch_;
    AddTrailer addTrailer_;
};

namespace detail {

struct StreamingAccess final {
    using BodyRead = CallableRef<std::optional<std::string_view>>::Invoke;
    using StreamWrite = ResponseStreamWriter::Write;
    using StreamEnd = ResponseStreamWriter::End;
    using StreamBindContext = ResponseStreamWriter::BindContext;
    using StreamScratch = ResponseStreamWriter::Scratch;
    using StreamAddTrailer = ResponseStreamWriter::AddTrailer;

    static void emplaceBodyReader(
        std::optional<BodyReader>& storage,
        void* target,
        BodyRead read) {
        storage.emplace(BodyReader::Token{}, target, read);
    }

    [[nodiscard]] static ResponseStreamWriter makeResponseStreamWriter(
        void* target,
        StreamWrite write,
        StreamEnd end,
        StreamBindContext bindContext,
        StreamScratch scratch,
        StreamAddTrailer addTrailer) noexcept {
        return ResponseStreamWriter(target, write, end, bindContext, scratch, addTrailer);
    }

    static void bindContext(ResponseStreamWriter& writer, Context& context) noexcept {
        writer.bindContext(context);
    }

    [[nodiscard]] static std::pmr::string& scratch(const ResponseStreamWriter& writer) noexcept {
        return writer.scratch();
    }
};

}  // namespace detail

struct SseMessage final {
    std::string_view data;
    std::string_view event;
    std::string_view id;
    std::optional<std::uint32_t> retry;
};

class SseWriter final {
public:
    explicit SseWriter(ResponseStreamWriter& writer) noexcept : writer_(writer) {}

    Task<void> writeSSE(const SseMessage& message) {
        auto& frame = detail::StreamingAccess::scratch(writer_);
        if (!message.event.empty()) {
            frame.append("event: ");
            frame.append(message.event.data(), message.event.size());
            frame.push_back('\n');
        }
        if (!message.id.empty()) {
            frame.append("id: ");
            frame.append(message.id.data(), message.id.size());
            frame.push_back('\n');
        }
        if (message.retry.has_value()) {
            frame.append("retry: ");
            appendUnsigned(frame, *message.retry);
            frame.push_back('\n');
        }
        appendData(frame, message.data);
        frame.push_back('\n');
        co_await writer_.write(frame);
    }

    void addTrailer(std::string_view name, std::string_view value) {
        writer_.addTrailer(name, value);
    }

    Task<void> end() {
        return writer_.end();
    }

private:
    static void appendData(std::pmr::string& frame, std::string_view data) {
        while (true) {
            const auto next = data.find('\n');
            auto line = next == std::string_view::npos ? data : data.substr(0, next);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            frame.append("data: ");
            frame.append(line.data(), line.size());
            frame.push_back('\n');
            if (next == std::string_view::npos) {
                return;
            }
            data.remove_prefix(next + 1);
        }
    }

    static void appendUnsigned(std::pmr::string& frame, std::uint32_t value) {
        std::array<char, 10> buffer;
        const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (ec != std::errc{}) {
            throw std::logic_error("failed to format SSE retry value");
        }
        frame.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
    }

    ResponseStreamWriter& writer_;
};

}  // namespace ruvia
