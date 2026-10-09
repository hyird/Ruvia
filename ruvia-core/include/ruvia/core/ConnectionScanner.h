#pragma once

#include <cstdint>
#include <memory>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/ConnectionScannerOptions.h"
#include "ruvia/core/WorkerHandle.h"

namespace ruvia {

// Worker-affine connection timeout and maintenance owner. Its implementation
// is allocated once per scanner; entries and registrations remain caller-owned
// intrusive nodes, with no per-connection allocation by the scanner.
class ConnectionScanner final {
private:
    struct Impl;

public:
    // Product-owned checks operate on their own transport. A check belonging
    // to one multiplexed stream must not close its connection's socket.
    using PeriodicCheck = void (*)(void*, std::int64_t) noexcept;
    using WorkerMaintenanceCheck = void (*)(void*) noexcept;

    enum class Phase : std::uint8_t {
        kIdle,
        kReadingInitial,
        kReadingPayload,
        kLongLived,
        kWriting,
    };

    class Entry;

    // Startup-owned worker maintenance node; the caller keeps it alive while
    // registered. reset() is safe after its scanner has been destroyed.
    class WorkerMaintenanceRegistration final {
    public:
        WorkerMaintenanceRegistration() noexcept = default;
        ~WorkerMaintenanceRegistration() noexcept;
        WorkerMaintenanceRegistration(const WorkerMaintenanceRegistration&) = delete;
        WorkerMaintenanceRegistration& operator=(const WorkerMaintenanceRegistration&) = delete;
        WorkerMaintenanceRegistration(WorkerMaintenanceRegistration&&) = delete;
        WorkerMaintenanceRegistration& operator=(WorkerMaintenanceRegistration&&) = delete;
        void reset() noexcept;

    private:
        friend class ConnectionScanner;
        friend struct Impl;
        ConnectionScanner* scanner_{nullptr};
        WorkerMaintenanceRegistration* prev_{nullptr};
        WorkerMaintenanceRegistration* next_{nullptr};
        void* target_{nullptr};
        WorkerMaintenanceCheck check_{nullptr};
    };

    // Per-connection or per-stream node; callback registration is allocation-free.
    // The entry detaches any remaining nodes when it is destroyed.
    class PeriodicCheckRegistration final {
    public:
        PeriodicCheckRegistration() noexcept = default;
        ~PeriodicCheckRegistration() noexcept;
        PeriodicCheckRegistration(const PeriodicCheckRegistration&) = delete;
        PeriodicCheckRegistration& operator=(const PeriodicCheckRegistration&) = delete;
        PeriodicCheckRegistration(PeriodicCheckRegistration&&) = delete;
        PeriodicCheckRegistration& operator=(PeriodicCheckRegistration&&) = delete;
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

    // Connection- or stream-owned node. touch()/setPhase() use the scanner's
    // cached coarse time, not a per-request clock read. Destruction detaches the
    // entry and its checks, including during a periodic callback. Destroying the
    // scanner detaches its entries before releasing its timestamp storage.
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
            PeriodicCheckRegistration& registration, void* target, PeriodicCheck tick) noexcept;

    private:
        friend class ConnectionScanner;
        friend class PeriodicCheckRegistration;
        friend struct Impl;
        [[nodiscard]] bool linked() const noexcept;
        void removePeriodicCheck(PeriodicCheckRegistration& registration) noexcept;
        void detachPeriodicChecks() noexcept;
        asio::ip::tcp::socket* socket_{nullptr};
        ConnectionScanner* scanner_{nullptr};
        Entry* prev_{nullptr};
        Entry* next_{nullptr};
        const std::int64_t* nowMs_{nullptr};
        std::int64_t lastActiveMs_{0};
        std::int64_t phase_started_ms_{0};
        Phase phase_{Phase::kIdle};
        PeriodicCheckRegistration* periodicChecks_{nullptr};
        PeriodicCheckRegistration* periodicScanNext_{nullptr};
    };

    // Binds a TCP socket to an entry for the guard lifetime; its destructor
    // safely detaches even if the scanner was destroyed first.
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
    ConnectionScanner(const ConnectionScanner&) = delete;
    ConnectionScanner& operator=(const ConnectionScanner&) = delete;

    [[nodiscard]] const WorkerHandle& worker() const& noexcept;
    const WorkerHandle& worker() const&& = delete;
    void start();
    void stop() noexcept;
    void registerWorkerMaintenance(WorkerMaintenanceRegistration& registration, void* target,
        WorkerMaintenanceCheck check) noexcept;
    void registerEntry(Entry& entry, asio::ip::tcp::socket& socket) noexcept;
    // Multiplexed stream checks have no socket: scanner timeouts must not
    // close an unrelated connection on their behalf.
    void registerEntry(Entry& entry) noexcept;
    void unregisterEntry(Entry& entry) noexcept;
    void closeAll() noexcept;

private:
    friend class Entry;
    friend class WorkerMaintenanceRegistration;
    friend class Guard;
    friend struct Impl;
    static void detachEntry(Entry& entry) noexcept;
    void removeWorkerMaintenance(WorkerMaintenanceRegistration& registration) noexcept;
    void periodicCheckAdded() noexcept;
    void periodicCheckRemoved() noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia
