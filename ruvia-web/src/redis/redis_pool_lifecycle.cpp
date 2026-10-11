#include <hiredis/hiredis.h>

#include <stdexcept>
#include <utility>

#include "redis/redis_protocol.h"
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

void redis_reader_deleter::operator()(redisReader* reader_value) const noexcept {
    if (reader_value != nullptr) {
        redisReaderFree(reader_value);
    }
}

redis_pool::connection_type::connection_type(asio::io_context& io_context, std::pmr::memory_resource* resource)
    : socket_(io_context),
      resolver_(io_context),
      write_buffer_(pmr_resource_or_default(resource)) {}

redis_pool::connection_type::~connection_type() = default;

redis_pool::connection_type::connection_type(connection_type&&) noexcept = default;
redis_pool::connection_type& redis_pool::connection_type::operator=(connection_type&&) noexcept = default;

redis_pool::redis_pool(asio::io_context& io_context, const redis_config_storage& config,
    asio::ssl::context* tls_context, std::optional<std::chrono::milliseconds> command_timeout,
    std::size_t pool_size, const worker_handle& worker_value, std::pmr::memory_resource* resource)
    : io_context_(io_context),
      config_(config),
      tls_context_(tls_context),
      command_timeout_(command_timeout),
      resource_(pmr_resource_or_default(resource)),
      connections_(resource_),
      slots_(*this, pool_size, require_redis_worker(worker_value), resource_) {
    connections_.reserve(pool_size);
    for (std::size_t i = 0; i < pool_size; ++i) {
        connections_.emplace_back(io_context_, resource_);
    }
}

redis_pool::~redis_pool() {
    close_now();
}

task<void> redis_pool::connect() {
    for (std::size_t slot = 0; slot < connections_.size(); ++slot) {
        if (!connections_[slot].connected_) {
            co_await connect(slot);
        }
    }
    co_return;
}

void redis_pool::close_now() noexcept {
    if (!slots_.close()) {
        return;
    }
    for (std::size_t slot = 0; slot < connections_.size(); ++slot) {
        close(slot);
    }
}

}  // namespace ruvia::detail
