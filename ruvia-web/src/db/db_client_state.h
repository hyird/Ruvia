#pragma once

#include <memory>
#include <string_view>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/web/db/db_client.h"

#include "client/client_lifecycle.h"
#include "db/db_registry.h"

namespace ruvia::detail {

class db_client_state final : public std::enable_shared_from_this<db_client_state> {
public:
    db_client_state(event_loop loop, const db_config& config);
    db_client_state(event_loop loop, const db_config& config, const redis_handle& cache_store,
        const db_cache_config& cache_policy);

    db_client_state(const db_client_state&) = delete;
    db_client_state& operator=(const db_client_state&) = delete;

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
    [[nodiscard]] db_handle handle(operation_options options);

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<db_client_state>;
    static constexpr std::string_view client_name{"database"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] db_registry& backend() noexcept {
        return databases_;
    }

    event_loop loop_;
    worker_handle worker_;
    worker_memory memory_;
    db_registry databases_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<db_client_state> lifecycle_;
};

}  // namespace ruvia::detail
