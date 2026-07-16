#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/WebSocketProtocol.h"
#include "ruvia/web/ScopedOperation.h"
#include "ruvia/web/detail/BorrowedView.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ruvia {

class WebSocketHeartbeatPolicy final {
public:
    [[nodiscard]] static WebSocketHeartbeatPolicy periodic(
        std::chrono::milliseconds pingInterval,
        std::chrono::milliseconds pongTimeout) {
        if (pingInterval.count() <= 0 || pongTimeout.count() <= 0) {
            throw std::invalid_argument(
                "websocket heartbeat intervals must be greater than zero");
        }
        return WebSocketHeartbeatPolicy(pingInterval, pongTimeout);
    }

    [[nodiscard]] static WebSocketHeartbeatPolicy periodic(
        std::chrono::milliseconds interval) {
        return periodic(interval, interval);
    }

    [[nodiscard]] std::chrono::milliseconds pingInterval() const noexcept {
        return pingInterval_;
    }

    [[nodiscard]] std::chrono::milliseconds pongTimeout() const noexcept {
        return pongTimeout_;
    }

private:
    WebSocketHeartbeatPolicy(
        std::chrono::milliseconds pingInterval,
        std::chrono::milliseconds pongTimeout) noexcept
        : pingInterval_(pingInterval),
          pongTimeout_(pongTimeout) {}

    std::chrono::milliseconds pingInterval_;
    std::chrono::milliseconds pongTimeout_;
};

// Runtime-only liveness policy. Wire framing and close-handshake state remain in
// ruvia-http; timers and transport abort policy belong to the Web runtime.
struct WebSocketLifecycleOptions final {
    std::optional<WebSocketHeartbeatPolicy> heartbeat;
    // A locally initiated Close waits for the peer Close before the underlying
    // transport is ended. nullopt disables this guard.
    std::optional<std::chrono::milliseconds> closeHandshakeTimeout{
        std::chrono::seconds(5)};
};

struct WebSocketRouteOptions final {
    // Route registration copies this list into startup-owned PMR storage, but
    // the options value itself can be retained before registration. Preserve
    // the zero-copy input while rejecting owning-string rvalues that would
    // leave it with an already-dangling view.
    class BorrowedText final {
    public:
        constexpr BorrowedText() noexcept = default;

        constexpr BorrowedText(std::string_view value) noexcept
            : value_(value) {}

        constexpr BorrowedText(const char* value) noexcept
            : value_(value) {}

        template <typename Traits, typename Allocator>
        constexpr BorrowedText(
            const std::basic_string<char, Traits, Allocator>& value) noexcept
            : value_(value.data(), value.size()) {}

        template <detail::RvalueCharBasicString String>
        BorrowedText(String&&) = delete;

        constexpr BorrowedText& operator=(std::string_view value) noexcept {
            value_ = value;
            return *this;
        }

        constexpr BorrowedText& operator=(const char* value) noexcept {
            value_ = std::string_view(value);
            return *this;
        }

        template <typename Traits, typename Allocator>
        constexpr BorrowedText& operator=(
            const std::basic_string<char, Traits, Allocator>& value) noexcept {
            value_ = std::string_view(value.data(), value.size());
            return *this;
        }

        template <detail::RvalueCharBasicString String>
        BorrowedText& operator=(String&&) = delete;

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return value_;
        }

        [[nodiscard]] constexpr operator std::string_view() const noexcept {
            return value_;
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return value_.empty();
        }

        friend constexpr bool operator==(
            BorrowedText left,
            BorrowedText right) noexcept {
            return left.value_ == right.value_;
        }

        friend constexpr bool operator==(
            BorrowedText left,
            std::string_view right) noexcept {
            return left.value_ == right;
        }

        friend constexpr bool operator==(
            std::string_view left,
            BorrowedText right) noexcept {
            return left == right.value_;
        }

        friend constexpr bool operator==(
            BorrowedText left,
            const char* right) noexcept {
            return left.value_ == right;
        }

        friend constexpr bool operator==(
            const char* left,
            BorrowedText right) noexcept {
            return left == right.value_;
        }

    private:
        std::string_view value_;
    };

    BorrowedText subprotocols;
    WebSocketLifecycleOptions lifecycle{};
};

static_assert(
    sizeof(WebSocketRouteOptions::BorrowedText) == sizeof(std::string_view));

namespace detail {
struct WebSocketAccess;
}  // namespace detail

class WebSocket final {
public:
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    [[nodiscard]] ScopedOperation<std::optional<WebSocketMessage>> read();

    ScopedOperation<void> text(std::string_view payload);

    ScopedOperation<void> binary(std::string_view payload);

    ScopedOperation<void> pong(std::string_view payload);

    ScopedOperation<void> ping(std::string_view payload = {});

    ScopedOperation<void> close(std::uint16_t code = 1000, std::string_view reason = {});
    void abort() noexcept;

private:
    friend struct detail::WebSocketAccess;

    using Read = Task<std::optional<WebSocketMessage>> (*)(void*);
    using Write = Task<void> (*)(void*, WebSocketOpcode, std::string_view);
    using Close = Task<void> (*)(void*, std::uint16_t, std::string_view);
    using Abort = void (*)(void*) noexcept;

    WebSocket(void* target, Read read, Write write, Close close, Abort abort) noexcept
        : target_(target), read_(read), write_(write), close_(close), abort_(abort) {}

    ScopedOperation<void> write(WebSocketOpcode opcode, std::string_view payload);

    void* target_;
    Read read_;
    Write write_;
    Close close_;
    Abort abort_;
    detail::ScopedOperationScope operationScope_;
};

}  // namespace ruvia
