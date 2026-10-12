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

// Redis has no ALPN identifier, so the TLS handshake offers none.
[[nodiscard]] client_tcp_endpoint redis_endpoint(const redis_config_storage& config) noexcept {
    return {.host_ = config.host_,
        .port_ = config.port_,
        .tls_ = config.tls_.mode_ == client_tls_mode::disabled ? nullptr : &config.tls_,
        .tcp_ = config.tcp_,
        .alpn_ = client_alpn::none};
}

}  // namespace

void redis_reader_deleter::operator()(redisReader* reader_value) const noexcept {
    if (reader_value != nullptr) {
        redisReaderFree(reader_value);
    }
}

redis_pool::connection_type::connection_type(redis_pool& pool, client_tcp_endpoint endpoint)
    : transport_(pool.io_context_, pool.tls_context_, pool.slots_.worker(), endpoint, pool.wire_counters_),
      write_buffer_(pool.resource_) {}

redis_pool::connection_type::~connection_type() = default;

redis_pool::redis_pool(asio::io_context& io_context, const redis_config_storage& config,
    asio::ssl::context& tls_context, std::optional<std::chrono::milliseconds> command_timeout,
    std::size_t pool_size, const worker_handle& worker_value, std::pmr::memory_resource* resource)
    : io_context_(io_context),
      config_(config),
      tls_context_(tls_context),
      command_timeout_(command_timeout),
      resource_(pmr_resource_or_default(resource)),
      slots_(*this, pool_size, require_redis_worker(worker_value), resource_),
      connections_(resource_) {
    const auto endpoint = redis_endpoint(config_);
    connections_.reserve(pool_size);
    for (std::size_t i = 0; i < pool_size; ++i) {
        connections_.push_back(make_pmr_object<connection_type>(resource_, *this, endpoint));
    }
}

redis_pool::~redis_pool() {
    close_now();
}

task<void> redis_pool::connect() {
    for (std::size_t slot = 0; slot < connections_.size(); ++slot) {
        if (!connections_[slot]->connected_) {
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
        abort(slot, client_abort_reason::closing);
    }
}

}  // namespace ruvia::detail
