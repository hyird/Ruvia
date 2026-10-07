#include "ruvia/core/detail/io/ConnectionScanner.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "ruvia/core/Socket.h"

namespace ruvia {
namespace {

[[nodiscard]] std::int64_t steadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] WorkerHandle requireScannerWorker(WorkerHandle worker) {
    if (!worker.valid()) {
        throw std::invalid_argument("connection scanner requires a valid worker");
    }
    return worker;
}

void validateScannerTimeout(const std::optional<std::chrono::milliseconds>& timeout) {
    if (timeout.has_value() && timeout->count() <= 0) {
        throw std::invalid_argument(
            "configured connection scanner timeouts must be greater than zero");
    }
}

[[nodiscard]] bool timeoutExpired(
    const std::optional<std::chrono::milliseconds>& timeout, std::int64_t inactiveMs) noexcept {
    return timeout.has_value() && inactiveMs >= timeout->count();
}

}  // namespace

ConnectionScanner::WorkerMaintenanceRegistration::~WorkerMaintenanceRegistration() noexcept {
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
    if (phase_ != nextPhase) {
        phase_started_ms_ = *nowMs_;
    }
    phase_ = nextPhase;
}
std::int64_t ConnectionScanner::Entry::lastActiveMs() const noexcept {
    return lastActiveMs_;
}
void ConnectionScanner::Entry::registerPeriodicCheck(
    PeriodicCheckRegistration& registration, void* target, PeriodicCheck tick) noexcept {
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
    registration.prev_ = registration.next_ = nullptr;
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
void ConnectionScanner::Entry::runPeriodicChecks(std::int64_t now) noexcept {
    periodicScanNext_ = periodicChecks_;
    while (periodicScanNext_ != nullptr) {
        auto* registration = periodicScanNext_;
        periodicScanNext_ = registration->next_;
        if (registration->tick_ != nullptr && registration->target_ != nullptr) {
            registration->tick_(registration->target_, now);
        }
    }
}
ConnectionScanner::Guard::Guard(
    ConnectionScanner* scanner, Entry& entry, asio::ip::tcp::socket& socket)
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

ConnectionScanner::Impl::Impl(
    ConnectionScanner* owner, WorkerHandle worker, ConnectionScannerOptions options)
    : owner_(owner),
      worker_(requireScannerWorker(std::move(worker))),
      timerState_(std::make_shared<TimerState>(this)),
      options_(std::move(options)),
      cachedNowMs_(steadyNowMs()) {
    if (options_.scanInterval.count() <= 0) {
        throw std::invalid_argument("connection scanner interval must be greater than zero");
    }
    validateScannerTimeout(options_.idleTimeout);
    validateScannerTimeout(options_.initialReadTimeout);
    validateScannerTimeout(options_.payloadReadTimeout);
    validateScannerTimeout(options_.writeTimeout);
    validateScannerTimeout(options_.initial_read_completion_timeout);
    validateScannerTimeout(options_.payload_read_completion_timeout);
    sentinel_.prev_ = sentinel_.next_ = &sentinel_;
}
ConnectionScanner::Impl::~Impl() noexcept {
    stop();
    {
        std::lock_guard lock(timerState_->mutex);
        timerState_->owner = nullptr;
    }
    while (sentinel_.next_ != &sentinel_) {
        unregisterEntry(*sentinel_.next_);
    }
    detachWorkerMaintenance();
}

ConnectionScanner::ConnectionScanner(WorkerHandle worker, ConnectionScannerOptions options)
    : impl_(std::make_unique<Impl>(this, std::move(worker), std::move(options))) {}
ConnectionScanner::~ConnectionScanner() noexcept = default;
const WorkerHandle& ConnectionScanner::worker() const& noexcept {
    return impl_->worker_;
}
void ConnectionScanner::start() {
    impl_->start();
}
void ConnectionScanner::stop() noexcept {
    impl_->stop();
}
void ConnectionScanner::registerWorkerMaintenance(WorkerMaintenanceRegistration& registration,
    void* target, WorkerMaintenanceCheck check) noexcept {
    impl_->registerWorkerMaintenance(registration, target, check);
}
void ConnectionScanner::registerEntry(Entry& entry, asio::ip::tcp::socket& socket) noexcept {
    impl_->registerEntry(entry, &socket);
}
void ConnectionScanner::registerEntry(Entry& entry) noexcept {
    impl_->registerEntry(entry, nullptr);
}
void ConnectionScanner::unregisterEntry(Entry& entry) noexcept {
    impl_->unregisterEntry(entry);
}
void ConnectionScanner::closeAll() noexcept {
    impl_->closeAll();
}
void ConnectionScanner::detachEntry(Entry& entry) noexcept {
    if (!entry.linked()) {
        return;
    }
    entry.prev_->next_ = entry.next_;
    entry.next_->prev_ = entry.prev_;
    entry.prev_ = entry.next_ = nullptr;
    entry.socket_ = nullptr;
    entry.nowMs_ = nullptr;
    entry.detachPeriodicChecks();
    entry.scanner_ = nullptr;
}
void ConnectionScanner::periodicCheckAdded() noexcept {
    impl_->periodicCheckAdded();
}
void ConnectionScanner::periodicCheckRemoved() noexcept {
    impl_->periodicCheckRemoved();
}
void ConnectionScanner::removeWorkerMaintenance(WorkerMaintenanceRegistration& registration) noexcept {
    impl_->removeWorkerMaintenance(registration);
}

void ConnectionScanner::Impl::start() {
    if (running_) {
        return;
    }
    running_ = true;
    try {
        schedule();
    } catch (...) {
        running_ = false;
        throw;
    }
}
void ConnectionScanner::Impl::stop() noexcept {
    running_ = false;
    timer_.cancel();
}
void ConnectionScanner::Impl::registerWorkerMaintenance(WorkerMaintenanceRegistration& registration,
    void* target, WorkerMaintenanceCheck check) noexcept {
    registration.reset();
    if (target == nullptr || check == nullptr) {
        return;
    }
    registration.scanner_ = owner_;
    registration.target_ = target;
    registration.check_ = check;
    registration.next_ = workerMaintenance_;
    if (workerMaintenance_ != nullptr) {
        workerMaintenance_->prev_ = &registration;
    }
    workerMaintenance_ = &registration;
}
void ConnectionScanner::Impl::registerEntry(Entry& entry, asio::ip::tcp::socket* socket) noexcept {
    entry.socket_ = socket;
    entry.scanner_ = owner_;
    entry.nowMs_ = &cachedNowMs_;
    entry.touch();
    entry.phase_started_ms_ = cachedNowMs_;
    entry.phase_ = Phase::kIdle;
    entry.next_ = sentinel_.next_;
    entry.prev_ = &sentinel_;
    sentinel_.next_->prev_ = &entry;
    sentinel_.next_ = &entry;
    for (auto* registration = entry.periodicChecks_; registration != nullptr;
        registration = registration->next_) {
        periodicCheckAdded();
    }
}
void ConnectionScanner::Impl::unregisterEntry(Entry& entry) noexcept {
    if (!entry.linked()) {
        return;
    }
    entry.prev_->next_ = entry.next_;
    entry.next_->prev_ = entry.prev_;
    entry.prev_ = entry.next_ = nullptr;
    entry.socket_ = nullptr;
    entry.nowMs_ = nullptr;
    entry.detachPeriodicChecks();
    entry.scanner_ = nullptr;
}
void ConnectionScanner::Impl::closeAll() noexcept {
    for (auto* current = sentinel_.next_; current != &sentinel_; current = current->next_) {
        if (current->socket_ != nullptr) {
            closeSocket(*current->socket_);
        }
    }
}
void ConnectionScanner::Impl::periodicCheckAdded() noexcept {
    ++periodicCheckCount_;
}
void ConnectionScanner::Impl::periodicCheckRemoved() noexcept {
    if (periodicCheckCount_ > 0) {
        --periodicCheckCount_;
    }
}
void ConnectionScanner::Impl::removeWorkerMaintenance(
    WorkerMaintenanceRegistration& registration) noexcept {
    if (registration.scanner_ != owner_) {
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
    registration.prev_ = registration.next_ = nullptr;
    registration.target_ = nullptr;
    registration.check_ = nullptr;
}
void ConnectionScanner::Impl::detachWorkerMaintenance() noexcept {
    workerMaintenanceScanNext_ = nullptr;
    while (workerMaintenance_ != nullptr) {
        auto* registration = workerMaintenance_;
        workerMaintenance_ = registration->next_;
        registration->scanner_ = nullptr;
        registration->prev_ = registration->next_ = nullptr;
        registration->target_ = nullptr;
        registration->check_ = nullptr;
    }
}
bool ConnectionScanner::Impl::hasScanningWork() const noexcept {
    return options_.idleTimeout.has_value() || options_.initialReadTimeout.has_value() ||
           options_.payloadReadTimeout.has_value() || options_.writeTimeout.has_value() ||
           options_.initial_read_completion_timeout.has_value() ||
           options_.payload_read_completion_timeout.has_value() ||
           workerMaintenance_ != nullptr || periodicCheckCount_ != 0;
}
void ConnectionScanner::Impl::schedule() {
    if (!running_) {
        return;
    }
    const auto timerState = timerState_;
    (worker_).schedule_timer(timer_, ::ruvia::workerTimerDeadlineAfter(options_.scanInterval), [timerState](WorkerTimerOutcome outcome) {
        if (outcome == WorkerTimerOutcome::kCancelled) {
            return;
        }
        std::lock_guard lock(timerState->mutex);
        auto* scanner = timerState->owner;
        if (scanner == nullptr || !scanner->running_) {
            return;
        }
        if (scanner->hasScanningWork()) {
            scanner->scan();
        }
        scanner->schedule();
    });
}
void ConnectionScanner::Impl::scan() noexcept {
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
        current->runPeriodicChecks(now);
        if (current->socket_ != nullptr && isTimedOut(*current, now)) {
            closeSocket(*current->socket_);
        }
        current = next;
    }
}
bool ConnectionScanner::Impl::isTimedOut(const Entry& entry, std::int64_t now) const noexcept {
    const auto inactiveMs = now - entry.lastActiveMs_;
    switch (entry.phase_) {
        case Phase::kReadingInitial:
            return timeoutExpired(options_.initialReadTimeout, inactiveMs) ||
                   timeoutExpired(options_.initial_read_completion_timeout, now - entry.phase_started_ms_);
        case Phase::kReadingPayload:
            return timeoutExpired(options_.payloadReadTimeout, inactiveMs) ||
                   timeoutExpired(options_.payload_read_completion_timeout, now - entry.phase_started_ms_);
        case Phase::kWriting:
            return timeoutExpired(options_.writeTimeout, inactiveMs);
        case Phase::kLongLived:
            return false;
        case Phase::kIdle:
        default:
            return timeoutExpired(options_.idleTimeout, inactiveMs);
    }
}

}  // namespace ruvia
