#include "net/server/ConnectionScanner.h"

#include "net/server/SocketUtils.h"

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

void ConnectionScanner::Entry::setWebSocketHeartbeat(void* target, WebSocketTick tick) noexcept {
    // Reuse the slot already holding this target (re-register), else the first free one.
    HeartbeatSlot* free = nullptr;
    for (auto& slot : webSocketHeartbeats_) {
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
    // else: all slots taken -- this tunnel simply gets no server-initiated heartbeat.
}

void ConnectionScanner::Entry::clearWebSocketHeartbeat(void* target) noexcept {
    for (auto& slot : webSocketHeartbeats_) {
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

bool ConnectionScanner::Entry::tickWebSocket(std::int64_t now) noexcept {
    // Tick EVERY registered tunnel; if any signals a dead peer (missed pong) the
    // connection is closed, tearing down all tunnels that share the socket.
    bool shouldClose = false;
    for (auto& slot : webSocketHeartbeats_) {
        if (slot.tick != nullptr && slot.target != nullptr && slot.tick(slot.target, now)) {
            shouldClose = true;
        }
    }
    return shouldClose;
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

ConnectionScanner::ConnectionScanner(asio::any_io_executor executor, ConnectionScannerOptions options)
    : timer_(std::move(executor)), options_(options), cachedNowMs_(steadyNowMs()) {
    sentinel_.prev_ = &sentinel_;
    sentinel_.next_ = &sentinel_;
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
    if (!entry.linked()) {
        return;
    }

    entry.prev_->next_ = entry.next_;
    entry.next_->prev_ = entry.prev_;
    entry.prev_ = nullptr;
    entry.next_ = nullptr;
    entry.socket_ = nullptr;
    entry.nowMs_ = nullptr;
    entry.webSocketHeartbeats_ = {};
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
    return options_.keepaliveTimeoutMs > 0 ||
        options_.clientHeaderTimeoutMs > 0 ||
        options_.clientBodyTimeoutMs > 0 ||
        options_.sendTimeoutMs > 0;
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
        const bool webSocketClose = current->tickWebSocket(now);
        if (current->socket_ != nullptr && (webSocketClose || isTimedOut(*current, now))) {
            closeSocket(*current->socket_);
        }
        current = next;
    }
}

bool ConnectionScanner::isTimedOut(const Entry& entry, std::int64_t now) const noexcept {
    // nginx-style inactivity timeouts: every phase measures time since the connection was last
    // active (touch() is called on each successful read/write), so a timer resets on I/O and
    // fires only after a gap. Each phase maps to its nginx directive:
    //   kReadingHeader -> client_header_timeout   kReadingBody -> client_body_timeout
    //   kWriting       -> send_timeout            kIdle/kWebSocket -> keepalive_timeout
    const auto inactiveMs = now - entry.lastActiveMs_;
    switch (entry.phase_) {
        case Phase::kReadingHeader:
            return options_.clientHeaderTimeoutMs > 0 && inactiveMs >= options_.clientHeaderTimeoutMs;
        case Phase::kReadingBody:
            return options_.clientBodyTimeoutMs > 0 && inactiveMs >= options_.clientBodyTimeoutMs;
        case Phase::kWriting:
            return options_.sendTimeoutMs > 0 && inactiveMs >= options_.sendTimeoutMs;
        case Phase::kWebSocket:
        case Phase::kIdle:
        default:
            return options_.keepaliveTimeoutMs > 0 && inactiveMs >= options_.keepaliveTimeoutMs;
    }
}

}  // namespace ruvia::detail
