#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/http/BorrowedText.h"
#include "ruvia/http/WebSocketProtocol.h"

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
    WebSocketDeflateConfig deflate{};
};

struct WebSocketSendOptions final {
    // False bypasses compression and never adds payload bytes to its dictionary.
    bool compress{true};
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
    ~WebSocket() {
        if (operationScope_.hasPendingOperations() && worker_ != nullptr && !worker_->isCurrent()) {
            std::terminate();
        }
    }

    /// Only one read operation may be outstanding. Creating another before
    /// the current operation completes or is discarded throws std::logic_error.
    [[nodiscard]] ScopedOperation<std::optional<WebSocketMessage>> read() &;
    ScopedOperation<std::optional<WebSocketMessage>> read() && = delete;

    /// The string_view overloads copy payloads into owner-worker PMR storage
    /// before returning. PMR-string inputs with the matching owner allocator
    /// can transfer their existing allocation; incompatible inputs are copied.
    ScopedOperation<void> text(std::string_view payload, WebSocketSendOptions options = {}) &;
    ScopedOperation<void> text(std::string_view, WebSocketSendOptions = {}) && = delete;

    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                 std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> text(Text&& payload, WebSocketSendOptions options = {}) & {
        return text(std::string_view(std::forward<Text>(payload)), options);
    }
    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                    std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> text(Text&&, WebSocketSendOptions = {}) && = delete;

    /// Zero-copy text frame: takes ownership of an already-allocated payload.
    ScopedOperation<void> text(std::pmr::string&& payload, WebSocketSendOptions options = {}) &;
    ScopedOperation<void> text(std::pmr::string&&, WebSocketSendOptions = {}) && = delete;

    ScopedOperation<void> binary(std::string_view payload, WebSocketSendOptions options = {}) &;
    ScopedOperation<void> binary(std::string_view, WebSocketSendOptions = {}) && = delete;

    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                 std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> binary(Text&& payload, WebSocketSendOptions options = {}) & {
        return binary(std::string_view(std::forward<Text>(payload)), options);
    }
    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                    std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> binary(Text&&, WebSocketSendOptions = {}) && = delete;

    /// Zero-copy binary frame.
    ScopedOperation<void> binary(std::pmr::string&& payload, WebSocketSendOptions options = {}) &;
    ScopedOperation<void> binary(std::pmr::string&&, WebSocketSendOptions = {}) && = delete;

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
    using Write = Task<void> (*)(void*, WebSocketOpcode, std::string_view, bool);
    using Close = Task<void> (*)(void*, WebSocketCloseOptions);
    using Abort = void (*)(void*) noexcept;

    WebSocket(std::pmr::memory_resource& resource, const WorkerHandle* worker, void* target, Read read, Write write, Close close, Abort abort) noexcept
        : resource_(&resource),
          worker_(worker),
          target_(target),
          read_(read),
          write_(write),
          close_(close),
          abort_(abort) {}

    void requireActive() const {
        if (worker_ != nullptr && !worker_->isCurrent()) {
            std::terminate();
        }
        if (!operationScope_.active()) {
            throw std::logic_error("websocket lifetime has expired");
        }
    }

    ScopedOperation<void> write(WebSocketOpcode opcode, std::string_view payload, bool compress = true);
    ScopedOperation<void> write(WebSocketOpcode opcode, std::pmr::string&& payload, bool compress = true);

    std::pmr::memory_resource* resource_;
    const WorkerHandle* worker_;
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
