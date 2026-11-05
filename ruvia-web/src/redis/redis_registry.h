#pragma once

#include "ruvia/core/worker_handle.h"
#include "ruvia/web/redis/redis_handle.h"

#include "integration/named_capability.h"
#include "redis/redis_config_storage.h"

#ifndef RUVIA_ENABLE_REDIS

#include <memory_resource>
#include <span>
#include <stdexcept>

#include <asio/io_context.hpp>

namespace ruvia::detail {

class redis_registry final {
public:
    redis_registry(asio::io_context&, std::pmr::memory_resource*, std::span<const redis_definition_type>,
        worker_handle worker_value) {
        if (!worker_value.valid()) {
            throw std::invalid_argument("redis registry requires a valid worker");
        }
    }

    redis_registry(const redis_registry&) = delete;
    redis_registry& operator=(const redis_registry&) = delete;

    [[nodiscard]] task<void> connect() {
        co_return;
    }

    void close_now() noexcept {}
    [[nodiscard]] bool empty() const noexcept {
        return true;
    }
};

}  // namespace ruvia::detail

#else

#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>

#include <asio/io_context.hpp>

#include "ruvia/core/memory/pmr_object.h"

#include "redis/redis_client_runtime.h"

namespace ruvia::detail {

class redis_registry final {
public:
    redis_registry(asio::io_context& io_context, std::pmr::memory_resource* resource,
        std::span<const redis_definition_type> redis, worker_handle worker_value);
    ~redis_registry();

    redis_registry(const redis_registry&) = delete;
    redis_registry& operator=(const redis_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] redis_handle get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;
    [[nodiscard]] redis_handle get(
        std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

private:
    using entry_type = std::unique_ptr<redis_client_runtime, pmr_object_deleter<redis_client_runtime>>;

    worker_handle worker_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<entry_type> runtimes_;
    named_capability_index alias_index_;
};

}  // namespace ruvia::detail

#endif
