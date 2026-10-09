#pragma once

#include <atomic>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/web_worker.h"

#include "server/http_connection_state.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
#include "server/http_server_worker_state.h"
#include "server/native_accepted_socket_ticket.h"

namespace ruvia::detail {

using tcp_socket_type = asio::ip::tcp::socket;
class accepted_connection_lease;
class context_services;
class route_table;
class worker_capabilities;

// Worker-local connection resources and session assembly. The surrounding Web
// runtime owns lifecycle phases and the task_scope that joins all sessions.
class worker_connections final {
public:
    worker_connections(asio::io_context& io, const worker_handle& worker_value,
        worker_memory& memory, const route_table& routes_value, worker_capabilities& capabilities,
        http_server_options& options, const stop_token& stop_token_value,
        http_server_worker_state& state_value, task_scope& tasks,
        std::span<const http_server_listener_definition> listeners);
    worker_connections(const worker_connections&) = delete;
    worker_connections& operator=(const worker_connections&) = delete;
    void prepare();
    void open_admission() noexcept;
    void close_admission() noexcept {
        serving_.store(false, std::memory_order_release);
    }
    void stop() noexcept;
    void retire_tls() noexcept;
    [[nodiscard]] bool available() const noexcept;
    void accept(native_accepted_socket_ticket&& ticket) noexcept;
    [[nodiscard]] http_server_stats stats() const noexcept;
    [[nodiscard]] connection_scanner& scanner() noexcept {
        return scanner_;
    }
    [[nodiscard]] http_server_session_config& listener(std::size_t index) noexcept {
        return *listeners_[index];
    }
    [[nodiscard]] std::atomic<std::size_t>& active_counter() noexcept {
        return active_;
    }
    [[nodiscard]] std::atomic<std::size_t>& refused_counter() noexcept {
        return refused_;
    }

private:
    void configure_tls(http_server_session_config& session);
    void accept_socket(std::size_t index, tcp_socket_type socket);
    task<void> run_session(http_server_session_config& listener, accepted_connection_lease connection);
    template <typename stream_type>
    task<void> run_stream(http_server_session_config& listener_value, stream_type& stream, tcp_socket_type& socket, context_services services);
    template <typename stream_type>
    task<void> run_http2(stream_type& stream, tcp_socket_type& socket, context_services services, std::string_view initial = {});
    asio::io_context& io_;
    const worker_handle& worker_;
    worker_memory& memory_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    http_server_options& options_;
    const stop_token& stop_token_;
    http_server_worker_state& state_;
    task_scope& tasks_;
    std::pmr::vector<std::unique_ptr<http_server_session_config, pmr_object_deleter<http_server_session_config>>> listeners_;
    connection_scanner scanner_;
    connection_work_set_pool work_sets_;
    // Caller-thread statistics and ingress observations require atomic reads.
    std::atomic<std::size_t> active_{};
    std::atomic<std::size_t> refused_{};
    std::atomic<std::size_t> failures_{};
    std::atomic<std::size_t> accept_failures_{};
    std::atomic<bool> serving_{};
};

}  // namespace ruvia::detail
