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

void ConnectionScanner::Entry::setPeriodicCheck(void* target, PeriodicCheck tick) noexcept {
    // Reuse the slot already holding this target (re-register), else the first free one.
    PeriodicCheckSlot* free = nullptr;
    for (auto& slot : periodicChecks_) {
        if (slot.target == target) {
            slot.tick = tick;
            return;
        }
        if (free == nullptr && slot.target == nullptr) {
            free = &slot;
        }
    }
    if (free != nullptr) {
        free->target = target;
        free->tick = tick;
    }
    // No free slot: the caller keeps its own fallback liveness behavior.
}

void ConnectionScanner::Entry::clearPeriodicCheck(void* target) noexcept {
    for (auto& slot : periodicChecks_) {
        if (slot.target == target) {
            slot.target = nullptr;
            slot.tick = nullptr;
            return;
        }
    }
}

bool ConnectionScanner::Entry::linked() const noexcept {
    return prev_ != nullptr && next_ != nullptr;
}

bool ConnectionScanner::Entry::tickLongLived(std::int64_t now) noexcept {
    // Run every registered check; any failure closes the owning connection.
    bool shouldClose = false;
    for (auto& slot : periodicChecks_) {
        if (slot.tick != nullptr && slot.target != nullptr && slot.tick(slot.target, now)) {
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
}

void ConnectionScanner::start() {
    if ((!hasAnyTimeout() && workerScannerCount_ == 0) || running_) {
        return;
    }

    running_ = true;
    schedule();
}

void ConnectionScanner::stop() noexcept {
    running_ = false;
    timer_.cancel();
}

void ConnectionScanner::setWorkerScanner(void* target, void (*scan)(void*) noexcept) noexcept {
    if (scan == nullptr || workerScannerCount_ >= workerScanners_.size()) {
        return;
    }
    workerScanners_[workerScannerCount_++] = WorkerScanner{target, scan};
}

void ConnectionScanner::registerEntry(Entry& entry, asio::ip::tcp::socket& socket) noexcept {
    entry.socket_ = &socket;
    entry.nowMs_ = &cachedNowMs_;
    entry.touch();
    entry.phase_ = Phase::kIdle;
    entry.next_ = sentinel_.next_;
    entry.prev_ = &sentinel_;
    sentinel_.next_->prev_ = &entry;
    sentinel_.next_ = &entry;
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
    entry.periodicChecks_ = {};
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

bool ConnectionScanner::hasAnyTimeout() const noexcept {
    return options_.idleTimeout.has_value() ||
        options_.initialReadTimeout.has_value() ||
        options_.payloadReadTimeout.has_value() ||
        options_.writeTimeout.has_value();
}

void ConnectionScanner::schedule() {
    if (!running_) {
        return;
    }

    timer_ = WorkerHandleAccess::scheduleTimer(
        worker_, std::chrono::steady_clock::now() + options_.scanInterval,
        [this](bool cancelled) {
        if (cancelled || !running_) {
            return;
        }

        scan();
        schedule();
    });
}

void ConnectionScanner::scan() noexcept {
    const auto now = steadyNowMs();
    cachedNowMs_ = now;
    for (std::size_t i = 0; i < workerScannerCount_; ++i) {
        workerScanners_[i].scan(workerScanners_[i].target);
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
