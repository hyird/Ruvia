#pragma once

#include <array>
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
#include "http3/http3_datagram_endpoint.h"
#include "http3/http3_quic_tls_context.h"
#include "http3/http3_quic_wire_owner.h"
#include "server/connection_drain.h"
#include "server/connection_scanner.h"
#include "server/http_server_listener.h"

namespace ruvia::detail {

class route_table;
class worker_capabilities;
struct http_server_options;

// The worker-local HTTP/3 server component, plugged into the worker lifecycle
// (stage -> start -> serve -> stop -> join). The worker binds its own UDP
// socket to the shared listen endpoint in start (the prepare phase); serve
// begins receiving once every worker is bound. One protocol loop on the
// worker's own loop owns the QUIC wire and every connection slot; it runs only
// when a datagram, QUIC/local timer, send completion or handler event marks
// work ready. Core owns the execution loop.
//
// Linux steers datagrams to the group socket named by the destination
// connection id partition (http3_udp_socket). The kernel numbers group members
// by bind order, which no worker can observe, so serve starts with a probe:
// every worker sends one datagram whose partition word is its own worker index
// to the listen endpoint, and the worker that receives it learns its group
// index, which becomes the partition it generates connection ids in. A worker
// that receives no probe or more than one proves the steering ineffective and
// fails startup. Other platforms have no steering: every worker accepts every
// connection id (partition count 1); Windows delivers all datagrams to the
// last bound socket.
class http3_worker final {
public:
    // Worker-wide bound for buffered request bodies and tunnel input.
    static constexpr std::size_t body_budget_bytes = std::size_t{64} * 1024 * 1024;
    static constexpr std::size_t probe_secret_bytes = 32;

    struct failure_notification final {
        void* context_{};
        void (*notify_)(void*, std::exception_ptr) noexcept {};
    };

    // Cold input fixed by the application before any worker launches.
    struct listener_binding final {
        asio::ip::udp::endpoint endpoint_;
        std::uint32_t worker_index_{};
        std::uint32_t worker_count_{1};
        // Shared by every worker of this process; authenticates calibration probes.
        std::array<std::byte, probe_secret_bytes> probe_secret_{};
    };

    http3_worker(worker_runtime_context& runtime, worker_memory& memory,
        const route_table& routes_value, worker_capabilities& capabilities,
        connection_scanner& scanner, const http_server_options& options,
        const stop_token& stop_token_value, const http_server_listener_definition::tls_type& tls,
        http3_listen_config config, connection_drain& active_connections,
        std::atomic<std::size_t>& refused_connections, failure_notification failure);
    ~http3_worker();
    http3_worker(const http3_worker&) = delete;
    http3_worker& operator=(const http3_worker&) = delete;

    // Cold staging before core launches the worker loop.
    void stage(listener_binding binding);
    // Owner-loop operations. start binds the socket; serve calibrates the
    // partition (Linux) and starts the protocol loop; join retires every
    // connection, the wire and the socket on the owning thread.
    void start();
    void serve();
    void drain() noexcept;
    void stop() noexcept;
    [[nodiscard]] task<void> join();

private:
    friend class http3_connection_driver;
    using clock_type = std::chrono::steady_clock;
    using driver_owner = std::unique_ptr<http3_connection_driver, pmr_object_deleter<http3_connection_driver>>;

    static void endpoint_notification(void*, http3_worker_datagram_endpoint::notification_kind) noexcept;
    [[nodiscard]] task<void> run();
    [[nodiscard]] task<void> calibrate_partition();
    [[nodiscard]] task<void> retire_endpoint();
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
    connection_drain& active_connections_;
    std::atomic<std::size_t>& refused_connections_;
    const failure_notification failure_notification_;
    worker_signal signal_;
    http3_body_budget body_budget_;
    std::optional<http3_quic_tls_context> tls_;
    std::optional<http3_worker_datagram_endpoint> endpoint_;
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
    task_scope tasks_;
    std::optional<listener_binding> binding_;
    quic_cid_partition partition_;
    std::exception_ptr failure_;
    std::size_t ready_head_{};
    bool started_{};
    bool serving_{};
    bool draining_{};
    bool stopping_{};
    bool failure_reported_{};
};

}  // namespace ruvia::detail
