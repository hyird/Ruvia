#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
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
    // transport is ended. The timeout starts when the Close is submitted, so it
    // also bounds writing the Close frame. nullopt disables this guard.
    std::optional<std::chrono::milliseconds> close_handshake_timeout_{std::chrono::seconds(5)};
};

struct websocket_route_config final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    std::vector<std::string> subprotocols_{};
    // By default a client offering none of subprotocols_ is still accepted,
    // with no Sec-WebSocket-Protocol response field (RFC 6455 4.2.2). When
    // set, such an upgrade is refused with 400 before any middleware or
    // handler runs; subprotocols_ must then be nonempty.
    bool require_subprotocol_{false};
    websocket_lifecycle_options lifecycle_{};
    websocket_deflate_config deflate_{};
};

struct websocket_send_options final {
    // False bypasses compression and never adds payload bytes to its dictionary.
    bool compress_{true};
};

struct websocket_close_options final {
    websocket_close_code code_{websocket_close_status::normal_closure};
    ::ruvia::borrowed_text reason_{};
};

class websocket;

namespace detail {
template <typename connection_type>
[[nodiscard]] websocket make_websocket_facade(connection_type& connection, std::pmr::memory_resource& resource) noexcept;
}  // namespace detail

class websocket final {
public:
    websocket(const websocket&) = delete;
    websocket& operator=(const websocket&) = delete;
    ~websocket() {
        if (operation_scope_.has_pending_operations() && !worker_->is_current()) {
            std::terminate();
        }
    }

    /// Only one read operation may run at a time. Starting another before the
    /// current operation completes throws std::logic_error. Unstarted operations may coexist.
    /// One reader may overlap one writer. A reader waiting behind an in-flight
    /// message polls control frames at message boundaries before the writer's
    /// next operation; the current complete message is never interrupted.
    [[nodiscard]] task<std::optional<websocket_message>> read() &;
    task<std::optional<websocket_message>> read() && = delete;

    /// The string_view overloads copy payloads into owner-worker PMR storage
    /// before returning. PMR-string inputs with the matching owner allocator
    /// can transfer their existing allocation; incompatible inputs are copied.
    /// Output operations share one write lane: starting a second before the
    /// current operation completes throws std::logic_error.
    [[nodiscard]] task<void> text(std::string_view payload, websocket_send_options options = {}) &;
    task<void> text(std::string_view, websocket_send_options = {}) && = delete;

    /// Zero-copy text frame: takes ownership of an already-allocated payload.
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> text(string_type&& payload, websocket_send_options options = {}) & {
        return write(websocket_opcode::text, std::move(payload), options.compress_);
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> text(string_type&&, websocket_send_options = {}) && = delete;

    [[nodiscard]] task<void> binary(std::string_view payload, websocket_send_options options = {}) &;
    task<void> binary(std::string_view, websocket_send_options = {}) && = delete;

    [[nodiscard]] task<void> binary(std::span<const std::byte> payload, websocket_send_options options = {}) &;
    task<void> binary(std::span<const std::byte>, websocket_send_options = {}) && = delete;

    /// Zero-copy binary frame.
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> binary(string_type&& payload, websocket_send_options options = {}) & {
        return write(websocket_opcode::binary, std::move(payload), options.compress_);
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> binary(string_type&&, websocket_send_options = {}) && = delete;

    [[nodiscard]] task<void> pong(std::string_view payload = {}) &;
    task<void> pong(std::string_view = {}) && = delete;

    /// Zero-copy pong frame.
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> pong(string_type&& payload) & {
        return write(websocket_opcode::pong, std::move(payload));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> pong(string_type&&) && = delete;

    [[nodiscard]] task<void> ping(std::string_view payload = {}) &;
    task<void> ping(std::string_view = {}) && = delete;

    /// Zero-copy ping frame.
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> ping(string_type&& payload) & {
        return write(websocket_opcode::ping, std::move(payload));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> ping(string_type&&) && = delete;

    /// Zero-copy send of a message returned by this connection's read(), with
    /// the message's own opcode (for example an echo). The frame borrows the
    /// payload from the connection's read storage, so this call claims the
    /// read lane immediately and holds it until the operation completes or is
    /// destroyed: read() and close() throw std::logic_error meanwhile, and this
    /// call throws std::logic_error while a read is pending. A message built
    /// with websocket_message::borrow over caller storage must keep that
    /// storage alive and unchanged until the operation completes.
    [[nodiscard]] task<void> send(websocket_message&& message, websocket_send_options options = {}) &;
    task<void> send(websocket_message&&, websocket_send_options = {}) && = delete;

    /// Started close cannot overlap a running read, close, or output operation.
    [[nodiscard]] task<void> close(websocket_close_options options = {}) &;
    task<void> close(websocket_close_options = {}) && = delete;
    void abort() noexcept;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const noexcept;
    [[nodiscard]] std::string_view peer_close_reason() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view peer_close_reason() const&& = delete;

private:
    template <typename connection_type>
    friend websocket detail::make_websocket_facade(
        connection_type& connection, std::pmr::memory_resource& resource) noexcept;

    using read_type = task<std::optional<websocket_message>> (*)(void*);
    using write_type = task<void> (*)(void*, websocket_opcode, std::string_view, bool);
    using close_type = task<void> (*)(void*, websocket_close_options);
    using abort_type = void (*)(void*) noexcept;
    using peer_close_code_type = std::optional<std::uint16_t> (*)(void*) noexcept;
    using peer_close_reason_type = std::string_view (*)(void*) noexcept;

    websocket(std::pmr::memory_resource& resource, const worker_handle& worker_value, void* target,
        std::string_view subprotocol_value, read_type read, write_type write, close_type close,
        abort_type abort, peer_close_code_type peer_close_code,
        peer_close_reason_type peer_close_reason) noexcept
        : resource_(&resource),
          worker_(&worker_value),
          target_(target),
          subprotocol_(subprotocol_value),
          read_(read),
          write_(write),
          close_(close),
          abort_(abort),
          peer_close_code_(peer_close_code),
          peer_close_reason_(peer_close_reason) {
        operation_scope_.bind_worker(worker_value);
    }

    void require_active() const {
        if (!worker_->is_current()) {
            std::terminate();
        }
        if (!operation_scope_.active()) {
            throw std::logic_error("websocket lifetime has expired");
        }
    }

    task<void> write(websocket_opcode opcode, std::string_view payload, bool compress = true);
    task<void> write(websocket_opcode opcode, std::pmr::string&& payload, bool compress = true);

    std::pmr::memory_resource* resource_;
    const worker_handle* worker_;
    void* target_;
    std::string_view subprotocol_;
    read_type read_;
    write_type write_;
    close_type close_;
    abort_type abort_;
    peer_close_code_type peer_close_code_;
    peer_close_reason_type peer_close_reason_;
    bool read_active_{false};
    bool write_active_{false};
    bool close_active_{false};
    ::ruvia::operation_scope operation_scope_;
};

}  // namespace ruvia
