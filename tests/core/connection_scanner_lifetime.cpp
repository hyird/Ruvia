#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"

namespace {

struct PeriodicProbe final {
    std::size_t ticks{0};

    static void tick(void* target, std::int64_t) noexcept {
        ++static_cast<PeriodicProbe*>(target)->ticks;
    }
};

struct PeriodicResetProbe final {
    ruvia::ConnectionScanner::PeriodicCheckRegistration* registration;
    std::size_t ticks{0};

    static void tick(void* target, std::int64_t) noexcept {
        auto& probe = *static_cast<PeriodicResetProbe*>(target);
        ++probe.ticks;
        probe.registration->reset();
    }
};

struct WorkerMaintenanceProbe final {
    std::size_t ticks{0};

    static void check(void* target) noexcept {
        ++static_cast<WorkerMaintenanceProbe*>(target)->ticks;
    }
};

struct WorkerMaintenanceResetProbe final {
    ruvia::ConnectionScanner::WorkerMaintenanceRegistration* registration;
    std::size_t ticks{0};

    static void check(void* target) noexcept {
        auto& probe = *static_cast<WorkerMaintenanceResetProbe*>(target);
        ++probe.ticks;
        probe.registration->reset();
    }
};

struct BlockingMaintenanceProbe final {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool release{false};

    static void check(void* target) noexcept {
        auto& probe = *static_cast<BlockingMaintenanceProbe*>(target);
        std::unique_lock lock(probe.mutex);
        probe.entered = true;
        probe.condition.notify_one();
        probe.condition.wait(lock, [&probe] { return probe.release; });
    }
};

}  // namespace

