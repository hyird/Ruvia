#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"

#include "server/connection_count.h"
#include "server/connection_scanner.h"
#include "server/http_connection_close.h"
#include "server/http_connection_state.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
#include "server/http_server_tls_contexts.h"
#include "server/http_server_worker_state.h"
#if defined(_WIN32)
#include "server/windows_acceptor.h"
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3/http3_endpoint.h"
#include "http3/http3_worker_directory.h"
#endif

namespace ruvia {
class worker_runtime;
}

namespace ruvia::detail {

using tcp_socket_type = asio::ip::tcp::socket;
class accepted_connection_lease;
class context_services;
template <typename stream_type>
struct http1_connection_session;
class route_table;
struct server_connection_fields;
class worker_capabilities;

// What serve_requests hands back to run_stream when the connection stays open:
// no input is left buffered (the next request is awaited without a work set),
// or a complete prior-knowledge HTTP/2 preface is pending in the work set's
// read buffer (RFC 9113 §3.3).
enum class http1_serve_handoff : std::uint8_t {
    await_request,
    cleartext_http2,
};

#if !defined(_WIN32)
// One worker's dup()s of a listener's sockets; udp_ is invalid without an
// HTTP/3 service.
struct listener_socket_duplicates final {
    native_socket tcp_{invalid_native_socket};
    native_socket udp_{invalid_native_socket};
};
#endif

// Worker-local listeners, connection resources and session assembly. The
// surrounding runtime owns lifecycle phases and the scope joining every task.
// Admission runs open -> stop, and stopping closes every connection at once:
// there is no drain. On POSIX each worker binds every TCP listener with
// SO_REUSEPORT and accepts on its own event loop; on Windows the
// application's acceptor accepts and hands each socket over (adopt_socket).
// A TLS listener with an HTTP/3 service also gets a QUIC endpoint
// (http3_endpoint), which admits and stops the same way.
//
// Listener slots are numbered alike on every worker (and in the Windows
// acceptor) and never reused within a run, so a hand-over queued for a
// removed listener cannot reach another. A listener added while running is
// prepared (bound, not admitting) on every worker before any opens it; a
// removed one stops admitting at once and retires -- slot emptied, its
// session configuration destroyed -- once every task it owns has ended.
class worker_connections final {
public:
    worker_connections(asio::io_context& io, const worker_handle& worker_value,
        const route_table& routes_value, worker_capabilities& capabilities,
        http_server_options& options, const stop_token& stop_token_value,
        http_server_worker_state& state_value, task_scope& tasks,
        std::span<const http_server_listener_definition> listeners, worker_failure_sink failure, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_placement http3_placement
#endif
    );
    worker_connections(const worker_connections&) = delete;
    worker_connections& operator=(const worker_connections&) = delete;
    void prepare();
    void open_admission() noexcept;
    // Worker thread, once the worker state left running: stops admission and
    // closes every connection at once.
    void stop() noexcept;
    void retire_tls() noexcept;
    // Worker thread: listener `listener_index` -- TLS over TCP and its
    // HTTP/3 service alike -- selects every later handshake's identity from
    // `identities`.
    void adopt_tls_identities(
        std::size_t listener_index, std::shared_ptr<const http_server_tls_identities> identities) noexcept;
    // Worker thread, the controls of application::add_listener(): prepares
    // `definition` in slot `index` as prepare() would -- TLS contexts, bound
    // or adopted sockets, the HTTP/3 endpoint in slot `http3_index` -- without
    // admitting, or throws with the slot left empty.
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition,
        bool externally_accepted = false
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory = nullptr
#endif
    );
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition, bool externally_accepted,
        prepared_http_server_session session
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory = nullptr
#endif
    );
    // 唤醒准备阶段已经创建的接入任务；提交不再分配监听资源。
    void open_listener(std::size_t index) noexcept;
    // Closes the sockets of a listener prepared but never opened and retires
    // it; an empty slot is left alone.
    void discard_listener(std::size_t index, std::shared_ptr<void> receipt = {}) noexcept;
    // application::remove_listener(): stops admitting listener `index` and
    // closes its TCP socket at once, holding `receipt` until its accept loop
    // exited. Its connections are served on; its HTTP/3 endpoint stops
    // admitting and closes its socket after its last connection.
    void remove_listener(std::size_t index, std::shared_ptr<void> receipt, listener_remove_options options = {}) noexcept;
#if !defined(_WIN32)
    // application::duplicate_listener_sockets(): this worker's sockets of
    // listener `index`, duplicated. Throws std::logic_error when it no longer
    // admits.
    [[nodiscard]] listener_socket_duplicates duplicate_listener(std::size_t index);
#endif
    // Takes an already accepted native socket, including ownership on failure.
    // It first binds that socket to this worker's execution context.
    void adopt_connection(std::size_t index, native_socket socket);
    [[nodiscard]] http_server_stats stats() const noexcept;
    // Owning worker only; false for an unknown or retired connection ID.
    [[nodiscard]] bool close_connection(std::uint64_t id) noexcept;
#if defined(_WIN32)
    // This worker as the acceptor's hand-over target.
    [[nodiscard]] windows_acceptor::business_worker acceptor_target() noexcept;
    void bind_acceptor(windows_acceptor& acceptor) noexcept {
#if defined(RUVIA_ENABLE_HTTP3)
        http3_placement_.acceptor_ = &acceptor;
        http3_services_.placement_.acceptor_ = &acceptor;
#else
        (void)acceptor;
#endif
    }
#endif

