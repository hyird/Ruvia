#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <mysql/mysql.h>

#include <exception>
#include <memory_resource>
#include <utility>

namespace ruvia {

detail::MariaDbPool::ConnectionSlot::ConnectionSlot(asio::io_context& ioContext, std::pmr::memory_resource* resource)
    : resolver(ioContext),
      waitSocket(nullptr, SlotSocketDeleter{detail::pmrResourceOrDefault(resource)}),
      socketQuarantine(detail::makePmrObject<detail::DbSlotSocketQuarantine>(
          detail::processResource(),
          ioContext)) {}

detail::MariaDbPool::ConnectionSlot::~ConnectionSlot() {
    if (waitActive) {
        std::terminate();
    }
    if (waitSocket != nullptr) {
        socketQuarantine->retain(std::move(*waitSocket), connection);
        waitSocket.reset();
        connection = nullptr;
        // Deliberately abandon the process-backed node: its socket destructor
        // and mysql_close() must not both close the same native handle.
        (void)socketQuarantine.release();
    }
}
detail::MariaDbPool::ConnectionSlot::ConnectionSlot(ConnectionSlot&&) noexcept = default;
detail::MariaDbPool::ConnectionSlot& detail::MariaDbPool::ConnectionSlot::operator=(ConnectionSlot&&) noexcept = default;

detail::MariaDbPool::MariaDbPool(asio::io_context& ioContext, DbConfigStorage config, std::pmr::memory_resource* resource, const WorkerHandle* worker)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(detail::pmrResourceOrDefault(resource)),
      slots_(resource_),
      scheduler_(1, resource_),
      worker_(worker == nullptr ? WorkerHandle{} : *worker) {
    detail::validateDbConfig(config_);
    if (config_.driver != DbDriver::kMariaDb) {
        throw std::invalid_argument("MariaDB pool requires the MariaDB driver");
    }
    slots_.reserve(1);
    slots_.emplace_back(ioContext_, resource_);
    if (worker_.valid()) {
        slots_.back().cancellationState = makeDbOperationCancellationState(worker_, *this, 0);
    }
}

detail::MariaDbPool::~MariaDbPool() {
    for (auto& slot : slots_) {
        if (slot.cancellationState != nullptr) {
            slot.cancellationState->detach(this);
        }
    }
    closeNow();
}

Task<void> detail::MariaDbPool::connect() {
    const OperationTimeout operationTimeout(std::nullopt);
    for (auto& slot : slots_) {
        co_await connectUnlocked(slot, operationTimeout);
    }
}

void detail::MariaDbPool::closeNow() noexcept {
    (void)scheduler_.close();
    for (auto& slot : slots_) {
        closeSlot(slot);
    }
}

void detail::MariaDbPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    // A per-operation timeout may constrain acquire even when DbConfig does not.
    scheduler_.scanDeadlines(now);

    for (auto& slot : slots_) {
        const auto kind = slot.deadline.expire(now);
        if (!kind.has_value()) {
            continue;
        }
        if (*kind == ConnectionSlot::DeadlineKind::kResolve) {
            slot.resolver.cancel();
        } else if (*kind == ConnectionSlot::DeadlineKind::kSocket) {
            if (slot.waitSocket != nullptr) {
                slot.waitSocket->cancel();
            }
        } else if (*kind == ConnectionSlot::DeadlineKind::kSleep) {
            auto handle = std::exchange(slot.deadlineContinuation, {});
            if (handle) {
                handle.resume();
            }
        }
    }
}

bool detail::MariaDbPool::needsDeadlineScan() const noexcept {
    return true;
}

Task<std::size_t> detail::MariaDbPool::acquireSlot(const OperationTimeout& timeout, StopToken stopToken) {
    return detail::acquireDbSlot(*this, timeout, std::move(stopToken));
}

void detail::MariaDbPool::releaseSlot(std::size_t slot) noexcept {
    detail::releaseDbSlot(*this, slot);
}

void detail::MariaDbPool::closeSlot(ConnectionSlot& slot) noexcept {
    slot.closeRequested = true;
    slot.resolver.cancel();
    if (slot.waitActive) {
        const auto* activeKind = slot.deadline.kind();
        if (activeKind != nullptr && *activeKind == ConnectionSlot::DeadlineKind::kSleep) {
            auto handle = std::exchange(slot.deadlineContinuation, {});
            if (handle) {
                handle.resume();
            }
        } else if (slot.waitSocket != nullptr) {
            // Keep the wrapper and driver connection alive until every queued
            // wait completion has run. Cancellation wakes the coroutine; it
            // releases the borrowed socket before completing slot teardown.
            slot.waitSocket->cancel();
        }
        return;
    }

    const auto* kind = slot.deadline.kind();
    if (kind != nullptr && *kind == ConnectionSlot::DeadlineKind::kSocket && slot.waitSocket != nullptr) {
        slot.waitSocket->cancel();
    } else if (kind != nullptr && *kind == ConnectionSlot::DeadlineKind::kSleep) {
        auto handle = std::exchange(slot.deadlineContinuation, {});
        if (handle) {
            handle.resume();
        }
    }
    clearSlotDeadline(slot);
    if (slot.waitSocket != nullptr) {
        // release() leaves the wrapper attached on failure. Keep both objects
        // in the slot so a later close attempt can retry without either owner
        // closing the native socket behind the other.
        if (slot.waitSocket->release()) {
            (void)scheduler_.close();
            return;
        }
        slot.waitSocket.reset();
    }
    if (slot.connection != nullptr) {
        mysql_close(slot.connection);
        slot.connection = nullptr;
    }
    slot.connected = false;
    slot.closeRequested = false;
}

void detail::MariaDbPool::cancelOperation(std::size_t slotIndex, std::uint64_t generation) noexcept {
    if (slotIndex >= slots_.size()) {
        std::terminate();
    }
    auto& slot = slots_[slotIndex];
    if (slot.operationGeneration != generation) {
        return;
    }
    slot.abortReason = DbSlotAbortReason::kCancelled;
    closeSlot(slot);
}

void detail::MariaDbPool::throwIfCancelled(const ConnectionSlot& slot) const {
    if (slot.abortReason == DbSlotAbortReason::kCancelled) {
        throw DbError(DbError::Code::kCancelled, DbDriver::kMariaDb, "database operation cancelled");
    }
}

void detail::MariaDbPool::setSlotDeadline(ConnectionSlot& slot, std::chrono::milliseconds timeout, ConnectionSlot::DeadlineKind kind) noexcept {
    if (timeout.count() <= 0) {
        slot.deadline.reset();
        return;
    }
    slot.deadline.arm(detail::workerTimerDeadlineAfter(timeout), kind);
}

void detail::MariaDbPool::clearSlotDeadline(ConnectionSlot& slot) noexcept {
    (void)slot.deadline.clear();
    slot.deadlineContinuation = {};
}

}  // namespace ruvia
