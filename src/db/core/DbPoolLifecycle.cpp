#include "../DbInternal.h"
#include "DbSlotSocket.h"
#include "DbUtils.h"

#include <mysql/mysql.h>

#include <algorithm>
#include <memory_resource>
#include <utility>

namespace ruvia {

detail::MariaDbPool::ConnectionSlot::ConnectionSlot(std::pmr::memory_resource* resource) noexcept
    : waitSocket(nullptr, SlotSocketDeleter{detail::resolveDbResource(resource)}) {}

detail::MariaDbPool::ConnectionSlot::~ConnectionSlot() = default;
detail::MariaDbPool::ConnectionSlot::ConnectionSlot(ConnectionSlot&&) noexcept = default;
detail::MariaDbPool::ConnectionSlot& detail::MariaDbPool::ConnectionSlot::operator=(ConnectionSlot&&) noexcept = default;

detail::MariaDbPool::MariaDbPool(asio::io_context& ioContext, DbConfig config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(detail::resolveDbResource(resource)),
      slots_(resource_),
      freeSlots_(resource_) {
    slots_.reserve(std::max<std::size_t>(1, config_.poolSize));
    freeSlots_.reserve(std::max<std::size_t>(1, config_.poolSize));
    for (std::size_t i = 0; i < std::max<std::size_t>(1, config_.poolSize); ++i) {
        slots_.emplace_back(resource_);
        freeSlots_.push_back(i);
    }
}

detail::MariaDbPool::~MariaDbPool() {
    closeNow();
}

Task<void> detail::MariaDbPool::connect() {
    for (auto& slot : slots_) {
        co_await connectUnlocked(slot);
    }
}

void detail::MariaDbPool::closeNow() noexcept {
    if (closing_) {
        return;
    }
    closing_ = true;
    while (waiterHead_ != nullptr) {
        auto* waiter = waiterHead_;
        removeWaiter(*waiter);
        if (waiter->ready != nullptr) {
            *waiter->ready = true;
        }
        if (waiter->slot != nullptr) {
            *waiter->slot = slots_.size();
        }
        if (waiter->handle) {
            waiter->handle.resume();
        }
    }
    for (auto& slot : slots_) {
        closeSlot(slot);
    }
}

void detail::MariaDbPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    auto* waiter = waiterHead_;
    while (waiter != nullptr) {
        auto* next = waiter->next;
        if (config_.acquireTimeout.count() > 0 && waiter->deadline <= now) {
            removeWaiter(*waiter);
            if (waiter->timedOut != nullptr) {
                *waiter->timedOut = true;
            }
            if (waiter->ready != nullptr) {
                *waiter->ready = true;
            }
            if (waiter->handle) {
                waiter->handle.resume();
            }
        }
        waiter = next;
    }

    for (auto& slot : slots_) {
        if (!slot.deadlineActive || slot.deadline > now) {
            continue;
        }
        slot.timedOut = true;
        if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSocket) {
            if (slot.waitSocket != nullptr) {
                slot.waitSocket->cancel();
            }
        } else if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSleep) {
            auto handle = slot.deadlineContinuation;
            slot.deadlineContinuation = {};
            if (handle) {
                handle.resume();
            }
        }
    }
}

bool detail::MariaDbPool::hasAnyTimeout() const noexcept {
    return config_.connectTimeout.count() > 0 ||
        config_.queryTimeout.count() > 0 ||
        config_.readTimeout.count() > 0 ||
        config_.writeTimeout.count() > 0 ||
        config_.acquireTimeout.count() > 0;
}

void detail::MariaDbPool::closeSlot(ConnectionSlot& slot) noexcept {
    if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSocket && slot.waitSocket != nullptr) {
        slot.waitSocket->cancel();
    } else if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSleep) {
        auto handle = slot.deadlineContinuation;
        slot.deadlineContinuation = {};
        if (handle) {
            handle.resume();
        }
    }
    clearSlotDeadline(slot);
    // Detach the fd from ASIO before mysql_close() closes it.
    if (slot.waitSocket != nullptr) {
        slot.waitSocket->release();
        slot.waitSocket.reset();
    }
    if (slot.connection != nullptr) {
        mysql_close(slot.connection);
        slot.connection = nullptr;
    }
    slot.connected = false;
}

void detail::MariaDbPool::setSlotDeadline(
    ConnectionSlot& slot,
    std::chrono::milliseconds timeout,
    ConnectionSlot::DeadlineKind kind) noexcept {
    slot.deadlineKind = kind;
    slot.timedOut = false;
    if (timeout.count() <= 0) {
        slot.deadlineActive = false;
        return;
    }
    slot.deadline = std::chrono::steady_clock::now() + timeout;
    slot.deadlineActive = true;
}

void detail::MariaDbPool::clearSlotDeadline(ConnectionSlot& slot) noexcept {
    slot.deadlineActive = false;
    slot.deadlineKind = ConnectionSlot::DeadlineKind::kNone;
    slot.deadlineContinuation = {};
}

}  // namespace ruvia
