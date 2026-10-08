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

#include "ruvia/core/Task.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/http/Http3ServerRequestAdmission.h"
#include "ruvia/http/Http3StreamFrames.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/web/detail/http3/Http3CriticalStreamDriver.h"
#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"
#include "ruvia/web/detail/http3/Http3QuicTlsContext.h"
#include "ruvia/web/detail/http3/Http3QuicWireOwner.h"
#include "ruvia/web/detail/http3/Http3ServerStreamOutput.h"
#include "ruvia/web/detail/http3/http3_connection_driver.h"
#include "ruvia/web/detail/http3/http3_datagram_channel.h"
#include "ruvia/web/detail/http3/http3_datagram_endpoint.h"
#include "ruvia/web/detail/http3/http3_stream_buffer.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
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
        http3_worker_server* server{};
        std::size_t max_connections{};
        std::uint32_t buffer_capacity{};
        std::size_t max_requests_per_connection{};
        std::optional<std::chrono::milliseconds> idle_timeout{};
        std::optional<std::chrono::milliseconds> request_header_timeout{};
        std::optional<std::chrono::milliseconds> request_body_timeout{};
        std::optional<std::chrono::milliseconds> write_timeout{};
    };

    struct failure_notification final {
        void* context{};
        void (*notify)(void*, std::exception_ptr) noexcept {};
    };

    http3_worker_runtime(ruvia::WorkerRuntimeContext& runtime,
        asio::ip::udp::endpoint local, const HttpServerListenerDefinition::Tls& tls_config,
        const Http3ListenConfig& http3_config, worker_target worker,
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
    [[nodiscard]] Task<void> run_datagrams();
    [[nodiscard]] Task<void> join();
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

        worker_target target;
        http3_stream_buffer request_buffer;
        std::pmr::vector<std::unique_ptr<http3_connection_state,
            PmrObjectDeleter<http3_connection_state>>>
            states;
        std::pmr::vector<http3_connection_state*> state_views;
        std::pmr::vector<http3_connection_driver> connections;
        std::optional<http3_stream_buffer::borrowed_block> pending_response;
        std::optional<http3_stream_control> pending_response_control;
        bool response_buffer_drained{};
    };

    [[nodiscard]] bool pump_protocol(http3_quic_server_transport* transport = nullptr,
        http3_worker_datagram_endpoint* endpoint = nullptr) noexcept;
    void require_owner_thread() const noexcept;
    void request_stop_on_owner() noexcept;
    void finish_datagrams() noexcept;
    [[nodiscard]] Task<void> run_notifications();
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

    asio::io_context& ioContext_;
    const std::thread::id ownerThread_;
    WorkerMemory memory_;
    WorkerSignal protocol_signal_;
    TaskScope notification_tasks_;
    asio::ip::address bindAddress_;
    http3_quic_tls_context tls_;
    Http3QuicWireOwner wire_;
    std::unique_ptr<worker_link, PmrObjectDeleter<worker_link>> worker_;
    http3_datagram_channel* datagrams_;
    std::pmr::vector<ruvia::quic_initial_offer> pendingOffers_;
    asio::steady_timer monitorTimer_;
    failure_notification failureNotification_{};
    std::exception_ptr failure_;
    std::size_t pumpBudget_{512};
    std::chrono::milliseconds drainTimeout_{};
    std::chrono::milliseconds handshakeTimeout_{};
    Http3Settings localSettings_{};
    std::size_t next_packet_connection_{};
    bool native_stopping_{};
    bool staged_{};
    bool running_{};
    bool stopping_{};
    bool monitorScheduled_{};
    bool failureReported_{};
    bool datagram_run_started_{};
    bool datagram_run_retired_{};
    bool abandon_requested_{};
    bool transportActivityForPump_{};
};

}  // namespace ruvia::detail
