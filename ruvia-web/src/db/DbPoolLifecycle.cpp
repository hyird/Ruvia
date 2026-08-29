#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <mysql/mysql.h>

#include <exception>
#include <memory_resource>
#include <utility>

namespace ruvia {
namespace {

[[nodiscard]] const WorkerHandle& requireMariaDbWorker(const WorkerHandle& worker) {
    if (!worker.valid()) {
        throw std::invalid_argument("MariaDB pool requires a valid worker");
    }
    return worker;
}

}  // namespace

detail::MariaDbPool::ConnectionSlot::ConnectionSlot(
    asio::io_context& ioContext, std::pmr::memory_resource* resource)
    : resolver(ioContext),
      waitSocket(nullptr, SlotSocketDeleter{detail::pmrResourceOrDefault(resource)}),
      socketQuarantine(detail::makePmrObject<detail::DbSlotSocketQuarantine>(
          detail::processResource(), ioContext)),
      deadlineTimer(detail::makePmrObject<detail::WorkerTimerRegistration>(
          detail::pmrResourceOrDefault(resource))) {}

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
detail::MariaDbPool::ConnectionSlot& detail::MariaDbPool::ConnectionSlot::operator=(
    ConnectionSlot&&) noexcept = default;

detail::MariaDbPool::MariaDbPool(asio::io_context& ioContext, const WorkerHandle& worker,
    DbConfigStorage config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(detail::pmrResourceOrDefault(resource)),
      worker_(requireMariaDbWorker(worker)),
      slots_(resource_),
      scheduler_(1, worker_, resource_),
      cancellationMailbox_(makeWorkerCancellationMailbox(*this, worker_)) {
    if (config_.driver != DbDriver::kMariaDb) {
        throw std::invalid_argument("MariaDB pool requires the MariaDB driver");
    }
    slots_.reserve(1);
    slots_.emplace_back(ioContext_, resource_);
}

detail::MariaDbPool::~MariaDbPool() {
    cancellationMailbox_->detach(*this);
    closeNow();
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
    if (kind != nullptr && *kind == ConnectionSlot::DeadlineKind::kSocket &&
        slot.waitSocket != nullptr) {
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

void detail::MariaDbPool::setSlotDeadline(
    ConnectionSlot& slot, std::chrono::milliseconds timeout, ConnectionSlot::DeadlineKind kind) {
    clearSlotDeadline(slot);
    if (timeout.count() <= 0) {
        return;
    }
    const auto deadline = detail::workerTimerDeadlineAfter(timeout);
    slot.deadline.arm(deadline, kind);
    try {
        WorkerHandleAccess::scheduleTimer(
            worker_, *slot.deadlineTimer, deadline, [&slot](WorkerTimerOutcome outcome) noexcept {
                if (outcome != WorkerTimerOutcome::kExpired) {
                    return;
                }
                const auto expired = slot.deadline.expire(std::chrono::steady_clock::now());
                if (!expired.has_value()) {
                    return;
                }
                switch (*expired) {
                    case ConnectionSlot::DeadlineKind::kResolve:
                        slot.resolver.cancel();
                        break;
                    case ConnectionSlot::DeadlineKind::kSocket:
                        if (slot.waitSocket != nullptr) {
                            slot.waitSocket->cancel();
                        }
                        break;
                    case ConnectionSlot::DeadlineKind::kSleep: {
                        auto handle = std::exchange(slot.deadlineContinuation, {});
                        if (handle) {
                            handle.resume();
                        }
                        break;
                    }
                }
            });
    } catch (...) {
        slot.deadline.reset();
        throw;
    }
}

void detail::MariaDbPool::clearSlotDeadline(ConnectionSlot& slot) noexcept {
    slot.deadlineTimer->cancel();
    (void)slot.deadline.clear();
    slot.deadlineContinuation = {};
}

}  // namespace ruvia
