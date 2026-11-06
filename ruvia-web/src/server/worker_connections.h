#pragma once

#include <atomic>
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
#if !defined(_WIN32)
#include <asio/steady_timer.hpp>
#endif

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"

#include "server/connection_count.h"
#include "server/connection_scanner.h"
#include "server/http_connection_close.h"
#include "server/http_connection_state.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
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
class worker_capabilities;

// What serve_requests hands back to run_stream when the connection stays open:
// no input is left buffered (the next request is awaited without a work set),
// or a complete prior-knowledge HTTP/2 preface is pending in the work set's
// read buffer (RFC 9113 §3.3).
enum class http1_serve_handoff : std::uint8_t {
    await_request,
    cleartext_http2,
};

// Worker-local listeners, connection resources and session assembly. The
// surrounding runtime owns lifecycle phases and the scope joining every task.
// Admission runs open -> stop, and stopping closes every connection at once:
// there is no drain. On POSIX each worker binds every TCP listener with
// SO_REUSEPORT and accepts on its own event loop; on Windows the
// application's acceptor accepts and hands each socket over (adopt_socket).
// A TLS listener with an HTTP/3 service also gets a QUIC endpoint
// (http3_endpoint), which admits and stops the same way.
class worker_connections final {
public:
    worker_connections(asio::io_context& io, const worker_handle& worker_value,
        worker_memory& memory, const route_table& routes_value, worker_capabilities& capabilities,
        http_server_options& options, const stop_token& stop_token_value,
        http_server_worker_state& state_value, task_scope& tasks,
        std::span<const http_server_listener_definition> listeners, worker_runtime& runtime
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
    [[nodiscard]] http_server_stats stats() const noexcept;
#if defined(_WIN32)
    // This worker as the acceptor's hand-over target.
    [[nodiscard]] windows_acceptor::business_worker acceptor_target() noexcept;
#endif

private:
    struct listener final {
        listener(asio::io_context& io, const http_server_listener_definition& definition,
            std::pmr::memory_resource* resource)
            : session_(definition, resource),
              endpoint_(definition.endpoint_)
#if !defined(_WIN32)
              ,
              socket_(io),
              retry_(io)
#endif
        {
#if defined(_WIN32)
            (void)io;
#endif
        }

        http_server_session_config session_;
        asio::ip::tcp::endpoint endpoint_;
#if !defined(_WIN32)
        asio::ip::tcp::acceptor socket_;
        asio::steady_timer retry_;
#endif
    };
#if defined(RUVIA_ENABLE_HTTP3)
    void receive_http3_handoff(std::size_t listener_index, http3_handoff_datagram& datagram) noexcept;
#endif
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
        std::optional<asio::ip::tcp::endpoint> remote);
    task<void> run_tls_session(http_server_session_config& listener, accepted_connection_lease connection,
        std::optional<asio::ip::tcp::endpoint> remote);
    void report_session_failure(
        tcp_socket_type& socket, std::string_view remote_address, std::exception_ptr failure) noexcept;
    template <typename stream_type>
    task<http_connection_close> run_stream(http_server_session_config& listener_value, stream_type& stream, tcp_socket_type& socket, context_services services);
    template <typename stream_type>
    task<http_connection_close> run_http2(stream_type& stream, tcp_socket_type& socket, context_services services);
    template <typename stream_type>
    task<std::variant<http1_serve_handoff, http_connection_close>> serve_requests(
        http1_connection_session<stream_type>& session, connection_work_set& work_set);
    asio::io_context& io_;
    const worker_handle& worker_;
    worker_memory& memory_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    http_server_options& options_;
    const stop_token& stop_token_;
    http_server_worker_state& state_;
    task_scope& tasks_;
    worker_runtime& runtime_;
    std::pmr::vector<std::unique_ptr<listener, pmr_object_deleter<listener>>> listeners_;
    connection_scanner scanner_;
    connection_work_set_pool work_sets_;
    // Caller-thread statistics and the Windows acceptor's balancing read it.
    connection_count connection_count_;
    std::atomic<std::size_t> refused_{};
    std::atomic<std::size_t> failures_{};
    std::atomic<std::size_t> accept_failures_{};
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_placement http3_placement_;
    http3_worker_services http3_services_;
    // Declared last: destroyed before every service its connections borrow.
    std::pmr::vector<std::unique_ptr<http3_endpoint, pmr_object_deleter<http3_endpoint>>> http3_endpoints_;
#endif
};

}  // namespace ruvia::detail
