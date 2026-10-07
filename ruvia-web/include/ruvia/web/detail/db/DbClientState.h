#pragma once

#include <memory>
#include <string_view>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/db/DbClient.h"
#include "ruvia/web/detail/client/client_lifecycle.h"
#include "ruvia/web/detail/db/DbRegistry.h"

namespace ruvia::detail {

class DbClientState final : public std::enable_shared_from_this<DbClientState> {
public:
    DbClientState(EventLoop loop, const DbConfig& config);
    DbClientState(EventLoop loop, const DbConfig& config, const RedisHandle& cache_store,
        const DbCacheConfig& cache_policy);

    DbClientState(const DbClientState&) = delete;
    DbClientState& operator=(const DbClientState&) = delete;

    void bindStop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] Task<void> connect() {
        return lifecycle_.connect();
    }
    void requestClose() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] Task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] DbHandle handle(OperationOptions options);

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<DbClientState>;
    static constexpr std::string_view client_name{"database"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] DbRegistry& backend() noexcept {
        return databases_;
    }

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    DbRegistry databases_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<DbClientState> lifecycle_;
};

}  // namespace ruvia::detail
