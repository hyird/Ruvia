#include "redis/redis_client_runtime.h"

#include <utility>

#include "redis/redis_registry.h"

namespace ruvia::detail {

redis_client_runtime::redis_client_runtime(asio::io_context& io_context, const worker_handle& worker_value,
    redis_config_storage config, std::pmr::memory_resource* resource)
    : config_(std::move(config)),
      resource_(resource),
      general_(make_pmr_object<redis_pool>(resource_, io_context, config_,
          config_.command_timeout_, config_.pool_size_per_worker_, worker_value, resource_)),
      // Blocking waits have their own deadlines; ordinary command timeouts
      // must not interrupt a finite long wait or an explicitly cancelled wait.
      blocking_(make_pmr_object<redis_pool>(resource_, io_context, config_,
          std::nullopt, config_.blocking_pool_size_per_worker_, worker_value, resource_)) {}

redis_client_runtime::~redis_client_runtime() = default;

task<void> redis_client_runtime::connect() {
    // Validate ordinary connections at startup; blocking slots connect lazily.
    co_await general_->connect();
}

void redis_client_runtime::close_now() noexcept {
    general_->close_now();
    blocking_->close_now();
}

redis_handle redis_client_runtime::handle(::ruvia::operation_scope& scope) const {
    return redis_handle(*general_, *blocking_, resource_, scope);
}

redis_handle redis_client_runtime::handle(::ruvia::operation_scope& scope, operation_options options) const {
    validate_operation_options(options);
    return redis_handle(*general_, *blocking_, resource_, scope, std::move(options));
}

}  // namespace ruvia::detail
