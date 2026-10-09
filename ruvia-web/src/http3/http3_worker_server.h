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

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"

#include "http3/http3_connection_state.h"
#include "http3/http3_ready_scheduler.h"
#include "http3/http3_server_body_budget.h"
#include "http3/http3_server_connection.h"
#include "http3/http3_stream_buffer.h"

namespace ruvia::detail {

class route_table;
class worker_capabilities;
struct http_server_options;

// Protocol and handlers share one worker. Only the datagram owner crosses a
// thread boundary; local buffer/state readiness is delivered by worker_signal.
class http3_worker_server final {
public:
    struct install_link final {
        http3_stream_buffer* request_buffer_{};
        std::span<http3_connection_state* const> connections_{};
        http3_stream_buffer::local_callback protocol_ready_{};
    };

    http3_worker_server(const worker_handle& worker_value, worker_memory& memory,
        const route_table& routes_value, worker_capabilities& capabilities,
        connection_scanner& connection_scanner_value, asio::any_io_executor executor,
        const http_server_options& options, const stop_token& stop_token_value,
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
    [[nodiscard]] task<void> run();
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
            : remote_address_(resource),
              client_certificate_subject_(resource),
              connection_(nullptr, pmr_object_deleter<http3_server_connection>{resource}) {}

        http3_connection_state* state_{};
        http3_ready_scheduler::registration registration_{};
        http3_connection_identity identity_{};
        std::pmr::string remote_address_;
        std::pmr::string client_certificate_subject_;
        std::uint16_t remote_port_{};
        std::unique_ptr<http3_server_connection, pmr_object_deleter<http3_server_connection>> connection_;
        bool retirement_started_{};
        bool join_started_{};
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
    [[nodiscard]] task<void> join_retired_slot(slot& target);
    void clear_slot(slot& target) noexcept;

    const worker_handle& worker_;
    worker_memory& memory_;
    const route_table& routes_;
    worker_capabilities& capabilities_;
    connection_scanner& connection_scanner_;
    asio::any_io_executor executor_;
    const http_server_options& options_;
    const stop_token& stop_token_;
    std::atomic<std::size_t>& active_connections_;
    std::atomic<std::size_t>& refused_connections_;
    worker_signal signal_;
    http3_stream_buffer response_buffer_;
    std::optional<http3_ready_scheduler> scheduler_;
    http3_server_body_budget body_budget_;
    task_scope retirement_tasks_;
    std::pmr::vector<slot> slots_;
    struct pending_input final {
        http3_stream_buffer::borrowed_block block_;
        std::uint64_t sequence_{};
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
