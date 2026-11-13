#pragma once

#ifdef RUVIA_ENABLE_REDIS

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/worker_ref.h"
#include "ruvia/web/redis/redis_session.h"

#include "integration/named_capability.h"
#include "redis/redis_client_runtime.h"
#include "redis/redis_config_storage.h"

namespace ruvia::detail {

class redis_registry final {
public:
    redis_registry(asio::io_context& io_context,
        std::span<const redis_definition_type> redis, worker_ref worker_value);
    ~redis_registry();

    redis_registry(const redis_registry&) = delete;
    redis_registry& operator=(const redis_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] redis_session get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;
    [[nodiscard]] redis_session get(
        std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

private:
    using entry_type = std::unique_ptr<redis_client_runtime>;

    std::vector<entry_type> runtimes_;
    named_capability_index alias_index_;
};

}  // namespace ruvia::detail

#endif
