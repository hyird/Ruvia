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

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_protocol.h"

namespace ruvia {

struct websocket_heartbeat_config final {
    // Absence disables heartbeat. When ping is present and pong is absent,
    // pong defaults to the ping interval during configuration normalization.
    std::optional<timeout_duration> ping_interval_{};
    std::optional<timeout_duration> pong_timeout_{};
};

// Runtime-only liveness policy. Wire framing and close-handshake state remain in
// ruvia-http; timers and transport abort policy belong to the Web runtime.
struct websocket_lifecycle_options final {
    websocket_heartbeat_config heartbeat_{};
    // A locally initiated Close waits for the peer Close before the underlying
    // transport is ended. The timeout starts when the Close is submitted, so it
    // also bounds writing the Close frame. nullopt disables this guard.
    std::optional<timeout_duration> close_handshake_timeout_{std::chrono::seconds(5)};
};

struct websocket_route_config final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    std::pmr::vector<std::pmr::string> subprotocols_{process_resource()};
    // By default a client offering none of subprotocols_ is still accepted,
    // with no Sec-WebSocket-Protocol response field (RFC 6455 4.2.2). When
    // set, such an upgrade is refused with 400 before any middleware or
    // handler runs; subprotocols_ must then be nonempty.
    bool require_subprotocol_{false};
    websocket_lifecycle_options lifecycle_{};
    websocket_deflate_config deflate_{};
    // 单会话接收消息上限（含解压后）；unlimited 继承服务器上限，只能收紧。
    protocol_byte_limit max_message_bytes_{};
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
    /// nullopt is the only end: the peer's or this side's completed close
    /// handshake, EOF, a rejected frame, abort() or a heartbeat/close timeout;
    /// termination() tells which. A stop request or timeout in `operation`
    /// ends only this read with std::system_error (operation_canceled or
    /// timed_out); buffered input, the read lane and the session stay usable.
    [[nodiscard]] task<std::optional<websocket_message>> read(operation_options operation = {}) &;
    task<std::optional<websocket_message>> read(operation_options = {}) && = delete;

    /// The string_view overloads copy payloads into owner-worker PMR storage
    /// before returning. PMR-string inputs with the matching owner allocator
    /// can transfer their existing allocation; incompatible inputs are copied.
    /// Output operations share one write lane: starting a second before the
    /// current operation completes throws std::logic_error. A stop request or
    /// timeout in `operation` aborts the session (a partly written frame
    /// cannot be withdrawn) and throws std::system_error (operation_canceled
    /// or timed_out); an already-stopped token fails before any output.
    [[nodiscard]] task<void> text(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> text(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Zero-copy text frame: takes ownership of an already-allocated payload.
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> text(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write(websocket_opcode::text, std::move(payload), options.compress_, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> text(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> binary(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> binary(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> binary(std::span<const std::byte> payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> binary(std::span<const std::byte>, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Zero-copy binary frame.
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> binary(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write(websocket_opcode::binary, std::move(payload), options.compress_, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> binary(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> pong(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> pong(std::string_view = {}, operation_options = {}) && = delete;

    /// Zero-copy pong frame.
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> pong(string_type&& payload, operation_options operation = {}) & {
        return write(websocket_opcode::pong, std::move(payload), true, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> pong(string_type&&, operation_options = {}) && = delete;

    [[nodiscard]] task<void> ping(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> ping(std::string_view = {}, operation_options = {}) && = delete;

    /// Zero-copy ping frame.
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> ping(string_type&& payload, operation_options operation = {}) & {
        return write(websocket_opcode::ping, std::move(payload), true, std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> ping(string_type&&, operation_options = {}) && = delete;

    /// Zero-copy send of a message returned by this connection's read(), with
    /// the message's own opcode (for example an echo). The frame borrows the
    /// payload from the connection's read storage, so this call claims the
    /// read lane immediately and holds it until the operation completes or is
    /// destroyed: read() and close() throw std::logic_error meanwhile, and this
    /// call throws std::logic_error while a read is pending. A message built
    /// with websocket_message::borrow over caller storage must keep that
    /// storage alive and unchanged until the operation completes.
    [[nodiscard]] task<void> send(websocket_message&& message, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> send(websocket_message&&, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Claims the write lane only, so it may start while a read is pending:
    /// that read returns nullopt once the peer answers (termination()
    /// local_close) or the close-handshake timeout aborts the transport
    /// (timeout). Without a pending read, close() itself reads until the peer
    /// Close and a read started meanwhile throws std::logic_error. A handshake
    /// timeout throws std::system_error (timed_out); `operation` bounds the
    /// whole close like an output operation.
    [[nodiscard]] task<void> close(websocket_close_options options = {}, operation_options operation = {}) &;
    task<void> close(websocket_close_options = {}, operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const noexcept;
    [[nodiscard]] std::string_view peer_close_reason() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view peer_close_reason() const&& = delete;

    /// Why the session ended; none while it is active. The first cause wins
    /// and stays readable once read() has returned nullopt.
    [[nodiscard]] websocket_termination termination() const noexcept;
    /// RFC 6455 frame bytes on the wire (header, mask key and payload, after
    /// permessage-deflate) of data and control frames; the HTTP upgrade and
    /// the HTTP/2 or HTTP/3 framing that carries the frames are excluded.
    [[nodiscard]] std::uint64_t bytes_received() const noexcept;
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept;

private:
    template <typename connection_type>
    friend websocket detail::make_websocket_facade(
        connection_type& connection, std::pmr::memory_resource& resource) noexcept;

    using read_type = task<std::optional<websocket_message>> (*)(void*, operation_options);
    using write_type = task<void> (*)(void*, websocket_opcode, std::string_view, bool, operation_options);
    using close_type = task<void> (*)(void*, websocket_close_options, operation_options);
    using abort_type = void (*)(void*) noexcept;
    using peer_close_code_type = std::optional<std::uint16_t> (*)(void*) noexcept;
    using peer_close_reason_type = std::string_view (*)(void*) noexcept;
    using protocol_type = const websocket_connection& (*)(void*) noexcept;

    websocket(std::pmr::memory_resource& resource, const worker_handle& worker_value, void* target,
        std::string_view subprotocol_value, read_type read, write_type write, close_type close,
        abort_type abort, peer_close_code_type peer_close_code,
        peer_close_reason_type peer_close_reason, protocol_type protocol) noexcept
        : resource_(&resource),
          worker_(&worker_value),
          target_(target),
          subprotocol_(subprotocol_value),
          read_(read),
          write_(write),
          close_(close),
          abort_(abort),
          peer_close_code_(peer_close_code),
          peer_close_reason_(peer_close_reason),
          protocol_(protocol) {
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

    task<void> write(websocket_opcode opcode, std::string_view payload, bool compress, operation_options operation);
    task<void> write(websocket_opcode opcode, std::pmr::string&& payload, bool compress, operation_options operation);

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
    protocol_type protocol_;
    bool read_active_{false};
    bool write_active_{false};
    // A send(websocket_message&&) borrows read storage; close() cannot start
    // its own read under it.
    bool message_send_active_{false};
    ::ruvia::operation_scope operation_scope_;
};

}  // namespace ruvia
