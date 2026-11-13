#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

#include <asio/fwd.hpp>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/web/detail/integration/worker_state.h"

#include "client/http_client_registry.h"
#include "client/http_client_worker_limits.h"
#include "context/context_services.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db/db_registry.h"
#endif
#include "integration/worker_client_registry_view.h"
#include "ratelimit/rate_limiter.h"
#ifdef RUVIA_ENABLE_REDIS
#include "redis/redis_registry.h"
#endif
namespace ruvia {
class env;
}  // namespace ruvia

namespace ruvia::detail {

class context_services;
struct connection_failure_sink;

struct worker_capability_definitions final {
#ifdef RUVIA_ENABLE_DATABASE
    std::span<const db_definition> databases_{};
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::span<const redis_definition_type> redis_{};
#endif
    std::span<const worker_state_definition> worker_states_{};
    std::span<const http_client_definition_type> http_clients_{};
};

struct worker_capability_options final {
    std::optional<rate_limit_rule> default_rate_limit_{};
    route_rate_limit_presence route_rate_limits_{route_rate_limit_presence::absent};
    std::size_t rate_limit_capacity_{default_rate_limit_capacity_per_worker};
    std::size_t max_decoded_body_bytes_{default_max_buffered_body_bytes};
    http_client_worker_config http_client_worker_{};
    const env* env_{nullptr};
    const http_client_ip_extractor* trusted_proxies_{nullptr};
    bool precompressed_static_files_{false};
    // The server write_timeout_ bounding a file response's open on the pool.
    std::optional<timeout_duration> file_io_timeout_{};
    // The worker's connection failure outlet; required.
    const connection_failure_sink* connection_failure_{nullptr};
};

// Owns every application capability attached to one worker. Construction,
// startup, request exposure, and shutdown all follow this single ownership
// boundary; none of these registries is process-global or shared by workers.
// start()/stop() are the one worker bring-up and retirement chain, shared by
// the server worker and test_app. Both run on the owning worker.
class worker_capabilities final {
public:
    worker_capabilities(asio::io_context& io_context, const worker_ref& worker_value,
        worker_capability_definitions definitions,
        worker_capability_options options);
    worker_capabilities(asio::io_context&, worker_ref&&,
        worker_capability_definitions, worker_capability_options) = delete;

    worker_capabilities(const worker_capabilities&) = delete;
    worker_capabilities& operator=(const worker_capabilities&) = delete;

    // Creates the per-worker state, then connects remote capabilities. A
    // failure retires everything this call started before rethrowing.
    [[nodiscard]] task<void> start();
    // Interrupts capability I/O; idempotent. Retirement completes in stop().
    void close_now() noexcept;
    // Closes capability I/O, joins in-flight client work, then destroys the
    // per-worker state. Safe after a failed or skipped start().
    [[nodiscard]] task<void> stop();

    [[nodiscard]] context_services make_context_services(const stop_token& stop_token);
    context_services make_context_services(stop_token&&) = delete;
    [[nodiscard]] worker_client_registry_view client_registries() noexcept;
    [[nodiscard]] const worker_state_registry& worker_states() const noexcept;

    // Owner thread, together with adoption of the first routed table.
    void adopt_route_rate_limiter(rate_limiter& prepared) noexcept {
        rate_limiter_.adopt_route_storage(prepared);
    }

private:
    const worker_ref& worker_;
#ifdef RUVIA_ENABLE_REDIS
    redis_registry redis_;
#endif
#ifdef RUVIA_ENABLE_DATABASE
    db_registry databases_;
#endif
    http_client_worker_services http_client_services_;
    http_client_registry http_clients_;
    worker_state_registry worker_states_;
    rate_limiter rate_limiter_;
    worker_capability_options options_;
    context_worker_services worker_services_;
};

}  // namespace ruvia::detail
