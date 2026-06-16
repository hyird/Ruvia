#include "ConnectionScanner.h"

#include <asio/error.hpp>

namespace ruvia::detail {

namespace {

[[nodiscard]] std::int64_t steadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void closeSocket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

}  // namespace

void ConnectionScanner::Entry::touch() noexcept {
    if (nowMs != nullptr) {
        lastActiveMs = *nowMs;
    }
}

void ConnectionScanner::Entry::setPhase(Phase nextPhase) noexcept {
    if (nowMs == nullptr) {
        return;
    }
    const auto now = *nowMs;
    lastActiveMs = now;
    if (phase != nextPhase) {
        phase = nextPhase;
        phaseStartedMs = now;
    }
}

ConnectionScanner::Guard::Guard(ConnectionScanner* scanner, Entry& entry, asio::ip::tcp::socket& socket)
    : scanner_(scanner), entry_(&entry) {
    if (scanner_ != nullptr) {
        scanner_->registerEntry(*entry_, socket);
    }
}

ConnectionScanner::Guard::~Guard() {
    if (scanner_ != nullptr && entry_ != nullptr) {
        scanner_->unregisterEntry(*entry_);
    }
}

ConnectionScanner::ConnectionScanner(asio::any_io_executor executor, HttpServerOptions options)
    : timer_(std::move(executor)), options_(options), cachedNowMs_(steadyNowMs()) {
    sentinel_.prev = &sentinel_;
    sentinel_.next = &sentinel_;
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
    entry.socket = &socket;
    entry.nowMs = &cachedNowMs_;
    entry.touch();
    entry.phaseStartedMs = entry.lastActiveMs;
    entry.phase = Phase::kIdle;
    entry.next = sentinel_.next;
    entry.prev = &sentinel_;
    sentinel_.next->prev = &entry;
    sentinel_.next = &entry;
}

void ConnectionScanner::unregisterEntry(Entry& entry) noexcept {
    if (entry.prev == nullptr || entry.next == nullptr) {
        return;
    }

    entry.prev->next = entry.next;
    entry.next->prev = entry.prev;
    entry.prev = nullptr;
    entry.next = nullptr;
    entry.socket = nullptr;
    entry.webSocketTarget = nullptr;
    entry.webSocketTick = nullptr;
}

void ConnectionScanner::closeAll() noexcept {
    auto* current = sentinel_.next;
    while (current != &sentinel_) {
        auto* next = current->next;
        if (current->socket != nullptr) {
            closeSocket(*current->socket);
        }
        current = next;
    }
}

std::chrono::milliseconds ConnectionScanner::interval() const noexcept {
    if (options_.scanInterval.count() > 0) {
        return options_.scanInterval;
    }

    return std::chrono::milliseconds(1000);
}

bool ConnectionScanner::hasAnyTimeout() const noexcept {
    return options_.idleTimeout.count() > 0 ||
        options_.headerTimeout.count() > 0 ||
        options_.bodyTimeout.count() > 0 ||
        options_.writeTimeout.count() > 0;
}

void ConnectionScanner::schedule() {
    if (!running_) {
        return;
    }

    timer_.expires_after(interval());
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
    auto* current = sentinel_.next;
    while (current != &sentinel_) {
        auto* next = current->next;
        const bool webSocketClose = current->webSocketTick != nullptr &&
            current->webSocketTarget != nullptr &&
            current->webSocketTick(current->webSocketTarget, now);
        if (current->socket != nullptr && (webSocketClose || isTimedOut(*current, now))) {
            closeSocket(*current->socket);
        }
        current = next;
    }
}

bool ConnectionScanner::isTimedOut(const Entry& entry, std::int64_t now) const noexcept {
    if (options_.idleTimeout.count() > 0 && now - entry.lastActiveMs >= options_.idleTimeout.count()) {
        return true;
    }

    switch (entry.phase) {
        case Phase::kReadingHeader:
            return options_.headerTimeout.count() > 0 && now - entry.phaseStartedMs >= options_.headerTimeout.count();
        case Phase::kReadingBody:
            return options_.bodyTimeout.count() > 0 && now - entry.phaseStartedMs >= options_.bodyTimeout.count();
        case Phase::kWebSocket:
            return options_.idleTimeout.count() > 0 && now - entry.lastActiveMs >= options_.idleTimeout.count();
        case Phase::kWriting:
            return options_.writeTimeout.count() > 0 && now - entry.phaseStartedMs >= options_.writeTimeout.count();
        case Phase::kIdle:
        default:
            return options_.idleTimeout.count() > 0 && now - entry.lastActiveMs >= options_.idleTimeout.count();
    }
}

}  // namespace ruvia::detail
