#include "ruvia/core/detail/ConnectionScanner.h"

#include "ruvia/core/detail/SocketUtils.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {

namespace {

[[nodiscard]] std::int64_t steadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void validateScannerTimeout(
    const std::optional<std::chrono::milliseconds>& timeout) {
    if (timeout.has_value() && timeout->count() <= 0) {
        throw std::invalid_argument(
            "configured connection scanner timeouts must be greater than zero");
    }
}

[[nodiscard]] bool timeoutExpired(
    const std::optional<std::chrono::milliseconds>& timeout,
    std::int64_t inactiveMs) noexcept {
    return timeout.has_value() && inactiveMs >= timeout->count();
}

}  // namespace

ConnectionScanner::WorkerMaintenanceRegistration::
~WorkerMaintenanceRegistration() noexcept {
    reset();
}

void ConnectionScanner::WorkerMaintenanceRegistration::reset() noexcept {
    if (scanner_ != nullptr) {
        scanner_->removeWorkerMaintenance(*this);
    }
}

ConnectionScanner::PeriodicCheckRegistration::~PeriodicCheckRegistration() noexcept {
    reset();
}

void ConnectionScanner::PeriodicCheckRegistration::reset() noexcept {
    if (entry_ != nullptr) {
        entry_->removePeriodicCheck(*this);
    }
}

ConnectionScanner::Entry::~Entry() noexcept {
    detachPeriodicChecks();
}

void ConnectionScanner::Entry::touch() noexcept {
    if (nowMs_ != nullptr) {
        lastActiveMs_ = *nowMs_;
    }
}

void ConnectionScanner::Entry::setPhase(Phase nextPhase) noexcept {
    if (nowMs_ == nullptr) {
        return;
    }
    lastActiveMs_ = *nowMs_;
    phase_ = nextPhase;
}

std::int64_t ConnectionScanner::Entry::lastActiveMs() const noexcept {
    return lastActiveMs_;
}

void ConnectionScanner::Entry::registerPeriodicCheck(
    PeriodicCheckRegistration& registration,
    void* target,
    PeriodicCheck tick) noexcept {
    registration.reset();
    if (target == nullptr || tick == nullptr) {
        return;
    }

    registration.entry_ = this;
    registration.target_ = target;
    registration.tick_ = tick;
    registration.next_ = periodicChecks_;
    if (periodicChecks_ != nullptr) {
        periodicChecks_->prev_ = &registration;
    }
    periodicChecks_ = &registration;
    if (scanner_ != nullptr) {
        scanner_->periodicCheckAdded();
    }
}

void ConnectionScanner::Entry::removePeriodicCheck(
    PeriodicCheckRegistration& registration) noexcept {
    if (registration.entry_ != this) {
        return;
    }
    if (registration.prev_ != nullptr) {
        registration.prev_->next_ = registration.next_;
    } else {
        periodicChecks_ = registration.next_;
    }
    if (registration.next_ != nullptr) {
        registration.next_->prev_ = registration.prev_;
    }
    if (periodicScanNext_ == &registration) {
        periodicScanNext_ = registration.next_;
    }
    if (scanner_ != nullptr) {
        scanner_->periodicCheckRemoved();
    }
    registration.entry_ = nullptr;
    registration.prev_ = nullptr;
    registration.next_ = nullptr;
    registration.target_ = nullptr;
    registration.tick_ = nullptr;
}

void ConnectionScanner::Entry::detachPeriodicChecks() noexcept {
    periodicScanNext_ = nullptr;
    while (periodicChecks_ != nullptr) {
        periodicChecks_->reset();
    }
}

bool ConnectionScanner::Entry::linked() const noexcept {
    return prev_ != nullptr && next_ != nullptr;
}

bool ConnectionScanner::Entry::tickLongLived(std::int64_t now) noexcept {
    // Run every registered check; any failure closes the owning connection.
    bool shouldClose = false;
    periodicScanNext_ = periodicChecks_;
    while (periodicScanNext_ != nullptr) {
        auto* registration = periodicScanNext_;
        periodicScanNext_ = registration->next_;
        if (registration->tick_ != nullptr &&
            registration->target_ != nullptr &&
            registration->tick_(registration->target_, now)) {
            shouldClose = true;
        }
    }
    return shouldClose;
}

