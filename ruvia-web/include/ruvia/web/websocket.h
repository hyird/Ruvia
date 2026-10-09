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

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/websocket_protocol.h"

namespace ruvia {

struct websocket_heartbeat_config final {
    // Absence disables heartbeat. When ping is present and pong is absent,
    // pong defaults to the ping interval during configuration normalization.
    std::optional<std::chrono::milliseconds> ping_interval_{};
    std::optional<std::chrono::milliseconds> pong_timeout_{};
};

// Runtime-only liveness policy. Wire framing and close-handshake state remain in
// ruvia-http; timers and transport abort policy belong to the Web runtime.
struct websocket_lifecycle_options final {
    websocket_heartbeat_config heartbeat_{};
    // A locally initiated Close waits for the peer Close before the underlying
    // transport is ended. nullopt disables this guard.
    std::optional<std::chrono::milliseconds> close_handshake_timeout_{std::chrono::seconds(5)};
    // HTTP/3 only: after publishing local QUIC FIN, wait at most this long for
    // the peer transport FIN. A websocket Close frame is not a transport FIN.
    std::chrono::milliseconds peer_transport_fin_timeout_{std::chrono::seconds(5)};
};

struct websocket_route_config final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    std::vector<std::string> subprotocols_{};
    websocket_lifecycle_options lifecycle_{};
    websocket_deflate_config deflate_{};
};

struct websocket_send_options final {
    // False bypasses compression and never adds payload bytes to its dictionary.
    bool compress_{true};
};

struct websocket_close_options final {
    std::uint16_t code_{1000};
    ::ruvia::borrowed_text reason_{};
};

namespace detail {
struct websocket_access;
}  // namespace detail

class websocket final {
public:
    websocket(const websocket&) = delete;
    websocket& operator=(const websocket&) = delete;
    ~websocket() {
        if (operation_scope_.has_pending_operations() && worker_ != nullptr && !worker_->is_current()) {
            std::terminate();
        }
    }

    /// Only one read operation may be outstanding. Creating another before
    /// the current operation completes or is discarded throws std::logic_error.
    [[nodiscard]] scoped_operation<std::optional<websocket_message>> read() &;
    scoped_operation<std::optional<websocket_message>> read() && = delete;

    /// The string_view overloads copy payloads into owner-worker PMR storage
    /// before returning. PMR-string inputs with the matching owner allocator
    /// can transfer their existing allocation; incompatible inputs are copied.
    scoped_operation<void> text(std::string_view payload, websocket_send_options options = {}) &;
    scoped_operation<void> text(std::string_view, websocket_send_options = {}) && = delete;

    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                 std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> text(text_type&& payload_value, websocket_send_options options = {}) & {
        return text(std::string_view(std::forward<text_type>(payload_value)), options);
    }
    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                    std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> text(text_type&&, websocket_send_options = {}) && = delete;

    /// Zero-copy text frame: takes ownership of an already-allocated payload.
    scoped_operation<void> text(std::pmr::string&& payload, websocket_send_options options = {}) &;
    scoped_operation<void> text(std::pmr::string&&, websocket_send_options = {}) && = delete;

    scoped_operation<void> binary(std::string_view payload, websocket_send_options options = {}) &;
    scoped_operation<void> binary(std::string_view, websocket_send_options = {}) && = delete;

    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                 std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> binary(text_type&& payload_value, websocket_send_options options = {}) & {
        return binary(std::string_view(std::forward<text_type>(payload_value)), options);
    }
    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                    std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> binary(text_type&&, websocket_send_options = {}) && = delete;

    /// Zero-copy binary frame.
    scoped_operation<void> binary(std::pmr::string&& payload, websocket_send_options options = {}) &;
    scoped_operation<void> binary(std::pmr::string&&, websocket_send_options = {}) && = delete;

    scoped_operation<void> pong(std::string_view payload) &;
    scoped_operation<void> pong(std::string_view) && = delete;

    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                 std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> pong(text_type&& payload_value) & {
        return pong(std::string_view(std::forward<text_type>(payload_value)));
    }
    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                    std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> pong(text_type&&) && = delete;

    /// Zero-copy pong frame.
    scoped_operation<void> pong(std::pmr::string&& payload) &;
    scoped_operation<void> pong(std::pmr::string&&) && = delete;

    scoped_operation<void> ping(std::string_view payload = {}) &;
    scoped_operation<void> ping(std::string_view = {}) && = delete;

    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                 std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> ping(text_type&& payload_value) & {
        return ping(std::string_view(std::forward<text_type>(payload_value)));
    }
    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                    std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> ping(text_type&&) && = delete;

    /// Zero-copy ping frame.
    scoped_operation<void> ping(std::pmr::string&& payload) &;
    scoped_operation<void> ping(std::pmr::string&&) && = delete;

    /// Close cannot overlap a read, another close, or an output operation.
    scoped_operation<void> close(websocket_close_options options = {}) &;
    scoped_operation<void> close(websocket_close_options = {}) && = delete;
    void abort() noexcept;

private:
    friend struct detail::websocket_access;

    using read_type = task<std::optional<websocket_message>> (*)(void*);
    using write_type = task<void> (*)(void*, websocket_opcode, std::string_view, bool);
    using close_type = task<void> (*)(void*, websocket_close_options);
    using abort_type = void (*)(void*) noexcept;

    websocket(std::pmr::memory_resource& resource, const worker_handle* worker_value, void* target, read_type read, write_type write, close_type close, abort_type abort) noexcept
        : resource_(&resource),
          worker_(worker_value),
          target_(target),
          read_(read),
          write_(write),
          close_(close),
          abort_(abort) {}

    void require_active() const {
        if (worker_ != nullptr && !worker_->is_current()) {
            std::terminate();
        }
        if (!operation_scope_.active()) {
            throw std::logic_error("websocket lifetime has expired");
        }
    }

    scoped_operation<void> write(websocket_opcode opcode, std::string_view payload, bool compress = true);
    scoped_operation<void> write(websocket_opcode opcode, std::pmr::string&& payload, bool compress = true);

    std::pmr::memory_resource* resource_;
    const worker_handle* worker_;
    void* target_;
    read_type read_;
    write_type write_;
    close_type close_;
    abort_type abort_;
    bool read_active_{false};
    bool write_active_{false};
    bool close_active_{false};
    ::ruvia::operation_scope operation_scope_;
};

}  // namespace ruvia
