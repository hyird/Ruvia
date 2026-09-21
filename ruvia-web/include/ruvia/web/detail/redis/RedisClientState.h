#pragma once

#include <atomic>
#include <memory>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/detail/client/ClientCloseState.h"
#include "ruvia/web/detail/redis/RedisClientRuntime.h"
#include "ruvia/web/redis/RedisClient.h"

namespace ruvia::detail {

class RedisClientState final : public std::enable_shared_from_this<RedisClientState> {
public:
    RedisClientState(EventLoop loop, const RedisConfig& config);
    ~RedisClientState();

    RedisClientState(const RedisClientState&) = delete;
    RedisClientState& operator=(const RedisClientState&) = delete;

    void bindStop();
    [[nodiscard]] Task<void> connect();
    void requestClose() noexcept;
    [[nodiscard]] Task<void> shutdown();
    [[nodiscard]] RedisHandle handle(OperationOptions options);

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

private:
    enum class Phase : unsigned char {
        kFresh,
        kConnecting,
        kConnected,
        kClosing,
        kClosed,
    };

    [[nodiscard]] static EventLoop requireLoop(EventLoop loop);

    [[nodiscard]] static Task<void> connectOwned(std::shared_ptr<RedisClientState> state);
    [[nodiscard]] static Task<void> shutdownOwned(std::shared_ptr<RedisClientState> state);
    [[nodiscard]] Task<void> connectOnWorker();
    [[nodiscard]] Task<void> closeOnWorker();
    void requireConnectedOnWorker() const;
    void startCloseOnWorker() noexcept;
    void finishClose(const TaskCompletionResult<void>& result);

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    RedisClientRuntime runtime_;
    StopSource stopSource_;
    EventLoopStopRegistration stopRegistration_;
    ClientCloseState closeState_;
    std::atomic<Phase> phase_{Phase::kFresh};
    bool connectInFlight_{false};
    // Declared last so handles and cold operations expire before the pool and
    // its unsynchronized worker memory are destroyed.
    ScopedOperationScope operationScope_;
};

}  // namespace ruvia::detail
