#pragma once

#include <memory>
#include <string_view>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/web/redis/redis_client.h"

#include "client/client_lifecycle.h"
#include "redis/redis_client_runtime.h"

namespace ruvia::detail {

class redis_client_state final : public std::enable_shared_from_this<redis_client_state> {
public:
    redis_client_state(event_loop loop, const redis_config& config);

    redis_client_state(const redis_client_state&) = delete;
    redis_client_state& operator=(const redis_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] task<void> connect() {
        return lifecycle_.connect();
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] redis_handle handle(operation_options options);

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<redis_client_state>;
    static constexpr std::string_view client_name{"redis"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] redis_client_runtime& backend() noexcept {
        return runtime_;
    }

    event_loop loop_;
    worker_handle worker_;
    worker_memory memory_;
    redis_client_runtime runtime_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<redis_client_state> lifecycle_;
};

}  // namespace ruvia::detail