ConnectionScanner::Guard::Guard(ConnectionScanner* scanner, Entry& entry, asio::ip::tcp::socket& socket)
    : entry_(scanner != nullptr ? &entry : nullptr) {
    if (scanner != nullptr) {
        scanner->registerEntry(*entry_, socket);
    }
}

ConnectionScanner::Guard::~Guard() {
    if (entry_ != nullptr) {
        ConnectionScanner::detachEntry(*entry_);
    }
}

ConnectionScanner::ConnectionScanner(WorkerHandle worker, ConnectionScannerOptions options)
    : worker_(std::move(worker)), options_(std::move(options)), cachedNowMs_(steadyNowMs()) {
    if (options_.scanInterval.count() <= 0) {
        throw std::invalid_argument(
            "connection scanner interval must be greater than zero");
    }
    validateScannerTimeout(options_.idleTimeout);
    validateScannerTimeout(options_.initialReadTimeout);
    validateScannerTimeout(options_.payloadReadTimeout);
    validateScannerTimeout(options_.writeTimeout);
    sentinel_.prev_ = &sentinel_;
    sentinel_.next_ = &sentinel_;
}

ConnectionScanner::~ConnectionScanner() noexcept {
    stop();
    // Entries and their guards may be owned by longer-lived connection objects.
    // Remove every scanner-owned pointer before the sentinel and timestamp die.
    detachAllEntries();
    detachWorkerMaintenance();
}

void ConnectionScanner::start() {
    if (running_) {
        return;
    }
    // Registrations can appear after startup. Keep one coarse worker timer
    // armed, but schedule() skips the connection-list walk while no timeout,
    // worker maintenance check, or periodic registration exists.
    running_ = true;
    schedule();
}

void ConnectionScanner::stop() noexcept {
    running_ = false;
    timer_.cancel();
}

void ConnectionScanner::registerWorkerMaintenance(
    WorkerMaintenanceRegistration& registration,
    void* target,
    WorkerMaintenanceCheck check) noexcept {
    registration.reset();
    if (target == nullptr || check == nullptr) {
        return;
    }
    registration.scanner_ = this;
    registration.target_ = target;
    registration.check_ = check;
    registration.next_ = workerMaintenance_;
    if (workerMaintenance_ != nullptr) {
        workerMaintenance_->prev_ = &registration;
    }
    workerMaintenance_ = &registration;
}

void ConnectionScanner::registerEntry(Entry& entry, asio::ip::tcp::socket& socket) noexcept {
    entry.socket_ = &socket;
    entry.scanner_ = this;
    entry.nowMs_ = &cachedNowMs_;
    entry.touch();
    entry.phase_ = Phase::kIdle;
    entry.next_ = sentinel_.next_;
    entry.prev_ = &sentinel_;
    sentinel_.next_->prev_ = &entry;
    sentinel_.next_ = &entry;
    for (auto* registration = entry.periodicChecks_;
         registration != nullptr;
         registration = registration->next_) {
        periodicCheckAdded();
    }
}

void ConnectionScanner::unregisterEntry(Entry& entry) noexcept {
    detachEntry(entry);
}

void ConnectionScanner::detachEntry(Entry& entry) noexcept {
    if (!entry.linked()) {
        return;
    }

    entry.prev_->next_ = entry.next_;
    entry.next_->prev_ = entry.prev_;
    entry.prev_ = nullptr;
    entry.next_ = nullptr;
    entry.socket_ = nullptr;
    entry.nowMs_ = nullptr;
    entry.detachPeriodicChecks();
    entry.scanner_ = nullptr;
}

void ConnectionScanner::detachAllEntries() noexcept {
    while (sentinel_.next_ != &sentinel_) {
        detachEntry(*sentinel_.next_);
    }
}

