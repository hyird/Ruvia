#include <hiredis.h>

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <openssl/ssl.h>

#include "client/client_transport.h"
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
      write_buffer_(detail::pmr_resource_or_default(resource)),
      reader_(redisReaderCreate()),
      deadline_timer_(make_pmr_object<worker_timer_registration>(resource)) {}

redis_pool::connection_type::~connection_type() = default;

redis_pool::connection_type::connection_type(connection_type&&) noexcept = default;
redis_pool::connection_type& redis_pool::connection_type::operator=(connection_type&&) noexcept = default;

redis_pool::redis_pool(asio::io_context& io_context, const redis_config_storage& config,
    std::optional<std::chrono::milliseconds> command_timeout, std::size_t pool_size,
    const worker_handle& worker_value, std::pmr::memory_resource* resource)
    : io_context_(io_context),
      worker_(require_redis_worker(worker_value)),
      config_(config),
      command_timeout_(command_timeout),
      resource_(detail::pmr_resource_or_default(resource)),
      connections_(resource_),
      scheduler_(pool_size, worker_, resource_),
      cancellation_target_(make_worker_cancellation_target(*this, worker_)) {
    if (config_.tls_.mode_ == client_tls_mode::verify_identity) {
        tls_context_.emplace(asio::ssl::context::tls_client);
        configure_client_tls_context(*tls_context_->native_handle(),
            {.ca_file_ = config_.tls_.ca_file_,
                .certificate_chain_file_ = config_.tls_.certificate_file_,
                .private_key_file_ = config_.tls_.private_key_file_});
    }
    connections_.reserve(pool_size);
    for (std::size_t i = 0; i < pool_size; ++i) {
        connections_.emplace_back(io_context_, resource_);
    }
}

redis_pool::~redis_pool() {
    close_now();
}

task<void> redis_pool::connect() {
    for (auto& connection : connections_) {
        if (!connection.connected_) {
            co_await connect(connection);
        }
    }
    co_return;
}

void redis_pool::close_now() noexcept {
    cancellation_target_->detach(*this);
    if (!scheduler_.close()) {
        return;
    }
    for (auto& connection : connections_) {
        if (connection.abort_reason_ == connection_type::abort_reason_type::none) {
            connection.abort_reason_ = connection_type::abort_reason_type::closing;
        }
        close(connection);
    }
}

}  // namespace ruvia::detail
