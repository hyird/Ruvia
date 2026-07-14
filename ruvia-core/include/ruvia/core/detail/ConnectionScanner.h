#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <asio/ip/tcp.hpp>

#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/detail/WorkerTimer.h>

namespace ruvia::detail {

struct ConnectionScannerOptions final {
    std::chrono::milliseconds scanInterval{std::chrono::seconds(1)};
    // Inactivity timeouts measured from the connection's last successful I/O.
    // Protocol runtimes map their own lifecycle states onto these generic phases.
    // Absence disables the corresponding phase timeout.
    std::optional<std::chrono::milliseconds> idleTimeout;
    std::optional<std::chrono::milliseconds> initialReadTimeout;
    std::optional<std::chrono::milliseconds> payloadReadTimeout;
    std::optional<std::chrono::milliseconds> writeTimeout;
};

class ConnectionScanner final {
public:
    using PeriodicCheck = bool (*)(void*, std::int64_t) noexcept;
    using WorkerMaintenanceCheck = void (*)(void*) noexcept;

    enum class Phase {
        kIdle,
        kReadingInitial,
        kReadingPayload,
        kLongLived,
        kWriting
    };

    class Entry;

    // Startup-owned worker maintenance hooks (for example product resource
    // deadline scans) use caller-owned intrusive nodes. This keeps the core
    // generic, allocation-free, and free of a fixed registration ceiling.
    class WorkerMaintenanceRegistration final {
    public:
        WorkerMaintenanceRegistration() noexcept = default;
        ~WorkerMaintenanceRegistration() noexcept;

        WorkerMaintenanceRegistration(
            const WorkerMaintenanceRegistration&) = delete;
        WorkerMaintenanceRegistration& operator=(
            const WorkerMaintenanceRegistration&) = delete;
        WorkerMaintenanceRegistration(
            WorkerMaintenanceRegistration&&) = delete;
        WorkerMaintenanceRegistration& operator=(
            WorkerMaintenanceRegistration&&) = delete;

        void reset() noexcept;

    private:
        friend class ConnectionScanner;

        ConnectionScanner* scanner_{nullptr};
        WorkerMaintenanceRegistration* prev_{nullptr};
        WorkerMaintenanceRegistration* next_{nullptr};
        void* target_{nullptr};
        WorkerMaintenanceCheck check_{nullptr};
    };

    // Intrusive registration: the checked object owns the node, so one
    // multiplexed connection can carry any number of stream-local checks
    // without a scanner allocation or an arbitrary fixed slot limit.
    class PeriodicCheckRegistration final {
    public:
        PeriodicCheckRegistration() noexcept = default;
        ~PeriodicCheckRegistration() noexcept;

        PeriodicCheckRegistration(const PeriodicCheckRegistration&) = delete;
        PeriodicCheckRegistration& operator=(
            const PeriodicCheckRegistration&) = delete;
        PeriodicCheckRegistration(PeriodicCheckRegistration&&) = delete;
        PeriodicCheckRegistration& operator=(
            PeriodicCheckRegistration&&) = delete;

        void reset() noexcept;

    private:
        friend class ConnectionScanner;
        friend class Entry;

        Entry* entry_{nullptr};
        PeriodicCheckRegistration* prev_{nullptr};
        PeriodicCheckRegistration* next_{nullptr};
        void* target_{nullptr};
        PeriodicCheck tick_{nullptr};
    };

    class Entry final {
    public:
        Entry() noexcept = default;
        ~Entry() noexcept;

        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&&) = delete;
        Entry& operator=(Entry&&) = delete;

        void touch() noexcept;
        void setPhase(Phase nextPhase) noexcept;
        [[nodiscard]] std::int64_t lastActiveMs() const noexcept;
        void registerPeriodicCheck(
            PeriodicCheckRegistration& registration,
            void* target,
            PeriodicCheck tick) noexcept;

    private:
        friend class ConnectionScanner;
        friend class PeriodicCheckRegistration;

        [[nodiscard]] bool linked() const noexcept;
        [[nodiscard]] bool tickLongLived(std::int64_t now) noexcept;
        void removePeriodicCheck(
            PeriodicCheckRegistration& registration) noexcept;
        void detachPeriodicChecks() noexcept;

        asio::ip::tcp::socket* socket_{nullptr};
        ConnectionScanner* scanner_{nullptr};
        Entry* prev_{nullptr};
        Entry* next_{nullptr};
        // Coarse timestamp source owned by the scanner; refreshed once per scan
        // tick so per-request touch()/setPhase() never hit the system clock.
        const std::int64_t* nowMs_{nullptr};
        std::int64_t lastActiveMs_{0};
        Phase phase_{Phase::kIdle};
        PeriodicCheckRegistration* periodicChecks_{nullptr};
        // The next node to visit while tickLongLived() is active. Unlinking a
        // different registration from inside a callback advances this cursor,
        // so RAII teardown cannot leave iteration pointing at freed storage.
        PeriodicCheckRegistration* periodicScanNext_{nullptr};
    };

    class Guard final {
    public:
        Guard(ConnectionScanner* scanner, Entry& entry, asio::ip::tcp::socket& socket);
        ~Guard();

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Entry* entry_;
    };

    ConnectionScanner(WorkerHandle worker, ConnectionScannerOptions options);
    ~ConnectionScanner() noexcept;

    void start();
    void stop() noexcept;
    void registerWorkerMaintenance(
        WorkerMaintenanceRegistration& registration,
        void* target,
        WorkerMaintenanceCheck check) noexcept;
    void registerEntry(Entry& entry, asio::ip::tcp::socket& socket) noexcept;
    void unregisterEntry(Entry& entry) noexcept;
    void closeAll() noexcept;

private:
    static void detachEntry(Entry& entry) noexcept;
    void detachAllEntries() noexcept;
    void periodicCheckAdded() noexcept;
    void periodicCheckRemoved() noexcept;
    void removeWorkerMaintenance(
        WorkerMaintenanceRegistration& registration) noexcept;
    void detachWorkerMaintenance() noexcept;
    [[nodiscard]] bool hasScanningWork() const noexcept;
    void schedule();
    void scan() noexcept;
    [[nodiscard]] bool isTimedOut(const Entry& entry, std::int64_t now) const noexcept;

    WorkerHandle worker_;
    WorkerTimerRegistration timer_;
    ConnectionScannerOptions options_;
    std::int64_t cachedNowMs_{0};
    Entry sentinel_{};
    WorkerMaintenanceRegistration* workerMaintenance_{nullptr};
    WorkerMaintenanceRegistration* workerMaintenanceScanNext_{nullptr};
    std::size_t periodicCheckCount_{0};
    bool running_{false};
};

}  // namespace ruvia::detail
