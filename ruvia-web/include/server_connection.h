#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "borrowed_text.h"
#include "callback_ref.h"
#include "context.h"
#include "ip_address.h"
#include "move_only_function.h"
#include "operation_options.h"
#include "operation_scope.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"

namespace ruvia {

namespace detail {
template <typename stream_type>
class server_connection_session;
class connection_lifecycle;

// What the worker knows of an accepted connection. Every view borrows
// connection-owned storage that outlives the server_connection.
struct server_connection_fields final {
    struct tls_type final {
        std::optional<std::string_view> client_certificate_subject_;
        std::string_view alpn_;
        std::string_view server_name_;
    };

    std::uint64_t id_;
    std::string_view local_address_;
    std::uint16_t local_port_;
    std::string_view remote_address_;
    std::uint16_t remote_port_;
    std::optional<tls_type> tls_;
    const std::shared_ptr<void>* state_{};
    connection_lifecycle* lifecycle_{};
};
}  // namespace detail

// One connection a listener_dispatch::connection listener accepted, handed to
// its host's connection callback: the decrypted byte stream after a TLS
// handshake, the socket's bytes otherwise. The host owns it only while the
// callback task runs; every member runs on that callback's worker.
//
// One read and one output operation (write or finish) may run at a time,
// concurrently with each other. The connection is subject to the server's
// limits: it holds a max_connections_per_worker_ slot (and is never evicted to
// admit another connection), its read buffer counts against the inbound
// buffer budgets, idle_timeout_ closes it after that long without a read or
// write completing, write_timeout_ closes it when a write makes no progress
// for that long, and host shutdown closes it at once and requests its
// stop_token(). A closed connection fails its pending and later operations
// with std::system_error.
class server_connection final {
public:
    server_connection(const server_connection&) = delete;
    server_connection& operator=(const server_connection&) = delete;
    server_connection(server_connection&&) = delete;
    server_connection& operator=(server_connection&&) = delete;

    // conn_info::connection_id() of this connection; equal to connection_id() of its
    // on_tls_client_hello records.
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return id_;
    }

    // The server address the connection reached.
    [[nodiscard]] constexpr ip_endpoint local() const noexcept {
        return local_;
    }

    [[nodiscard]] constexpr ip_endpoint remote() const noexcept {
        return remote_;
    }

    // Null for a cleartext listener.
    [[nodiscard]] constexpr const tls_connection_transport* tls() const& noexcept RUVIA_LIFETIMEBOUND {
        return tls_.has_value() ? &*tls_ : nullptr;
    }
    const tls_connection_transport* tls() const&& = delete;

    // Requested when the application stops.
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept {
        return stop_token_;
    }

    // Borrowed for this callback; copy the handle to retain its dispatcher.
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return worker_;
    }
    const worker_ref& worker() const&& = delete;
    [[nodiscard]] std::shared_ptr<void> connection_state() const noexcept {
        return state_ == nullptr ? std::shared_ptr<void>{} : *state_;
    }

    // The next bytes the peer sent, at most 4 KiB; std::nullopt once the peer
    // ended its send direction (TCP FIN or TLS close_notify, or TCP closed
    // without close_notify). The view stays valid until the next read starts.
    // Reading past the inbound buffer budget throws std::bad_alloc. A timeout
    // or stop in `operation` ends only this read with std::system_error
    // (timed_out or operation_canceled); no input is lost and reading may
    // continue. A non-positive timeout throws std::invalid_argument.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    // Copies `bytes` before returning, then sends them once awaited. A timeout
    // or stop in `operation` of write() or finish() aborts the connection (a
    // partial write cannot be withdrawn) and throws std::system_error
    // (timed_out or operation_canceled); a stop requested before the operation
    // starts sends nothing. The server's write_timeout_ still applies.
    [[nodiscard]] task<void> write(std::string_view bytes, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& bytes, operation_options operation = {}) & {
        return write_owned(std::move(bytes), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;
    // Ends the send direction: TCP shuts down its send side, TLS sends
    // close_notify. Reading remains available until the peer ends its own.
    // Writing afterwards throws std::logic_error; finishing again does nothing.
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    // Closes the connection at once, failing operations still running.
    void abort() & noexcept;
    void abort() && = delete;

private:
    template <typename stream_type>
    friend class detail::server_connection_session;
    using read_type = task<std::optional<std::span<const std::byte>>> (*)(void*, operation_options);
    using write_type = task<void> (*)(void*, std::string_view, operation_options);
    using finish_type = task<void> (*)(void*, operation_options);
    using abort_type = void (*)(void*) noexcept;

    server_connection(const detail::server_connection_fields& fields, const ::ruvia::stop_token& stop_token_value, const worker_ref& worker_value, void* target, read_type read, write_type write, finish_type finish_value, abort_type abort_value) noexcept;
    void require_active() const;
    [[nodiscard]] task<void> write_owned(std::string&& bytes, operation_options operation);

    const worker_ref& worker_;
    void* target_;
    read_type read_;
    write_type write_;
    finish_type finish_;
    abort_type abort_;
    std::uint64_t id_;
    ip_endpoint local_;
    ip_endpoint remote_;
    std::optional<tls_connection_transport> tls_;
    ::ruvia::stop_token stop_token_;
    const std::shared_ptr<void>* state_;
    bool read_active_{false};
    bool write_active_{false};
    bool send_ended_{false};
    ::ruvia::operation_scope operations_;
};

namespace detail {
using connection_callback_ref = callback_ref<task<void>(server_connection&)>;
}  // namespace detail

// Host-owned handler of every listener_dispatch::connection listener.
// application may share one target across workers; http_server_group builds
// a target per worker through its callback factory. When the returned
// task completes the server ends the connection: TLS sends close_notify
// unless the handler finished or aborted it, then the socket closes. A task
// that throws aborts the connection; its exception reaches
// on_connection_failure unless it is a peer reset or close, or I/O the
// server cancelled (timeouts or host shutdown).
// 空函数指针与目标析构重入的契约同 access_log_callback。
using connection_callback = move_only_function<task<void>(server_connection&)>;

}  // namespace ruvia
