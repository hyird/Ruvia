#pragma once

#include <memory>
#include <memory_resource>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/redis/redis_handle.h"

#include "redis/redis_config_storage.h"

namespace asio {
class io_context;
}

namespace ruvia::detail {

// The single backend owner used by standalone clients and application registrations.
// Both pools borrow this stable owner's normalized configuration and worker.
class redis_client_runtime final {
public:
    redis_client_runtime(asio::io_context& io_context, const worker_handle& worker_value,
        redis_config_storage config, std::pmr::memory_resource* resource);
    redis_client_runtime(asio::io_context&, worker_handle&&, redis_config_storage,
        std::pmr::memory_resource*) = delete;
    ~redis_client_runtime();

    redis_client_runtime(const redis_client_runtime&) = delete;
    redis_client_runtime& operator=(const redis_client_runtime&) = delete;

    [[nodiscard]] task<void> connect();
    void close_now() noexcept;
    [[nodiscard]] redis_handle handle(::ruvia::operation_scope& scope) const;
    [[nodiscard]] redis_handle handle(::ruvia::operation_scope& scope, operation_options options) const;

private:
    redis_config_storage config_;
    std::pmr::memory_resource* resource_;
    std::unique_ptr<redis_pool, pmr_object_deleter<redis_pool>> general_;
    std::unique_ptr<redis_pool, pmr_object_deleter<redis_pool>> blocking_;
};

}  // namespace ruvia::detail
