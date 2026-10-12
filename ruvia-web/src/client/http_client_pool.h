#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_client.h"
#include "ruvia/web/http_client_handle.h"

#include "client/client_request_policy.h"
#include "client/client_tcp_transport.h"
#include "client/http_client_config_storage.h"
#include "client/http_client_request_storage.h"
#include "client/http_client_response_memory.h"
#include "client/http_client_result_budget.h"
#include "integration/slot_pool.h"

namespace ruvia::detail {

class http_client_connection;

class http_client_pool;
using http_client_cancellation_target = worker_cancellation_target<http_client_pool>;

// Request admission over the pool's fixed lease slots, with HTTP client
// errors for every non-acquired outcome. Slot deadlines and slot cancellation
// are unused: an active connection owns its operation deadline and cancellation.
struct http_client_slot_policy final {
    using owner_type = http_client_pool;
    enum class deadline_kind_type : std::uint8_t { request };
    [[noreturn]] static void throw_acquire_failure(const http_client_pool& pool, const pool_waiter_result& result);
    [[noreturn]] static void throw_aborted(const http_client_pool& pool, pool_slot_abort_reason reason);
    static void expire_slot(http_client_pool&, std::size_t, deadline_kind_type) noexcept {}
    static void cancel_slot(http_client_pool&, std::size_t) noexcept {}
};

// One origin client on one worker. Fixed TCP slots exclusively lease each
// connection to one HTTP/1 exchange, with shared cookie policy and statistics.
class http_client_pool final {
public:
    // `resource` is the owner's worker resource for all pool storage;
    // `response_storage` states whether it also outlives every response.
    http_client_pool(asio::io_context& io_context, const worker_handle& worker_value,
        http_client_config_storage config, std::shared_ptr<http_client_result_budget_domain> result_budget_domain,
        std::pmr::memory_resource* resource, http_client_response_storage response_storage);
    http_client_pool(asio::io_context&, worker_handle&&, http_client_config_storage,
        std::shared_ptr<http_client_result_budget_domain>, std::pmr::memory_resource*,
        http_client_response_storage) = delete;
    ~http_client_pool();
    http_client_pool(const http_client_pool&) = delete;
    http_client_pool& operator=(const http_client_pool&) = delete;

    [[nodiscard]] task<http_client_response> execute(http_client_request_storage request, operation_options options);
    [[nodiscard]] task<http_client_exchange> open_request(
        http_client_request_storage request, http_client_upload_config upload, operation_options options);
    [[nodiscard]] task<http_client_tunnel_result> open_tunnel(
        http_client_request_storage request, http_client_tunnel_config config, operation_options options);
    void close_now() noexcept;
    [[nodiscard]] task<void> join();
    [[nodiscard]] http_client_stats stats() const noexcept;
    [[nodiscard]] std::string_view host() const noexcept {
        return config_.host_;
    }
    [[nodiscard]] std::uint16_t port() const noexcept {
        return config_.port_;
    }
    [[nodiscard]] http_scheme scheme() const noexcept {
        return config_.scheme_;
    }

    // Response-state operations of the public response and tunnel owners.
    void abandon_response(http_client_response_state& state_value) noexcept;
    [[nodiscard]] http_client_response_memory_domain& response_memory() noexcept {
        return *response_memory_;
    }
    // worker_cancellation_target hook for the active TCP operation.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;

    // Origin services shared by the pool's connections and exchange drivers.
    [[nodiscard]] asio::io_context& io_context() noexcept {
        return io_context_;
    }
    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }
    [[nodiscard]] const http_client_config_storage& config() const noexcept {
        return config_;
    }
    [[nodiscard]] const http_origin_view& origin() const noexcept {
        return origin_;
    }
    [[nodiscard]] client_request_policy& policy() noexcept {
        return policy_;
    }
    [[nodiscard]] const std::shared_ptr<http_client_cancellation_target>& cancellation_target() const noexcept {
        return cancellation_target_;
    }
    [[nodiscard]] asio::ssl::context& tls_context() noexcept {
        return tls_context_;
    }
    [[nodiscard]] client_wire_counters& wire_counters() noexcept {
        return wire_counters_;
    }

private:
    using connection_owner_type = std::unique_ptr<http_client_connection, pmr_object_deleter<http_client_connection>>;
    // Rejects new requests with closing once join() has started.
    void require_accepting() const;
    [[nodiscard]] task<void> execute_into(
        http_client_request_storage request, operation_options options, http_client_response_state* state);
    [[nodiscard]] task<void> execute_request_into(
        http_client_request_storage request, operation_options options, http_client_response_state* state);
    [[nodiscard]] task<void> execute_tcp(http_client_connection& connection, const http_client_request_storage& request,
        const operation_timeout& timeout, stop_token stop_token_value,
        http_client_response_state& state_value, bool& discard_connection);
    // Spawns the request driver for `state_value`, which the caller's response owns.
    void spawn_request(http_client_request_storage request, operation_options options,
        http_client_response_state& state_value);
    // Waits for the final head and, for encoded bodies, the decoded body.
    [[nodiscard]] static task<void> await_head(http_client_response_state& state_value, std::string_view failure_message);
    [[nodiscard]] http_client_connection& connection(std::size_t index) noexcept {
        return *connections_[index];
    }

    asio::io_context& io_context_;
    const worker_handle& worker_;
    std::pmr::memory_resource* resource_;
    http_client_config_storage config_;
    // Borrows config_.uri_host_; the pool is address-stable and never moved.
    http_origin_view origin_;
    client_request_policy policy_;
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain_;
    http_client_response_memory_domain::owner response_memory_{};
    client_wire_counters wire_counters_;
    asio::ssl::context tls_context_;
    slot_pool<http_client_slot_policy> slots_;
    std::shared_ptr<http_client_cancellation_target> cancellation_target_;
    task_scope background_tasks_;
    std::pmr::vector<connection_owner_type> connections_;
    std::size_t requests_buffered_{0};
    std::size_t requests_in_flight_{0};
    std::size_t completed_requests_{0};
    std::size_t failed_requests_{0};
    bool background_joined_{false};
};

}  // namespace ruvia::detail
