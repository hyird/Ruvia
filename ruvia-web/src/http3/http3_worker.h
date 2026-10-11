#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <unordered_map>
#include <vector>

#include <asio/ip/udp.hpp>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime_context.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/quic_types.h"

#include "http3/http3_body_budget.h"
#include "http3/http3_connection_driver.h"
#include "http3/http3_datagram_channel.h"
#include "http3/http3_quic_tls_context.h"
#include "http3/http3_quic_wire_owner.h"
#include "server/connection_scanner.h"
#include "server/http_server_listener.h"

namespace ruvia::detail {

class route_table;
class worker_capabilities;
struct http_server_options;

// The worker-local HTTP/3 server component, plugged into the worker lifecycle
// (stage -> start -> stop -> join). One protocol loop on the worker's own loop
// owns the QUIC wire and every connection slot; it runs only when a datagram,
// QUIC/local timer, output credit or handler event marks work ready. Core owns
// the execution loop; the Acceptor owns the borrowed datagram channel until
// this component's final ACK.
class http3_worker final {
public:
    // Worker-wide bound for buffered request bodies and tunnel input.
    static constexpr std::size_t body_budget_bytes = std::size_t{64} * 1024 * 1024;

    struct failure_notification final {
        void* context_{};
        void (*notify_)(void*, std::exception_ptr) noexcept {};
    };

    http3_worker(worker_runtime_context& runtime, worker_memory& memory,
        const route_table& routes_value, worker_capabilities& capabilities,
        connection_scanner& scanner, const http_server_options& options,
        const stop_token& stop_token_value, const http_server_listener_definition::tls_type& tls,
        http3_listen_config config, std::atomic<std::size_t>& active_connections,
        std::atomic<std::size_t>& refused_connections, failure_notification failure);
    ~http3_worker();
    http3_worker(const http3_worker&) = delete;
    http3_worker& operator=(const http3_worker&) = delete;

    // Cold ingress staging can race cancellation before core launches its loop.
    void stage(http3_datagram_channel&, asio::ip::udp::endpoint, quic_cid_partition);
    void abandon_before_launch() noexcept;
    // Owner-loop operations. join retires every connection, the wire and the
    // channel borrow (final ACK) on the owning thread.
    void start();
    void stop() noexcept;
    [[nodiscard]] task<void> join();

private:
    friend class http3_connection_driver;
    using clock_type = std::chrono::steady_clock;
    using driver_owner = std::unique_ptr<http3_connection_driver, pmr_object_deleter<http3_connection_driver>>;

    [[nodiscard]] task<void> run();
    [[nodiscard]] task<void> run_notifications();
    [[nodiscard]] task<void> yield();
    [[nodiscard]] bool turn();
    void receive(clock_type::time_point now);
    void admit(clock_type::time_point now);
    void expire(clock_type::time_point now);
    void serve_ready(clock_type::time_point now);
    void flush(http3_connection_driver& driver, clock_type::time_point now);
    void begin_stop() noexcept;
    [[nodiscard]] bool connections_retired() const noexcept;
    void report_failure(std::exception_ptr failure) noexcept;
    void finish_channel() noexcept;
    [[nodiscard]] http3_connection_driver* find(ruvia::quic_connection_token token) noexcept;

    // Connection-driver services.
    void mark_ready(http3_connection_driver& driver) noexcept;
    void map_token(ruvia::quic_connection_token token, http3_connection_driver& driver);
    void unmap_token(ruvia::quic_connection_token token) noexcept;
    void spawn_join(http3_connection_driver& driver);
    void wake() noexcept;

    worker_runtime_context& runtime_;
    worker_memory& memory_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    connection_scanner& scanner_;
    const http_server_options& options_;
    const stop_token& stop_token_;
    const http_server_listener_definition::tls_type& tls_config_;
    const http3_listen_config config_;
    std::atomic<std::size_t>& active_connections_;
    std::atomic<std::size_t>& refused_connections_;
    const failure_notification failure_notification_;
    worker_signal signal_;
    http3_body_budget body_budget_;
    std::optional<http3_quic_tls_context> tls_;
    std::optional<http3_quic_wire_owner> wire_;
    std::pmr::vector<driver_owner> drivers_;
    std::pmr::unordered_map<std::uint64_t, http3_connection_driver*> connections_;
    std::pmr::vector<http3_connection_driver*> ready_;
    std::pmr::vector<http3_connection_driver*> send_blocked_;
    std::pmr::vector<ruvia::quic_initial_offer> offers_;
    std::pmr::vector<std::byte> scratch_;
    std::optional<clock_type::time_point> local_deadline_;
    // Declared after every object its tasks borrow; joined before they die.
    task_scope joins_;
    task_scope notifications_;
    task_scope tasks_;
    asio::ip::udp::endpoint endpoint_;
    quic_cid_partition partition_;
    std::atomic<http3_datagram_channel*> staged_channel_{};
    std::atomic<bool> abandoned_{};
    http3_datagram_channel* channel_{};
    std::exception_ptr failure_;
    std::size_t ready_head_{};
    bool started_{};
    bool stopping_{};
    bool native_stopping_{};
    bool failure_reported_{};
};

}  // namespace ruvia::detail
