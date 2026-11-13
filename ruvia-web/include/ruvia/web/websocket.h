#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_status.h"
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
    std::vector<std::string> subprotocols_{};
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
[[nodiscard]] websocket make_websocket_facade(connection_type& connection);
struct websocket_accept_access;
class websocket_state;
// Whether a write submits a whole message or one fragment of one.
enum class websocket_write_extent : std::uint8_t {
    message,
    fragment,
    final_fragment,
};
}  // namespace detail

class websocket_message_writer;

// A handshake the server refused. The server sends the selected response,
// with this status, after the request callback returns.
struct websocket_rejection final {
    http_status_code status_;
    // The failed handshake check, as in the error response's code:
    // invalid_websocket_handshake, unsupported_websocket_version or
    // unsupported_websocket_subprotocol.
    std::string_view code_;
};

// Outcome of accepting a WebSocket upgrade, fixed at construction: the
// established session, borrowed until the request callback ends, or the
// rejection.
class websocket_accept_result final {
public:
    websocket_accept_result(const websocket_accept_result&) = delete;
    websocket_accept_result& operator=(const websocket_accept_result&) = delete;
    websocket_accept_result(websocket_accept_result&&) noexcept = default;
    websocket_accept_result& operator=(websocket_accept_result&&) = delete;

    // nullptr when the handshake was rejected.
    [[nodiscard]] websocket* session() const noexcept {
        const auto* accepted = std::get_if<websocket*>(&value_);
        return accepted != nullptr ? *accepted : nullptr;
    }
    // nullptr when the session was established.
    [[nodiscard]] const websocket_rejection* rejection() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_rejection>(&value_);
    }
    const websocket_rejection* rejection() const&& = delete;

private:
    friend struct detail::websocket_accept_access;
    explicit websocket_accept_result(websocket& session) noexcept
        : value_(&session) {}
    explicit websocket_accept_result(websocket_rejection rejection) noexcept
        : value_(rejection) {}
    std::variant<websocket*, websocket_rejection> value_;
};

// One worker-affine session API for both roles. Client handshakes return an owning
// handle; server acceptance borrows the request lifetime. Moving preserves writers
// and cold/running operations, but never extends a server request lifetime.
class websocket final {
public:
    websocket(const websocket&) = delete;
    websocket& operator=(const websocket&) = delete;
    websocket(websocket&&) noexcept;
    websocket& operator=(websocket&&) noexcept;
    ~websocket();

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

    /// The string_view overloads copy payloads into owned storage before
    /// returning. std::string&& inputs transfer their existing allocation.
    /// Output operations share one write lane: starting a second before the
    /// current operation completes throws std::logic_error. A stop request or
    /// timeout in `operation` aborts the session (a partly written frame
    /// cannot be withdrawn) and throws std::system_error (operation_canceled
    /// or timed_out); an already-stopped token fails before any output.
    [[nodiscard]] task<void> text(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> text(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Zero-copy text frame: takes ownership of an already-allocated payload.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> text(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write_owned(websocket_opcode::text, std::move(payload), options.compress_, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> text(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> binary(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> binary(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> binary(std::span<const std::byte> payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> binary(std::span<const std::byte>, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Zero-copy binary frame.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> binary(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write_owned(websocket_opcode::binary, std::move(payload), options.compress_, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> binary(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> pong(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> pong(std::string_view = {}, operation_options = {}) && = delete;

    /// Zero-copy pong frame.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> pong(string_type&& payload, operation_options operation = {}) & {
        return write_owned(websocket_opcode::pong, std::move(payload), true, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> pong(string_type&&, operation_options = {}) && = delete;

    [[nodiscard]] task<void> ping(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> ping(std::string_view = {}, operation_options = {}) && = delete;

    /// Zero-copy ping frame.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> ping(string_type&& payload, operation_options operation = {}) & {
        return write_owned(websocket_opcode::ping, std::move(payload), true, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
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

    /// Starts a data message sent as several frames; see websocket_message_writer.
    /// Throws std::logic_error while another such message is open.
    [[nodiscard]] websocket_message_writer begin_text(websocket_send_options options = {}) &;
    websocket_message_writer begin_text(websocket_send_options = {}) && = delete;
    [[nodiscard]] websocket_message_writer begin_binary(websocket_send_options options = {}) &;
    websocket_message_writer begin_binary(websocket_send_options = {}) && = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND;
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

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] const worker_ref& worker() const& RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    friend class http_client_websocket_request;
    template <typename connection_type>
    friend websocket detail::make_websocket_facade(connection_type&);
    explicit websocket(std::shared_ptr<detail::websocket_state> state) noexcept;
    [[nodiscard]] task<void> write_owned(websocket_opcode, std::string&&, bool, operation_options);
    std::shared_ptr<detail::websocket_state> state_;
};

/// A data message sent as several frames (RFC 6455 section 5.4), so that a
/// caller can pace a large message, for example under a shared bandwidth
/// budget. Each write() sends one non-final frame and completes once the
/// transport accepted it; finish() sends the final frame. Fragments of a
/// compressed message continue one permessage-deflate stream. Until finish()
/// completes the message owns the data lane: text(), binary(), send() and
/// another begin_*() throw std::logic_error, while ping(), pong(), close() and
/// automatic control frames may go out between fragments. Fragment operations
/// share the write lane with them and accept the same operation_options.
/// Destroying or abort()ing an unfinished message aborts the session, since
/// the peer could not tell where the message ends. The writer borrows the
/// websocket and expires with it.
class websocket_message_writer final {
public:
    websocket_message_writer(websocket_message_writer&& other) noexcept;
    websocket_message_writer& operator=(websocket_message_writer&&) = delete;
    websocket_message_writer(const websocket_message_writer&) = delete;
    websocket_message_writer& operator=(const websocket_message_writer&) = delete;
    ~websocket_message_writer();

    [[nodiscard]] task<void> write(std::string_view chunk, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> chunk, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& chunk, operation_options operation = {}) & {
        return submit_owned(std::move(chunk), false, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(std::string_view last_chunk = {}, operation_options operation = {}) &;
    task<void> finish(std::string_view = {}, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(std::span<const std::byte> last_chunk, operation_options operation = {}) &;
    task<void> finish(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> finish(string_type&& last_chunk, operation_options operation = {}) & {
        return submit_owned(std::move(last_chunk), true, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> finish(string_type&&, operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class detail::websocket_state;
    websocket_message_writer(detail::websocket_state& owner, websocket_opcode opcode, bool compress) noexcept;
    [[nodiscard]] task<void> submit(std::string_view chunk, bool final, operation_options operation);
    [[nodiscard]] task<void> submit_owned(
        std::string&& chunk, bool final, operation_options operation);
    [[nodiscard]] bool message_open() const noexcept;
    static void expire_capability(void* target) noexcept;

    detail::websocket_state* owner_;
    std::uint64_t message_id_;
    websocket_opcode opcode_;
    bool compress_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
