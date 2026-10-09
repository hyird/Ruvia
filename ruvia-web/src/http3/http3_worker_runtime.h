#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime_context.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http3_server_request_admission.h"
#include "ruvia/http/http3_stream_frames.h"
#include "ruvia/http/http3_var_int.h"

#include "http3/http3_connection_driver.h"
#include "http3/http3_critical_stream_driver.h"
#include "http3/http3_datagram_channel.h"
#include "http3/http3_datagram_endpoint.h"
#include "http3/http3_quic_server_transport.h"
#include "http3/http3_quic_tls_context.h"
#include "http3/http3_quic_wire_owner.h"
#include "http3/http3_server_stream_output.h"
#include "http3/http3_stream_buffer.h"
#include "server/http_server_listener.h"
namespace ruvia::detail {

class http3_worker_server;

[[nodiscard]] bool queue_http3_initial_offer(
    std::pmr::vector<ruvia::quic_initial_offer>& pending_offers,
    const ruvia::quic_initial_offer& offer) noexcept;
[[nodiscard]] http3_worker_datagram_endpoint::pump_result send_http3_version_negotiation(
    ruvia::quic_server& server, ruvia::quic_version_negotiation_plan& plan,
    http3_worker_datagram_endpoint& endpoint);

// Worker packet routing, cooperative budgets, notifications and timer orchestration.
// Connection resources and protocol operations belong to each connection driver.
class http3_worker_runtime final {
public:
    struct worker_target final {
        http3_worker_server* server_{};
        std::size_t max_connections_{};
        std::uint32_t buffer_capacity_{};
        std::size_t max_requests_per_connection_{};
        std::optional<std::chrono::milliseconds> idle_timeout_{};
        std::optional<std::chrono::milliseconds> request_header_timeout_{};
        std::optional<std::chrono::milliseconds> request_body_timeout_{};
        std::optional<std::chrono::milliseconds> write_timeout_{};
    };

    struct failure_notification final {
        void* context_{};
        void (*notify_)(void*, std::exception_ptr) noexcept {};
    };

    http3_worker_runtime(ruvia::worker_runtime_context& runtime,
        asio::ip::udp::endpoint local, const http_server_listener_definition::tls_type& tls_config_value,
        const http3_listen_config& http3_config, worker_target worker_value,
        http3_datagram_channel& datagrams, ruvia::quic_cid_partition partition,
        failure_notification failure);
    ~http3_worker_runtime();

    http3_worker_runtime(const http3_worker_runtime&) = delete;
    http3_worker_runtime& operator=(const http3_worker_runtime&) = delete;

    // All methods are worker-owner only. Stage before http3_worker_server install;
    // spawn its run and run_datagrams before releasing the serving barrier.
    void stage();
    void start();
    void wake() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool drained() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint local_endpoint() const;
    [[nodiscard]] task<void> run_datagrams();
    [[nodiscard]] task<void> join();
    [[nodiscard]] bool runner_started() const noexcept {
        return datagram_run_started_;
    }
    // No datagram runner was launched. Complete ordered stop, then ACK the
    // detached endpoint once drained() is true; the caller keeps the loop alive.
    void abandon_before_launch() noexcept;

private:
    struct worker_link final {
        worker_link(std::pmr::memory_resource* resource, http3_worker_runtime& runtime,
            worker_target configured);

        worker_target target_;
        http3_stream_buffer request_buffer_;
        std::pmr::vector<std::unique_ptr<http3_connection_state,
            pmr_object_deleter<http3_connection_state>>>
            states_;
        std::pmr::vector<http3_connection_state*> state_views_;
        std::pmr::vector<http3_connection_driver> connections_;
        std::optional<http3_stream_buffer::borrowed_block> pending_response_;
        std::optional<http3_stream_control> pending_response_control_;
        bool response_buffer_drained_{};
    };

    [[nodiscard]] bool pump_protocol(http3_quic_server_transport* transport = nullptr,
        http3_worker_datagram_endpoint* endpoint = nullptr) noexcept;
    void require_owner_thread() const noexcept;
    void request_stop_on_owner() noexcept;
    void finish_datagrams() noexcept;
    [[nodiscard]] task<void> run_notifications();
    [[nodiscard]] bool protocol_drained() const noexcept;
    void report_failure(std::exception_ptr failure) noexcept;
    void schedule_monitor() noexcept;
    void monitor(const asio::error_code& error) noexcept;
    [[nodiscard]] bool pump_worker(worker_link& worker) noexcept;
    [[nodiscard]] bool pump_responses(worker_link& worker) noexcept;
    [[nodiscard]] http3_connection_driver* find_connection(
        worker_link& worker, http3_stream_id id) noexcept;
    [[nodiscard]] http3_connection_driver* find_connection(
        worker_link& worker, std::uint64_t epoch, std::uint64_t generation) noexcept;

    asio::io_context& io_context_;
    const std::thread::id owner_thread_;
    worker_memory memory_;
    worker_signal protocol_signal_;
    task_scope notification_tasks_;
    asio::ip::address bind_address_;
    http3_quic_tls_context tls_;
    http3_quic_wire_owner wire_;
    std::unique_ptr<worker_link, pmr_object_deleter<worker_link>> worker_;
    http3_datagram_channel* datagrams_;
    std::pmr::vector<ruvia::quic_initial_offer> pending_offers_;
    asio::steady_timer monitor_timer_;
    failure_notification failure_notification_{};
    std::exception_ptr failure_;
    std::size_t pump_budget_{512};
    std::chrono::milliseconds drain_timeout_{};
    std::chrono::milliseconds handshake_timeout_{};
    http3_settings local_settings_{};
    std::size_t next_packet_connection_{};
    bool native_stopping_{};
    bool staged_{};
    bool running_{};
    bool stopping_{};
    bool monitor_scheduled_{};
    bool failure_reported_{};
    bool datagram_run_started_{};
    bool datagram_run_retired_{};
    bool abandon_requested_{};
    bool transport_activity_for_pump_{};
};

}  // namespace ruvia::detail
