#include <exception>

#include "redis/redis_registry.h"

namespace ruvia::detail {

redis_pool::connection_guard_type::connection_guard_type(
    redis_pool& pool, std::size_t index, const stop_token& stop_token_value)
    : pool_(pool),
      index_(index),
      cancellation_(pool.cancellation_target_, pool.connections_[index].cancellation_id_) {
    auto& connection = pool_.connections_[index_];
    connection.abort_reason_ = connection_type::abort_reason_type::none;
    cancellation_.arm(stop_token_value);
}

redis_pool::connection_guard_type::~connection_guard_type() {
    cancellation_.reset();
    auto& connection = pool_.connections_[index_];
    if (discard_) {
        pool_.close(connection);
    }
    pool_.release(index_);
}

redis_pool::connection_type& redis_pool::connection_guard_type::connection() noexcept {
    return pool_.connections_[index_];
}

void redis_pool::connection_guard_type::discard() noexcept {
    discard_ = true;
}

task<std::size_t> redis_pool::acquire(const ruvia::operation_timeout& timeout, stop_token stop_token_value) {
    const auto result_value = co_await scheduler_.acquire(
        timeout.constrained_by(config_.acquire_timeout_).remaining(), std::move(stop_token_value));
    switch (result_value.status()) {
        case pool_waiter_result::status_type::acquired:
            co_return result_value.index();
        case pool_waiter_result::status_type::timed_out:
            throw redis_error(redis_error::code_type::timeout, "redis connection pool acquire timed out");
        case pool_waiter_result::status_type::cancelled:
            throw redis_error(redis_error::code_type::cancelled, "redis operation cancelled");
        case pool_waiter_result::status_type::closed:
            throw redis_error(redis_error::code_type::closing, "redis pool is closing");
    }
    std::terminate();
}

void redis_pool::release(std::size_t index) noexcept {
    const auto status = scheduler_.release(index);
    if (status == pool_lease_release_status::invalid_slot ||
        status == pool_lease_release_status::already_released) {
        std::terminate();
    }
}

}  // namespace ruvia::detail