private:
    struct listener final {
        listener(asio::io_context& io, const worker_handle& worker_value,
            const http_server_listener_definition& definition, std::pmr::memory_resource* resource,
            prepared_http_server_session session = {})
            : session_owner_(std::move(session)),
              session_(session_owner_ ? *session_owner_ : inline_session_.emplace(definition, resource)),
              endpoint_(definition.endpoint_),
              tasks_(worker_value, {.resource_ = resource}),
              retired_(worker_value),
              activated_(worker_value)
#if !defined(_WIN32)
              ,
              socket_(io),
              inherited_(definition.inherited_sockets_)
#endif
        {
#if defined(_WIN32)
            (void)io;
#endif
        }

        prepared_http_server_session session_owner_;
        std::optional<http_server_session_config> inline_session_;
        http_server_session_config& session_;
        asio::ip::tcp::endpoint endpoint_;
        // Its accept loop, sessions and HTTP/3 endpoint tasks, all joined
        // before the listener is destroyed: they borrow session_.
        task_scope tasks_;
        // Notified once removal or the worker's stop ended admission.
        worker_signal retired_;
        worker_signal activated_;
        // 取消准备持有至该监听的全部任务已 join；强制移除不等待调用它的会话自身。
        std::shared_ptr<void> retirement_receipt_;
#if defined(RUVIA_ENABLE_HTTP3)
        std::optional<std::size_t> http3_index_;
#endif
#if !defined(_WIN32)
        asio::ip::tcp::acceptor socket_;
        // Until preparation claimed this worker's sockets.
        std::shared_ptr<inherited_listener_sockets> inherited_;
        // A removal's, until the accept loop exited.
        std::shared_ptr<void> removal_receipt_;
        bool accepting_{false};
#endif
        // From preparation on, a task awaits retired_ to retire the listener.
        bool prepared_{false};
        // From opening until removal or the worker's stop.
        bool admitting_{false};
        bool externally_accepted_{false};
    };
#if defined(RUVIA_ENABLE_HTTP3)
    void add_http3_endpoint(std::size_t listener_index, std::size_t http3_index);
    void receive_http3_handoff(std::size_t listener_index, http3_handoff_datagram& datagram) noexcept;
#endif
    // Builds the listener's TLS contexts, binds or adopts its sockets and
    // starts the task that retires it.
    void prepare_listener_resources(std::size_t index);
    void start_listener(std::size_t index) noexcept;
    void close_listener_sockets(listener& configured) noexcept;
    // Awaits the listener's retired_, joins its tasks, then empties its slot.
    task<void> retire_listener(std::size_t index);
#if defined(_WIN32)
    // The worker thread: binds a socket the acceptor accepted to this
    // worker's io_context (its first IOCP association) and serves it.
    void adopt_socket(std::size_t listener_index, windows_accepted_socket accepted);
#else
    void close_listeners() noexcept;
    task<void> run_accept_loop(std::size_t index);
#endif
    void report_accept_failure(std::exception_ptr failure, bool fatal) noexcept;
    task<void> rotate_tls_ticket_keys();
    void accept_socket(std::size_t index, tcp_socket_type socket);
    // `remote` is the peer accept_socket() read before the socket moved into
    // the lease; absent when the socket no longer knew its peer.
    task<void> run_session(http_server_session_config& listener, accepted_connection_lease connection,
        std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id);
    task<void> run_tls_session(http_server_session_config& listener, accepted_connection_lease connection,
        std::optional<asio::ip::tcp::endpoint> remote, std::uint64_t connection_id);
    void report_session_failure(
        tcp_socket_type& socket, std::string_view remote_address, std::exception_ptr failure) noexcept;
    template <typename stream_type>
    task<http_connection_close> run_stream(http_server_session_config& listener_value, stream_type& stream, tcp_socket_type& socket, context_services services);
#if defined(RUVIA_ENABLE_HTTP2)
    template <typename stream_type>
    task<http_connection_close> run_http2(stream_type& stream, tcp_socket_type& socket, context_services services);
#endif
    // A listener_dispatch::connection listener's connection: application::on_connection
    // owns it until its task completes. `fields` names everything but the
    // local endpoint, which this reads from `socket`.
    template <typename stream_type>
    task<void> serve_connection(stream_type& stream, tcp_socket_type& socket, server_connection_fields fields);
    template <typename stream_type>
    task<std::variant<http1_serve_handoff, http_connection_close>> serve_requests(
        http1_connection_session<stream_type>& session, connection_work_set& work_set);
    asio::io_context& io_;
    const worker_handle& worker_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    http_server_options& options_;
    const stop_token& stop_token_;
    http_server_worker_state& state_;
    task_scope& tasks_;
    worker_failure_sink failure_;
    // This worker's position among the application's workers: the inherited
    // socket of each listener it claims.
    std::size_t worker_index_;
    // Indexed by listener slot; empty once a listener retired or failed to
    // prepare.
    std::pmr::vector<std::unique_ptr<listener, pmr_object_deleter<listener>>> listeners_;
    connection_scanner scanner_;
    // Caller-thread statistics and the Windows acceptor's balancing read it.
    connection_count connection_count_;
    connection_id_sequence connection_ids_;
    connection_registry registry_;
    std::atomic<std::size_t> refused_{};
    std::atomic<std::size_t> failures_{};
    std::atomic<std::size_t> accept_failures_{};
    bool rotating_ticket_keys_{false};
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_placement http3_placement_;
    http3_worker_services http3_services_;
    bool http3_attached_{false};
    // Indexed by HTTP/3 slot, like listeners_. Declared last: destroyed
    // before every service its connections borrow.
    std::pmr::vector<std::unique_ptr<http3_endpoint, pmr_object_deleter<http3_endpoint>>> http3_endpoints_;
#endif
};

}  // namespace ruvia::detail
