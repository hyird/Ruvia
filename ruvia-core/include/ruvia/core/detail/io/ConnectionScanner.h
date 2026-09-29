#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/worker/WorkerTimer.h"

namespace ruvia {

struct ConnectionScanner::Impl final {
    explicit Impl(ConnectionScanner* owner, WorkerHandle worker, ConnectionScannerOptions options);
    ~Impl() noexcept;

    void start();
    void stop() noexcept;
    void registerWorkerMaintenance(WorkerMaintenanceRegistration& registration, void* target,
        WorkerMaintenanceCheck check) noexcept;
    void registerEntry(Entry& entry, asio::ip::tcp::socket* socket) noexcept;
    void unregisterEntry(Entry& entry) noexcept;
    void closeAll() noexcept;
    void periodicCheckAdded() noexcept;
    void periodicCheckRemoved() noexcept;
    void removeWorkerMaintenance(WorkerMaintenanceRegistration& registration) noexcept;
    void detachWorkerMaintenance() noexcept;
    [[nodiscard]] bool hasScanningWork() const noexcept;
    void schedule();
    void scan() noexcept;
    [[nodiscard]] bool isTimedOut(const Entry& entry, std::int64_t now) const noexcept;

    struct TimerState final {
        explicit TimerState(Impl* owner) noexcept
            : owner(owner) {}
        std::mutex mutex;
        Impl* owner;
    };

    ConnectionScanner* owner_;
    WorkerHandle worker_;
    WorkerTimerRegistration timer_;
    std::shared_ptr<TimerState> timerState_;
    ConnectionScannerOptions options_;
    std::int64_t cachedNowMs_{0};
    Entry sentinel_{};
    WorkerMaintenanceRegistration* workerMaintenance_{nullptr};
    WorkerMaintenanceRegistration* workerMaintenanceScanNext_{nullptr};
    std::size_t periodicCheckCount_{0};
    std::atomic_bool running_{false};
};

}  // namespace ruvia
