#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/callback.h"

namespace ruvia {

namespace detail {
template <typename stream_type>
class server_connection_session;

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
};
}  // namespace detail

// One connection a listener_dispatch::connection listener accepted, handed to
// application::on_connection: the decrypted byte stream of a TLS listener's
// connection once its handshake completed, the socket's bytes otherwise. The
// server owns it and it is valid only while the callback task runs; every
// member must be used on the worker running that task.
//
// One read and one output operation (write or finish) may run at a time,
// concurrently with each other. The connection is subject to the server's
// limits: it holds a max_connections_per_worker_ slot (and is never evicted to
// admit another connection), its read buffer counts against the inbound
// buffer budgets, idle_timeout_ closes it after that long without a read or
// write completing, write_timeout_ closes it when a write makes no progress
// for that long, and application::stop() closes it at once and requests
// stop_token(). A closed connection fails its pending and later operations
// with std::system_error.
class server_connection final {
public:
    server_connection(const server_connection&) = delete;
    server_connection& operator=(const server_connection&) = delete;
    server_connection(server_connection&&) = delete;
    server_connection& operator=(server_connection&&) = delete;

    // conn_info::id() of this connection; equal to connection_id() of its
    // on_tls_client_hello records.
    [[nodiscard]] constexpr std::uint64_t id() const noexcept {
        return id_;
    }

    // The server address the connection reached.
    [[nodiscard]] constexpr conn_info::endpoint local() const noexcept {
        return local_;
    }

    [[nodiscard]] constexpr conn_info::endpoint remote() const noexcept {
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
    [[nodiscard]] const worker_handle& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return worker_;
    }
    const worker_handle& worker() const&& = delete;

    // The next bytes the peer sent, at most 4 KiB; std::nullopt once the peer
    // ended its send direction (TCP FIN or TLS close_notify, or TCP closed
    // without close_notify). The view stays valid until the next read starts.
    // Reading past the inbound buffer budget throws std::bad_alloc.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read() &;
    task<std::optional<std::span<const std::byte>>> read() && = delete;
    // Copies `bytes` before returning, then sends them once awaited.
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes) &;
    task<void> write(std::span<const std::byte>) && = delete;
    // Ends the send direction: TCP shuts down its send side, TLS sends
    // close_notify. Reading remains available until the peer ends its own.
    // Writing afterwards throws std::logic_error; finishing again does nothing.
    [[nodiscard]] task<void> finish() &;
    task<void> finish() && = delete;
    // Closes the connection at once, failing operations still running.
    void abort() noexcept;

private:
    template <typename stream_type>
    friend class detail::server_connection_session;
    using read_type = task<std::optional<std::span<const std::byte>>> (*)(void*);
    using write_type = task<void> (*)(void*, std::string_view);
    using finish_type = task<void> (*)(void*);
    using abort_type = void (*)(void*) noexcept;

    server_connection(const detail::server_connection_fields& fields, const ::ruvia::stop_token& stop_token_value,
        const worker_handle& worker_value, void* target, read_type read, write_type write, finish_type finish_value,
        abort_type abort_value) noexcept;
    void require_active() const;

    const worker_handle& worker_;
    void* target_;
    read_type read_;
    write_type write_;
    finish_type finish_;
    abort_type abort_;
    std::uint64_t id_;
    conn_info::endpoint local_;
    conn_info::endpoint remote_;
    std::optional<tls_connection_transport> tls_;
    ::ruvia::stop_token stop_token_;
    bool read_active_{false};
    bool write_active_{false};
    bool send_ended_{false};
    ::ruvia::operation_scope operations_;
};

namespace detail {
using connection_callback_ref = callback_ref<task<void>(server_connection&)>;
}  // namespace detail

// application-owned connection handler of every listener_dispatch::connection
// listener, invoked on the worker that accepted the connection; workers call
// the same target concurrently, so it must be thread-safe. When the returned
// task completes the server ends the connection: TLS sends close_notify
// unless the handler finished or aborted it, then the socket closes. A task
// that throws aborts the connection; its exception reaches
// on_connection_failure unless it is a peer reset or close, or I/O the
// server cancelled (timeouts, stop()).
// 空函数指针与目标析构重入的契约同 access_log_callback。
using connection_callback = detail::callback<task<void>(server_connection&)>;

}  // namespace ruvia