int main() {
    asio::io_context ioContext;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 16);
    auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    try {
        ruvia::ConnectionScanner invalid(ruvia::WorkerHandle{}, {});
        return 100;
    } catch (const std::invalid_argument&) {
    }
    const auto rejects = [&worker](ruvia::ConnectionScannerOptions options) {
        try {
            ruvia::ConnectionScanner scanner(worker, std::move(options));
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    {
        auto options = ruvia::ConnectionScannerOptions{};
        options.idle_timeout = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 1;
        }
    }
    {
        auto options = ruvia::ConnectionScannerOptions{};
        options.initialReadTimeout = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 2;
        }
    }
    {
        auto options = ruvia::ConnectionScannerOptions{};
        options.payloadReadTimeout = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 3;
        }
    }
    {
        auto options = ruvia::ConnectionScannerOptions{};
        options.write_timeout = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 4;
        }
    }
    {
        auto options = ruvia::ConnectionScannerOptions{};
        options.scanInterval = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 5;
        }
    }

    // A failed start is transactional. Scheduling off the worker is rejected,
    // but it must not poison the lifecycle state and suppress a later valid
    // on-worker retry.
    {
        auto options = ruvia::ConnectionScannerOptions{};
        options.scanInterval = std::chrono::milliseconds(1);
        ruvia::ConnectionScanner scanner(worker, std::move(options));
        if (scanner.worker().id() != worker.id()) {
            return 101;
        }
        WorkerMaintenanceProbe retryProbe;
        ruvia::ConnectionScanner::WorkerMaintenanceRegistration retryRegistration;
        scanner.registerWorkerMaintenance(
            retryRegistration, &retryProbe, &WorkerMaintenanceProbe::check);
        bool offWorkerRejected = false;
        try {
            scanner.start();
        } catch (const std::logic_error&) {
            offWorkerRejected = true;
        }
        if (!offWorkerRejected ||
            dispatcher->post([&scanner] { scanner.start(); }) != ruvia::PostStatus::kAccepted) {
            return 6;
        }
        ioContext.run_for(std::chrono::milliseconds(20));
        if (retryProbe.ticks == 0 ||
            dispatcher->post([&scanner] { scanner.stop(); }) != ruvia::PostStatus::kAccepted) {
            return 7;
        }
        if (ioContext.stopped()) {
            ioContext.restart();
        }
        ioContext.run_for(std::chrono::milliseconds(5));
    }

    asio::ip::tcp::socket socket(ioContext);
    ruvia::ConnectionScanner::Entry firstEntry;
    ruvia::ConnectionScanner::Entry secondEntry;
    std::optional<ruvia::ConnectionScanner::Guard> firstGuard;
    std::optional<ruvia::ConnectionScanner::Guard> secondGuard;

    {
        ruvia::ConnectionScanner scanner(worker, ruvia::ConnectionScannerOptions{});
        firstGuard.emplace(&scanner, firstEntry, socket);
        secondGuard.emplace(&scanner, secondEntry, socket);
    }

    // Scanner teardown must invalidate every coarse timestamp pointer and
    // detach every intrusive entry before longer-lived guards are destroyed.
    firstEntry.touch();
    firstEntry.setPhase(ruvia::ConnectionScanner::Phase::kReadingInitial);
    secondEntry.touch();
    secondEntry.setPhase(ruvia::ConnectionScanner::Phase::kWriting);
    firstGuard.reset();
    secondGuard.reset();

    // Multiplexed protocols can own more long-lived streams than the old fixed
    // eight-slot scanner table. Every checked object supplies its own intrusive
    // node, so registration remains allocation-free without silently dropping
    // the ninth stream.
    ioContext.restart();
    {
        auto options = ruvia::ConnectionScannerOptions{};
        options.scanInterval = std::chrono::milliseconds(1);
        ruvia::ConnectionScanner scanner(worker, std::move(options));
        ruvia::ConnectionScanner::Entry entry;
        ruvia::ConnectionScanner::Guard guard(&scanner, entry, socket);
        std::array<PeriodicProbe, 12> probes{};
        std::array<ruvia::ConnectionScanner::PeriodicCheckRegistration, 12> registrations{};
        PeriodicResetProbe resetProbe{&registrations[11]};
        ruvia::ConnectionScanner::PeriodicCheckRegistration resetRegistration;
        std::array<WorkerMaintenanceProbe, 8> workerProbes{};
        std::array<ruvia::ConnectionScanner::WorkerMaintenanceRegistration, 8>
            workerRegistrations{};
        WorkerMaintenanceResetProbe workerResetProbe{&workerRegistrations[7]};
        ruvia::ConnectionScanner::WorkerMaintenanceRegistration workerResetRegistration;
        if (dispatcher->post([&] {
                // start() initially has no work. Registrations added afterward
                // must become visible without any coarse timeout being enabled.
                scanner.start();
                for (std::size_t i = 0; i < registrations.size(); ++i) {
                    entry.registerPeriodicCheck(registrations[i], &probes[i], &PeriodicProbe::tick);
                }
                entry.registerPeriodicCheck(
                    resetRegistration, &resetProbe, &PeriodicResetProbe::tick);
                for (std::size_t i = 0; i < workerRegistrations.size(); ++i) {
                    scanner.registerWorkerMaintenance(
                        workerRegistrations[i], &workerProbes[i], &WorkerMaintenanceProbe::check);
                }
                scanner.registerWorkerMaintenance(workerResetRegistration, &workerResetProbe,
                    &WorkerMaintenanceResetProbe::check);
                entry.setPhase(ruvia::ConnectionScanner::Phase::kLongLived);
            }) != ruvia::PostStatus::kAccepted) {
            return 6;
        }
        // Windows timer dispatch can occasionally exceed a 10 ms scheduling
        // window under a parallel Debug build. Give the 1 ms scanner enough
        // time to complete at least one deterministic pass.
        ioContext.run_for(std::chrono::milliseconds(50));
        if (dispatcher->post([&scanner] { scanner.stop(); }) != ruvia::PostStatus::kAccepted) {
            return 7;
        }
        if (ioContext.stopped()) {
            ioContext.restart();
        }
        ioContext.run_for(std::chrono::milliseconds(5));
        for (std::size_t i = 0; i < probes.size(); ++i) {
            if ((i == 11 && probes[i].ticks != 0) || (i != 11 && probes[i].ticks == 0)) {
                return 8;
            }
        }
        if (resetProbe.ticks == 0) {
            return 9;
        }
        for (std::size_t i = 0; i < workerProbes.size(); ++i) {
            if ((i == 7 && workerProbes[i].ticks != 0) || (i != 7 && workerProbes[i].ticks == 0)) {
                return 10;
            }
        }
        if (workerResetProbe.ticks == 0) {
            return 11;
        }
    }

    // Long-lived sessions own their deadlines while ordinary phases retain
    // scanner timeouts. Periodic liveness checks must still run on idle sessions.
    ioContext.restart();
    {
        using Scanner = ruvia::ConnectionScanner;
        Scanner scanner(worker, {.scanInterval = std::chrono::milliseconds(1),
                                    .idle_timeout = std::chrono::milliseconds(1),
                                    .initialReadTimeout = std::chrono::milliseconds(1),
                                    .payloadReadTimeout = std::chrono::milliseconds(1),
                                    .write_timeout = std::chrono::milliseconds(1)});
        const std::array phases{Scanner::Phase::kIdle, Scanner::Phase::kReadingInitial,
            Scanner::Phase::kReadingPayload, Scanner::Phase::kWriting,
            Scanner::Phase::kLongLived, Scanner::Phase::kLongLived};
        std::array<asio::ip::tcp::socket, 6> sockets{
            asio::ip::tcp::socket(ioContext), asio::ip::tcp::socket(ioContext),
            asio::ip::tcp::socket(ioContext), asio::ip::tcp::socket(ioContext),
            asio::ip::tcp::socket(ioContext), asio::ip::tcp::socket(ioContext)};
        std::array<Scanner::Entry, 6> entries;
        std::array<std::optional<Scanner::Guard>, 6> guards;
        PeriodicProbe liveness;
        Scanner::PeriodicCheckRegistration livenessRegistration;
        for (std::size_t i = 0; i < sockets.size(); ++i) {
            sockets[i].open(asio::ip::tcp::v4());
            guards[i].emplace(&scanner, entries[i], sockets[i]);
            entries[i].setPhase(phases[i]);
        }
        entries[4].registerPeriodicCheck(livenessRegistration, &liveness, &PeriodicProbe::tick);
        if (dispatcher->post([&scanner] { scanner.start(); }) != ruvia::PostStatus::kAccepted) {
            return 13;
        }
        ioContext.run_for(std::chrono::milliseconds(50));
        for (std::size_t i = 0; i < sockets.size(); ++i) {
            if (sockets[i].is_open() != (phases[i] == Scanner::Phase::kLongLived)) {
                return 14;
            }
        }
        if (liveness.ticks == 0) {
            return 15;
        }
        // Returning to ordinary idle restores its deadline; no exemption leaks
        // from an earlier long-lived phase, with or without periodic checks.
        entries[4].setPhase(Scanner::Phase::kIdle);
        entries[5].setPhase(Scanner::Phase::kIdle);
        ioContext.run_for(std::chrono::milliseconds(50));
        if (sockets[4].is_open() || sockets[5].is_open()) {
            return 16;
        }
        if (dispatcher->post([&scanner] { scanner.stop(); }) != ruvia::PostStatus::kAccepted) {
            return 17;
        }
        ioContext.run_for(std::chrono::milliseconds(5));
    }

    // I/O progress renews inactivity, but cannot renew an absolute phase limit.
    ioContext.restart();
    {
        using Scanner = ruvia::ConnectionScanner;
        Scanner scanner(worker, {.scanInterval = std::chrono::milliseconds(1),
                                    .initialReadTimeout = std::chrono::seconds(1),
                                    .payloadReadTimeout = std::chrono::seconds(1),
                                    .initial_read_completion_timeout = std::chrono::milliseconds(5),
                                    .payload_read_completion_timeout = std::chrono::milliseconds(5)});
        struct Progress final {
            Scanner::Entry* entry;
            Scanner::Phase phase;
            static void tick(void* raw, std::int64_t) noexcept {
                auto& self = *static_cast<Progress*>(raw);
                self.entry->touch();
                self.entry->setPhase(self.phase);
            }
        };
        std::array<asio::ip::tcp::socket, 2> sockets{
            asio::ip::tcp::socket(ioContext), asio::ip::tcp::socket(ioContext)};
        std::array<Scanner::Entry, 2> entries;
        std::array<std::optional<Scanner::Guard>, 2> guards;
        std::array<Scanner::PeriodicCheckRegistration, 2> registrations;
        std::array<Progress, 2> progress{{{&entries[0], Scanner::Phase::kReadingInitial},
            {&entries[1], Scanner::Phase::kReadingPayload}}};
        for (std::size_t i = 0; i < sockets.size(); ++i) {
            sockets[i].open(asio::ip::tcp::v4());
            guards[i].emplace(&scanner, entries[i], sockets[i]);
            entries[i].setPhase(progress[i].phase);
            entries[i].registerPeriodicCheck(registrations[i], &progress[i], &Progress::tick);
        }
        if (dispatcher->post([&scanner] { scanner.start(); }) != ruvia::PostStatus::kAccepted) {
            return 18;
        }
        ioContext.run_for(std::chrono::milliseconds(50));
        if (sockets[0].is_open() || sockets[1].is_open()) {
            return 19;
        }
        if (dispatcher->post([&scanner] { scanner.stop(); }) != ruvia::PostStatus::kAccepted) {
            return 20;
        }
        ioContext.run_for(std::chrono::milliseconds(5));
    }

    // Entry teardown invalidates registrations that happen to outlive it;
    // their own RAII reset must then be harmless.
    ruvia::ConnectionScanner::PeriodicCheckRegistration registration;
    PeriodicProbe probe;
    {
        ruvia::ConnectionScanner::Entry entry;
        entry.registerPeriodicCheck(registration, &probe, &PeriodicProbe::tick);
    }
    registration.reset();

    // An expiry callback may already be scanning while another thread destroys
    // the scanner. Destruction must wait for that callback instead of letting
    // its recursive re-arm call observe a freed scanner.
    bool scannerLifetimeSafe = true;
    {
        asio::io_context scannerIo;
        auto scannerDispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(scannerIo, 16);
        auto scannerWorker = ruvia::detail::WorkerHandleAccess::make(scannerDispatcher);
        auto scanner = std::make_unique<ruvia::ConnectionScanner>(scannerWorker,
            ruvia::ConnectionScannerOptions{.scanInterval = std::chrono::milliseconds(1)});
        BlockingMaintenanceProbe blockingProbe;
        ruvia::ConnectionScanner::WorkerMaintenanceRegistration maintenance;
        scanner->registerWorkerMaintenance(
            maintenance, &blockingProbe, &BlockingMaintenanceProbe::check);
        if (scannerDispatcher->post([&scanner] { scanner->start(); }) !=
            ruvia::PostStatus::kAccepted) {
            scannerLifetimeSafe = false;
        }
        std::thread scannerThread([&] { scannerIo.run(); });
        {
            std::unique_lock lock(blockingProbe.mutex);
            if (!blockingProbe.condition.wait_for(lock, std::chrono::milliseconds(100),
                    [&blockingProbe] { return blockingProbe.entered; })) {
                scannerLifetimeSafe = false;
            }
        }
        std::atomic_bool destroyed{false};
        std::thread destroyer([scanner = std::move(scanner), &destroyed]() mutable {
            scanner.reset();
            destroyed.store(true, std::memory_order_release);
        });
        if (blockingProbe.entered) {
            const auto waitUntil =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (!destroyed.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < waitUntil) {
                std::this_thread::yield();
            }
            if (destroyed.load(std::memory_order_acquire)) {
                scannerLifetimeSafe = false;
            }
        }
        {
            std::lock_guard lock(blockingProbe.mutex);
            blockingProbe.release = true;
        }
        blockingProbe.condition.notify_one();
        destroyer.join();
        scannerIo.stop();
        scannerThread.join();
        scannerDispatcher->detachContext();
    }
    if (!scannerLifetimeSafe) {
        return 12;
    }

    // Scanner teardown likewise invalidates startup-owned maintenance nodes.
    ruvia::ConnectionScanner::WorkerMaintenanceRegistration maintenanceRegistration;
    WorkerMaintenanceProbe maintenanceProbe;
    {
        ruvia::ConnectionScanner scanner(worker, ruvia::ConnectionScannerOptions{});
        scanner.registerWorkerMaintenance(
            maintenanceRegistration, &maintenanceProbe, &WorkerMaintenanceProbe::check);
    }
    maintenanceRegistration.reset();
}
