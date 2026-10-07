#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <asio/any_io_executor.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/detail/http3/Http3ServerBodyBudget.h"
#include "ruvia/web/detail/http3/Http3ServerConnection.h"
#include "ruvia/web/detail/http3/http3_connection_state.h"
#include "ruvia/web/detail/http3/http3_ready_scheduler.h"
#include "ruvia/web/detail/http3/http3_stream_buffer.h"

namespace ruvia::detail {

class RouteTable;
class WorkerCapabilities;
struct HttpServerOptions;

// Protocol and handlers share one worker. Only the datagram owner crosses a
// thread boundary; local buffer/state readiness is delivered by WorkerSignal.
class http3_worker_server final {
public:
    struct install_link final {
        http3_stream_buffer* request_buffer{};
        std::span<http3_connection_state* const> connections{};
        http3_stream_buffer::local_callback protocol_ready{};
    };

    http3_worker_server(const WorkerHandle& worker, WorkerMemory& memory,
        const RouteTable& routes, WorkerCapabilities& capabilities,
        ConnectionScanner& connection_scanner, asio::any_io_executor executor,
        const HttpServerOptions& options, const StopToken& stop_token,
        std::size_t max_connections, std::uint32_t buffer_capacity,
        std::atomic<std::size_t>& active_connections,
        std::atomic<std::size_t>& refused_connections);
    ~http3_worker_server();

    http3_worker_server(const http3_worker_server&) = delete;
    http3_worker_server& operator=(const http3_worker_server&) = delete;

    void wake() noexcept;
    [[nodiscard]] http3_stream_buffer& response_buffer() noexcept {
        return response_buffer_;
    }
    [[nodiscard]] bool stage_install(install_link link) noexcept;
    [[nodiscard]] bool install() noexcept;
    [[nodiscard]] Task<void> run();
    void request_stop() noexcept;
    void abandon_before_launch() noexcept;
    [[nodiscard]] bool installed() const noexcept {
        return installed_;
    }
    [[nodiscard]] bool run_started() const noexcept {
        return run_started_;
    }
    [[nodiscard]] bool drained() const noexcept {
        return drained_;
    }

private:
    friend struct http3_worker_server_test_access;

    struct slot final {
        explicit slot(std::pmr::memory_resource* resource)
            : remote_address(resource),
              client_certificate_subject(resource),
              connection(nullptr, PmrObjectDeleter<Http3ServerConnection>{resource}) {}

        http3_connection_state* state{};
        http3_ready_scheduler::registration registration{};
        http3_connection_identity identity{};
        std::pmr::string remote_address;
        std::pmr::string client_certificate_subject;
        std::uint16_t remote_port{};
        std::unique_ptr<Http3ServerConnection, PmrObjectDeleter<Http3ServerConnection>> connection;
        bool retirement_started{};
        bool join_started{};
    };

    [[nodiscard]] bool pump() noexcept;
    [[nodiscard]] bool pump_states() noexcept;
    [[nodiscard]] bool pump_input() noexcept;
    [[nodiscard]] bool pump_scheduler() noexcept;
    [[nodiscard]] bool publish_drain_completions() noexcept;
    [[nodiscard]] slot* find_slot(http3_stream_id id) noexcept;
    [[nodiscard]] slot* find_slot(http3_connection_identity identity) noexcept;
    [[nodiscard]] bool construct_connection(slot& target,
        const http3_connection_state::binding_snapshot& binding) noexcept;
    void begin_retirement(slot& target) noexcept;
    void finish_stopped_slots() noexcept;
    [[nodiscard]] Task<void> join_retired_slot(slot& target);
    void clear_slot(slot& target) noexcept;

    const WorkerHandle& worker_;
    WorkerMemory& memory_;
    const RouteTable& routes_;
    WorkerCapabilities& capabilities_;
    ConnectionScanner& connection_scanner_;
    asio::any_io_executor executor_;
    const HttpServerOptions& options_;
    const StopToken& stop_token_;
    std::atomic<std::size_t>& active_connections_;
    std::atomic<std::size_t>& refused_connections_;
    WorkerSignal signal_;
    http3_stream_buffer response_buffer_;
    std::optional<http3_ready_scheduler> scheduler_;
    Http3ServerBodyBudget body_budget_;
    TaskScope retirement_tasks_;
    std::pmr::vector<slot> slots_;
    struct pending_input final {
        http3_stream_buffer::borrowed_block block;
        std::uint64_t sequence{};
    };
    std::pmr::vector<pending_input> pending_input_;
    std::size_t pending_input_count_{};
    std::uint64_t next_input_sequence_{};
    http3_stream_buffer* request_buffer_{};
    std::size_t max_connections_{};
    std::uint64_t epoch_{1};
    std::uint64_t next_connection_generation_{1};
    bool staged_{};
    bool installed_{};
    bool run_started_{};
    bool stopping_{};
    bool drained_{};
};

}  // namespace ruvia::detail
