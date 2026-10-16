#include "redis/redis_client_runtime.h"

#include <optional>
#include <utility>

#include <asio/ssl/context.hpp>

#include "client/client_tls.h"
#include "redis/redis_registry.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] asio::ssl::context make_redis_tls_context(const client_tls_config_storage& config) {
    asio::ssl::context context_value(asio::ssl::context::tls_client);
    if (config.mode_ != client_tls_mode::disabled) {
        configure_client_tls_context(*context_value.native_handle(), config);
    }
    return context_value;
}

}  // namespace

redis_client_runtime::redis_client_runtime(asio::io_context& io_context, const worker_handle& worker_value,
    redis_config_storage config, std::pmr::memory_resource* resource)
    : config_(std::move(config)),
      resource_(resource),
      worker_(worker_value),
      tls_context_(make_redis_tls_context(config_.tls_)),
      general_(make_pmr_object<redis_pool>(resource_, io_context, config_, tls_context_, config_.command_timeout_,
          config_.pool_size_per_worker_, worker_, resource_)),
      // Blocking waits have their own deadlines; ordinary command timeouts
      // must not interrupt a finite long wait or an explicitly cancelled wait.
      blocking_(make_pmr_object<redis_pool>(resource_, io_context, config_, tls_context_, std::nullopt,
          config_.blocking_pool_size_per_worker_, worker_, resource_)),
      subscriptions_(io_context, tls_context_, config_, worker_, resource_) {}

redis_client_runtime::~redis_client_runtime() = default;

task<void> redis_client_runtime::connect() {
    // Validate ordinary connections at startup; blocking slots and
    // subscriptions connect lazily.
    co_await general_->connect();
}

void redis_client_runtime::close_now() noexcept {
    general_->close_now();
    blocking_->close_now();
    subscriptions_.close_now();
}

redis_handle redis_client_runtime::handle(::ruvia::operation_scope& scope) {
    return redis_handle(*this, resource_, scope);
}

redis_handle redis_client_runtime::handle(::ruvia::operation_scope& scope, operation_options options) {
    validate_operation_options(options);
    return redis_handle(*this, resource_, scope, std::move(options));
}

}  // namespace ruvia::detail
