#pragma once

#include "ruvia/app/Task.h"

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

class BodyReader final {
public:
    using Read = Task<std::optional<std::string_view>> (*)(void*);

    constexpr BodyReader(void* target, Read read) noexcept : target_(target), read_(read) {}

    BodyReader(const BodyReader&) = delete;
    BodyReader& operator=(const BodyReader&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return read_ != nullptr;
    }

    [[nodiscard]] Task<std::optional<std::string_view>> read() {
        if (read_ == nullptr) {
            throw std::logic_error("request body is not streamable");
        }
        return read_(target_);
    }

private:
    void* target_{nullptr};
    Read read_{nullptr};
};

class ResponseStreamWriter final {
public:
    using Write = Task<void> (*)(void*, std::string_view);
    using End = Task<void> (*)(void*);
    using BindContext = void (*)(void*, Context*) noexcept;
    using Scratch = std::pmr::string& (*)(void*) noexcept;

    constexpr ResponseStreamWriter(
        void* target,
        Write write,
        End end,
        BindContext bindContext = nullptr,
        Scratch scratch = nullptr) noexcept
        : target_(target), write_(write), end_(end), bindContext_(bindContext), scratch_(scratch) {}

    ResponseStreamWriter(const ResponseStreamWriter&) = delete;
    ResponseStreamWriter& operator=(const ResponseStreamWriter&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return write_ != nullptr && end_ != nullptr;
    }

    Task<void> write(std::string_view chunk) {
        if (write_ == nullptr) {
            throw std::logic_error("response body is not streamable");
        }
        return write_(target_, chunk);
    }

    Task<void> end() {
        if (end_ == nullptr) {
            throw std::logic_error("response body is not streamable");
        }
        return end_(target_);
    }

    void bindContext(Context& context) noexcept {
        if (bindContext_ != nullptr) {
            bindContext_(target_, &context);
        }
    }

    [[nodiscard]] std::pmr::string& scratch() const {
        if (scratch_ != nullptr) {
            return scratch_(target_);
        }
        throw std::logic_error("response stream context is not bound");
    }

private:
    void* target_{nullptr};
    Write write_{nullptr};
    End end_{nullptr};
    BindContext bindContext_{nullptr};
    Scratch scratch_{nullptr};
};

struct SseMessage final {
    std::string_view data;
    std::string_view event;
    std::string_view id;
    std::optional<std::uint32_t> retry;
};

class SseWriter final {
public:
    SseWriter(ResponseStreamWriter& writer, std::pmr::memory_resource* resource) noexcept
        : writer_(writer) {
        (void)resource;
    }

    Task<void> writeSSE(const SseMessage& message) {
        auto& frame = writer_.scratch();
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
