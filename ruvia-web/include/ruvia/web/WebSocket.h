#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/WebSocketProtocol.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/http/BorrowedText.h"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ruvia {

struct WebSocketHeartbeatConfig final {
    // Absence disables heartbeat. When ping is present and pong is absent,
    // pong defaults to the ping interval during configuration normalization.
    std::optional<std::chrono::milliseconds> pingInterval{};
    std::optional<std::chrono::milliseconds> pongTimeout{};
};

// Runtime-only liveness policy. Wire framing and close-handshake state remain in
// ruvia-http; timers and transport abort policy belong to the Web runtime.
struct WebSocketLifecycleOptions final {
    WebSocketHeartbeatConfig heartbeat{};
    // A locally initiated Close waits for the peer Close before the underlying
    // transport is ended. nullopt disables this guard.
    std::optional<std::chrono::milliseconds> closeHandshakeTimeout{std::chrono::seconds(5)};
};

struct WebSocketRouteConfig final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    std::vector<std::string> subprotocols{};
    WebSocketLifecycleOptions lifecycle{};
};

struct WebSocketCloseOptions final {
    std::uint16_t code{1000};
    ::ruvia::BorrowedText reason{};
};

namespace detail {
struct WebSocketAccess;
}  // namespace detail

class WebSocket final {
public:
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    /// Only one read operation may be outstanding. Creating another before
    /// the current operation completes or is discarded throws std::logic_error.
    [[nodiscard]] ScopedOperation<std::optional<WebSocketMessage>> read() &;
    ScopedOperation<std::optional<WebSocketMessage>> read() && = delete;

    /// The string_view overloads copy payloads into process-owned PMR storage
    /// before returning. Hot-path producers that already hold a buffer in
    /// request-owned storage can move it into the matching PMR-string overload
    /// to skip that copy.
    ScopedOperation<void> text(std::string_view payload) &;
    ScopedOperation<void> text(std::string_view) && = delete;

    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                 std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> text(Text&& payload) & {
        return text(std::string_view(std::forward<Text>(payload)));
    }
    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                    std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> text(Text&&) && = delete;

    /// Zero-copy text frame: takes ownership of an already-allocated payload.
    ScopedOperation<void> text(std::pmr::string&& payload) &;
    ScopedOperation<void> text(std::pmr::string&&) && = delete;

    ScopedOperation<void> binary(std::string_view payload) &;
    ScopedOperation<void> binary(std::string_view) && = delete;

    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                 std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> binary(Text&& payload) & {
        return binary(std::string_view(std::forward<Text>(payload)));
    }
    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                    std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> binary(Text&&) && = delete;

    /// Zero-copy binary frame.
    ScopedOperation<void> binary(std::pmr::string&& payload) &;
    ScopedOperation<void> binary(std::pmr::string&&) && = delete;

    ScopedOperation<void> pong(std::string_view payload) &;
    ScopedOperation<void> pong(std::string_view) && = delete;

    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                 std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> pong(Text&& payload) & {
        return pong(std::string_view(std::forward<Text>(payload)));
    }
    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                    std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> pong(Text&&) && = delete;

    /// Zero-copy pong frame.
    ScopedOperation<void> pong(std::pmr::string&& payload) &;
    ScopedOperation<void> pong(std::pmr::string&&) && = delete;

    ScopedOperation<void> ping(std::string_view payload = {}) &;
    ScopedOperation<void> ping(std::string_view = {}) && = delete;

    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                 std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> ping(Text&& payload) & {
        return ping(std::string_view(std::forward<Text>(payload)));
    }
    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                    std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> ping(Text&&) && = delete;

    /// Zero-copy ping frame.
    ScopedOperation<void> ping(std::pmr::string&& payload) &;
    ScopedOperation<void> ping(std::pmr::string&&) && = delete;

    /// Close cannot overlap a read, another close, or an output operation.
    ScopedOperation<void> close(WebSocketCloseOptions options = {}) &;
    ScopedOperation<void> close(WebSocketCloseOptions = {}) && = delete;
    void abort() noexcept;

private:
    friend struct detail::WebSocketAccess;

    using Read = Task<std::optional<WebSocketMessage>> (*)(void*);
    using Write = Task<void> (*)(void*, WebSocketOpcode, std::string_view);
    using Close = Task<void> (*)(void*, WebSocketCloseOptions);
    using Abort = void (*)(void*) noexcept;

    WebSocket(void* target, Read read, Write write, Close close, Abort abort) noexcept
        : target_(target),
          read_(read),
          write_(write),
          close_(close),
          abort_(abort) {}

    void requireActive() const {
        if (!operationScope_.active()) {
            throw std::logic_error("websocket lifetime has expired");
        }
    }

    ScopedOperation<void> write(WebSocketOpcode opcode, std::string_view payload);
    ScopedOperation<void> write(WebSocketOpcode opcode, std::pmr::string&& payload);

    void* target_;
    Read read_;
    Write write_;
    Close close_;
    Abort abort_;
    bool readActive_{false};
    bool writeActive_{false};
    bool closeActive_{false};
    detail::ScopedOperationScope operationScope_;
};

}  // namespace ruvia