void ConnectionScanner::closeAll() noexcept {
    auto* current = sentinel_.next_;
    while (current != &sentinel_) {
        auto* next = current->next_;
        if (current->socket_ != nullptr) {
            closeSocket(*current->socket_);
        }
        current = next;
    }
}

void ConnectionScanner::periodicCheckAdded() noexcept {
    ++periodicCheckCount_;
}

void ConnectionScanner::periodicCheckRemoved() noexcept {
    if (periodicCheckCount_ > 0) {
        --periodicCheckCount_;
    }
}

void ConnectionScanner::removeWorkerMaintenance(
    WorkerMaintenanceRegistration& registration) noexcept {
    if (registration.scanner_ != this) {
        return;
    }
    if (registration.prev_ != nullptr) {
        registration.prev_->next_ = registration.next_;
    } else {
        workerMaintenance_ = registration.next_;
    }
    if (registration.next_ != nullptr) {
        registration.next_->prev_ = registration.prev_;
    }
    if (workerMaintenanceScanNext_ == &registration) {
        workerMaintenanceScanNext_ = registration.next_;
    }
    registration.scanner_ = nullptr;
    registration.prev_ = nullptr;
    registration.next_ = nullptr;
    registration.target_ = nullptr;
    registration.check_ = nullptr;
}

void ConnectionScanner::detachWorkerMaintenance() noexcept {
    workerMaintenanceScanNext_ = nullptr;
    while (workerMaintenance_ != nullptr) {
        auto* registration = workerMaintenance_;
        workerMaintenance_ = registration->next_;
        registration->scanner_ = nullptr;
        registration->prev_ = nullptr;
        registration->next_ = nullptr;
        registration->target_ = nullptr;
        registration->check_ = nullptr;
    }
}

bool ConnectionScanner::hasScanningWork() const noexcept {
    return options_.idleTimeout.has_value() ||
        options_.initialReadTimeout.has_value() ||
        options_.payloadReadTimeout.has_value() ||
        options_.writeTimeout.has_value() ||
        workerMaintenance_ != nullptr ||
        periodicCheckCount_ != 0;
}

void ConnectionScanner::schedule() {
    if (!running_) {
        return;
    }

    timer_ = WorkerHandleAccess::scheduleTimer(
        worker_, std::chrono::steady_clock::now() + options_.scanInterval,
        [this](WorkerTimerOutcome outcome) {
        if (outcome == WorkerTimerOutcome::kCancelled || !running_) {
            return;
        }

        if (hasScanningWork()) {
            scan();
        }
        schedule();
    });
}

void ConnectionScanner::scan() noexcept {
    const auto now = steadyNowMs();
    cachedNowMs_ = now;
    workerMaintenanceScanNext_ = workerMaintenance_;
    while (workerMaintenanceScanNext_ != nullptr) {
        auto* registration = workerMaintenanceScanNext_;
        workerMaintenanceScanNext_ = registration->next_;
        registration->check_(registration->target_);
    }
    auto* current = sentinel_.next_;
    while (current != &sentinel_) {
        auto* next = current->next_;
        const bool periodicCheckFailed = current->tickLongLived(now);
        if (current->socket_ != nullptr && (periodicCheckFailed || isTimedOut(*current, now))) {
            closeSocket(*current->socket_);
        }
        current = next;
    }
}

bool ConnectionScanner::isTimedOut(const Entry& entry, std::int64_t now) const noexcept {
    // Every phase measures time since the connection was last active, so the
    // timeout resets on successful I/O and fires only after an inactivity gap.
    const auto inactiveMs = now - entry.lastActiveMs_;
    switch (entry.phase_) {
        case Phase::kReadingInitial:
            return timeoutExpired(options_.initialReadTimeout, inactiveMs);
        case Phase::kReadingPayload:
            return timeoutExpired(options_.payloadReadTimeout, inactiveMs);
        case Phase::kWriting:
            return timeoutExpired(options_.writeTimeout, inactiveMs);
        case Phase::kLongLived:
        case Phase::kIdle:
        default:
            return timeoutExpired(options_.idleTimeout, inactiveMs);
    }
}

}  // namespace ruvia::detail
