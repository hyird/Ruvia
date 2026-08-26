#pragma once

#include <atomic>
#include <memory>
#include <memory_resource>
#include <vector>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/db/DbClient.h"
#include "ruvia/web/detail/db/DbRegistry.h"

namespace ruvia::detail {

class DbClientState final : public std::enable_shared_from_this<DbClientState> {
public:
    DbClientState(EventLoop loop, DbConfig config);
    ~DbClientState();

    DbClientState(const DbClientState&) = delete;
    DbClientState& operator=(const DbClientState&) = delete;

    void bindStop();
    [[nodiscard]] Task<void> connect();
    void requestClose() noexcept;
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
    [[nodiscard]] static std::pmr::vector<DbDefinition> makeDefinitions(
        const DbConfig& config, std::pmr::memory_resource* resource);
    [[nodiscard]] static ConnectionScannerOptions scannerOptions();

    [[nodiscard]] static Task<void> connectOwned(std::shared_ptr<DbClientState> state);
    [[nodiscard]] Task<void> connectOnWorker();
    void requireConnectedOnWorker() const;
    void closeOnWorker() noexcept;

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    std::pmr::vector<DbDefinition> definitions_;
    ConnectionScanner scanner_;
    DbRegistry databases_;
    StopSource stopSource_;
    EventLoopStopRegistration stopRegistration_;
    std::atomic<Phase> phase_{Phase::kFresh};
    // Declared last so handles and cold operations expire before the pool and
    // its unsynchronized worker memory are destroyed.
    ScopedOperationScope operationScope_;
};

}  // namespace ruvia::detail
