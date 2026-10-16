#include <stdexcept>
#include <utility>

#include "redis/redis_registry.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] const worker_handle& require_redis_worker(const worker_handle& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("redis pool requires a valid worker");
    }
    return worker_value;
}

}  // namespace

redis_pool::redis_pool(asio::io_context& io_context, const redis_config_storage& config,
    asio::ssl::context& tls_context, std::optional<std::chrono::milliseconds> command_timeout,
    std::size_t pool_size, const worker_handle& worker_value, std::pmr::memory_resource* resource)
    : config_(config),
      command_timeout_(command_timeout),
      resource_(pmr_resource_or_default(resource)),
      slots_(*this, pool_size, require_redis_worker(worker_value), resource_),
      connections_(resource_) {
    connections_.reserve(pool_size);
    for (std::size_t i = 0; i < pool_size; ++i) {
        connections_.push_back(make_pmr_object<redis_connection>(
            resource_, io_context, tls_context, slots_.worker(), config_, wire_counters_, resource_));
    }
}

redis_pool::~redis_pool() {
    close_now();
}

task<void> redis_pool::connect() {
    for (auto& connection : connections_) {
        if (!connection->connected()) {
            co_await connection->connect(nullptr, command_timeout_);
        }
    }
    co_return;
}

void redis_pool::close_now() noexcept {
    if (!slots_.close()) {
        return;
    }
    for (auto& connection : connections_) {
        connection->abort(client_abort_reason::closing);
    }
}

}  // namespace ruvia::detail
