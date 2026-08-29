#pragma once

#include <atomic>
#include <exception>
#include <memory>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/db/DbClient.h"
#include "ruvia/web/detail/db/DbRegistry.h"

namespace ruvia::detail {

class DbClientState final : public std::enable_shared_from_this<DbClientState> {
public:
    DbClientState(EventLoop loop, const DbConfig& config);
    ~DbClientState();

    DbClientState(const DbClientState&) = delete;
    DbClientState& operator=(const DbClientState&) = delete;

    void bindStop();
    [[nodiscard]] Task<void> connect();
    void requestClose() noexcept;
    [[nodiscard]] Task<void> shutdown();
    [[nodiscard]] DbHandle handle(OperationOptions options);

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

    [[nodiscard]] static Task<void> connectOwned(std::shared_ptr<DbClientState> state);
    [[nodiscard]] static Task<void> shutdownOwned(std::shared_ptr<DbClientState> state);
    [[nodiscard]] Task<void> connectOnWorker();
    [[nodiscard]] Task<void> closeOnWorker();
    void requireConnectedOnWorker() const;
    void startCloseOnWorker() noexcept;
    void finishClose(const TaskCompletionResult<void>& result);

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    DbRegistry databases_;
    StopSource stopSource_;
    EventLoopStopRegistration stopRegistration_;
    WorkerSignal closeSignal_;
    std::atomic<Phase> phase_{Phase::kFresh};
    bool connectInFlight_{false};
    bool closeTaskStarted_{false};
    bool closeComplete_{false};
    std::exception_ptr closeFailure_;
    // Declared last so handles and cold operations expire before the pool and
    // its unsynchronized worker memory are destroyed.
    ScopedOperationScope operationScope_;
};

}  // namespace ruvia::detail
