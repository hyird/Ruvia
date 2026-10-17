#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task_scope.h"

#include "server/connection_drain.h"
#include "server/connection_scanner.h"
#include "server/http_connection_close.h"
#include "server/http_connection_state.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
#include "server/http_server_worker_state.h"
#if !defined(_WIN32)
#include "server/native_accepted_socket_ticket.h"
#endif
#if defined(_WIN32)
#include "server/windows_accept_domain.h"
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

// Worker-local connection resources and session assembly. The surrounding Web
// runtime owns lifecycle phases and the task_scope that joins all sessions.
// Admission runs open -> drain (optional) -> stop: draining closes connections
// without an admitted request, has WebSocket sessions start their close
// handshake, and lets admitted requests and tunnels finish; stopping closes
// every connection.
class worker_connections final {
public:
    worker_connections(asio::io_context& io, const worker_handle& worker_value,
        worker_memory& memory, const route_table& routes_value, worker_capabilities& capabilities,
        http_server_options& options, const stop_token& stop_token_value,
        http_server_worker_state& state_value, task_scope& tasks,
        std::span<const http_server_listener_definition> listeners, worker_runtime& runtime);
    worker_connections(const worker_connections&) = delete;
    worker_connections& operator=(const worker_connections&) = delete;
    void prepare();
    void open_admission() noexcept;
    // Worker thread, once the worker state left running.
    void drain() noexcept;
    void stop() noexcept;
    // Lifecycle thread: false when `deadline` passed before every connection closed.
    [[nodiscard]] bool wait_until_drained(std::chrono::steady_clock::time_point deadline);
    void retire_tls() noexcept;
#if !defined(_WIN32)
    [[nodiscard]] bool available() const noexcept;
    void accept(native_accepted_socket_ticket&& ticket) noexcept;
#endif
#if defined(_WIN32)
    void attach_windows_accept(
        windows_accept_domain& domain, std::size_t origin, worker_runtime_context& context);
    [[nodiscard]] task<void> run_windows_accepts();
#endif
    [[nodiscard]] http_server_stats stats() const noexcept;

    [[nodiscard]] connection_scanner& scanner() noexcept {
        return scanner_;
    }
    [[nodiscard]] const http_server_session_config& listener(std::size_t index) const {
        return *listeners_.at(index);
    }
    [[nodiscard]] connection_drain& connection_lifecycle() noexcept {
        return drain_;
    }
    [[nodiscard]] std::atomic<std::size_t>& refused_counter() noexcept {
        return refused_;
    }

private:
#if defined(_WIN32)
    struct native_listener final {
        native_listener(asio::io_context& io, asio::ip::tcp::endpoint endpoint)
            : endpoint_(std::move(endpoint)),
              socket_(io) {}
        asio::ip::tcp::endpoint endpoint_;
        asio::ip::tcp::acceptor socket_;
    };
    void close_listeners() noexcept;
    windows_accept_domain* windows_accept_{};
    std::size_t windows_origin_{};
    std::pmr::vector<std::unique_ptr<native_listener, pmr_object_deleter<native_listener>>> native_listeners_;
#endif
    void report_accept_failure(std::exception_ptr failure, bool fatal) noexcept;
    void configure_tls(http_server_session_config& session);
    void accept_socket(std::size_t index, tcp_socket_type socket);
    task<void> run_session(http_server_session_config& listener, accepted_connection_lease connection);
    task<void> run_tls_session(http_server_session_config& listener, accepted_connection_lease connection);
    void report_session_failure(
        tcp_socket_type& socket, std::string_view remote_address, std::exception_ptr failure) noexcept;
    template <typename stream_type>
    task<http_connection_close> run_stream(http_server_session_config& listener_value, stream_type& stream, tcp_socket_type& socket, context_services services);
    template <typename stream_type>
    task<std::optional<http_connection_close>> serve_requests(
        http1_connection_session<stream_type>& session, connection_work_set& work_set);
    template <typename stream_type>
    task<http_connection_close> run_http2(stream_type& stream, tcp_socket_type& socket, context_services services, std::string_view initial = {});
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
    std::pmr::vector<std::unique_ptr<http_server_session_config, pmr_object_deleter<http_server_session_config>>> listeners_;
    connection_scanner scanner_;
    connection_work_set_pool work_sets_;
    // Caller-thread statistics and ingress observations require atomic reads.
    connection_drain drain_;
    std::atomic<std::size_t> refused_{};
    std::atomic<std::size_t> failures_{};
    std::atomic<std::size_t> accept_failures_{};
#if !defined(_WIN32)
    std::atomic<bool> serving_{};
#endif
};

}  // namespace ruvia::detail
