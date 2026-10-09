#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/web/detail/integration/worker_state.h"

#include "client/http_client_registry.h"
#include "client/http_client_result_budget.h"
#include "db/db_registry.h"
#include "integration/worker_client_registry_view.h"
#include "ratelimit/rate_limiter.h"
#include "redis/redis_registry.h"

namespace asio {
class io_context;
}

namespace ruvia {
class blocking_pool;
class env;
}  // namespace ruvia

namespace ruvia::detail {

class context_services;
class trusted_proxy_set;

struct worker_capability_definitions final {
    std::span<const db_definition> databases_{};
    std::span<const redis_definition_type> redis_{};
    std::span<const worker_state_definition> worker_states_{};
    std::span<const http_client_definition_type> http_clients_{};
};

struct worker_capability_options final {
    std::optional<rate_limit_rule> default_rate_limit_{};
    route_rate_limit_presence route_rate_limits_{route_rate_limit_presence::absent};
    std::size_t rate_limit_capacity_{default_rate_limit_capacity_per_worker};
    std::size_t max_decoded_body_bytes_{default_max_buffered_body_bytes};
    http_client_result_budget_config http_client_result_budget_{};
    blocking_pool* blocking_pool_{nullptr};
    const env* env_{nullptr};
    const trusted_proxy_set* trusted_proxies_{nullptr};
    bool precompressed_static_files_{false};
};

// Owns every application capability attached to one worker. Construction,
// startup, request exposure, and shutdown all follow this single ownership
// boundary; none of these registries is process-global or shared by workers.
class worker_capabilities final {
public:
    worker_capabilities(asio::io_context& io_context, const worker_handle& worker_value,
        std::pmr::memory_resource* resource, worker_capability_definitions definitions,
        worker_capability_options options);
    worker_capabilities(asio::io_context&, worker_handle&&, std::pmr::memory_resource*,
        worker_capability_definitions, worker_capability_options) = delete;

    worker_capabilities(const worker_capabilities&) = delete;
    worker_capabilities& operator=(const worker_capabilities&) = delete;

    [[nodiscard]] task<void> connect();
    [[nodiscard]] task<void> join();
    void close_now() noexcept;
    void initialize_worker_state();
    void shutdown_worker_state() noexcept;

    [[nodiscard]] context_services make_context_services(const stop_token& stop_token);
    context_services make_context_services(stop_token&&) = delete;
    [[nodiscard]] worker_client_registry_view client_registries() noexcept;
    [[nodiscard]] const worker_state_registry& worker_states() const noexcept;
    [[nodiscard]] rate_limiter_type& rate_limiter() noexcept;
    [[nodiscard]] blocking_pool* get_blocking_pool() const noexcept;

private:
    const worker_handle& worker_;
    redis_registry redis_;
    db_registry databases_;
    std::shared_ptr<http_client_result_budget_domain> http_client_result_budget_domain_;
    http_client_registry http_clients_;
    worker_state_registry worker_states_;
    rate_limiter_type rate_limiter_;
    worker_capability_options options_;
};

}  // namespace ruvia::detail
