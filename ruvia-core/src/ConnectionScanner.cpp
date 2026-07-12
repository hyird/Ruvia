#include "ruvia/core/detail/ConnectionScanner.h"

#include "ruvia/core/detail/SocketUtils.h"

#include <asio/error.hpp>
#include <chrono>
#include <utility>

namespace ruvia::detail {

namespace {

[[nodiscard]] std::int64_t steadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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

ConnectionScanner::ConnectionScanner(asio::any_io_executor executor, ConnectionScannerOptions options)
    : timer_(std::move(executor)), options_(options), cachedNowMs_(steadyNowMs()) {
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
    try {
        timer_.cancel();
    } catch (...) {
    }
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
    return options_.idleTimeoutMs > 0 ||
        options_.initialReadTimeoutMs > 0 ||
        options_.payloadReadTimeoutMs > 0 ||
        options_.writeTimeoutMs > 0;
}

void ConnectionScanner::schedule() {
    if (!running_) {
        return;
    }

    timer_.expires_after(options_.scanInterval);
    timer_.async_wait([this](const std::error_code& ec) {
        if (ec || !running_) {
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
            return options_.initialReadTimeoutMs > 0 && inactiveMs >= options_.initialReadTimeoutMs;
        case Phase::kReadingPayload:
            return options_.payloadReadTimeoutMs > 0 && inactiveMs >= options_.payloadReadTimeoutMs;
        case Phase::kWriting:
            return options_.writeTimeoutMs > 0 && inactiveMs >= options_.writeTimeoutMs;
        case Phase::kLongLived:
        case Phase::kIdle:
        default:
            return options_.idleTimeoutMs > 0 && inactiveMs >= options_.idleTimeoutMs;
    }
}

}  // namespace ruvia::detail
