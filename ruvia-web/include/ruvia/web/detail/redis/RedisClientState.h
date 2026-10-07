#pragma once

#include <memory>
#include <string_view>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/detail/client/client_lifecycle.h"
#include "ruvia/web/detail/redis/RedisClientRuntime.h"
#include "ruvia/web/redis/RedisClient.h"

namespace ruvia::detail {

class RedisClientState final : public std::enable_shared_from_this<RedisClientState> {
public:
    RedisClientState(EventLoop loop, const RedisConfig& config);

    RedisClientState(const RedisClientState&) = delete;
    RedisClientState& operator=(const RedisClientState&) = delete;

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
    [[nodiscard]] RedisHandle handle(OperationOptions options);

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<RedisClientState>;
    static constexpr std::string_view client_name{"redis"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] RedisClientRuntime& backend() noexcept {
        return runtime_;
    }

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    RedisClientRuntime runtime_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<RedisClientState> lifecycle_;
};

}  // namespace ruvia::detail
