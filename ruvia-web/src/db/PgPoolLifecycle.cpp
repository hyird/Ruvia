#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <libpq-fe.h>

#include <exception>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {
namespace {

[[nodiscard]] const WorkerHandle& requirePostgreSqlWorker(const WorkerHandle& worker) {
    if (!worker.valid()) {
        throw std::invalid_argument("PostgreSQL pool requires a valid worker");
    }
    return worker;
}

}  // namespace

PostgreSqlPool::ConnectionSlot::ConnectionSlot(
    asio::io_context& ioContext, std::pmr::memory_resource* resource)
    : resolver(ioContext),
      waitSocket(nullptr, SlotSocketDeleter{pmrResourceOrDefault(resource)}),
      socketQuarantine(makePmrObject<DbSlotSocketQuarantine>(processResource(), ioContext)) {}

PostgreSqlPool::ConnectionSlot::~ConnectionSlot() {
    if (waitActive) {
        std::terminate();
    }
    if (waitSocket != nullptr) {
        socketQuarantine->retain(std::move(*waitSocket), connection);
        waitSocket.reset();
        connection = nullptr;
        // Deliberately abandon the process-backed node: its socket destructor
        // and PQfinish() must not both close the same native handle.
        (void)socketQuarantine.release();
    }
}
PostgreSqlPool::ConnectionSlot::ConnectionSlot(ConnectionSlot&&) noexcept = default;
PostgreSqlPool::ConnectionSlot& PostgreSqlPool::ConnectionSlot::operator=(
    ConnectionSlot&&) noexcept = default;

PostgreSqlPool::PostgreSqlPool(asio::io_context& ioContext, const WorkerHandle& worker,
    DbConfigStorage config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(pmrResourceOrDefault(resource)),
      slots_(resource_),
      scheduler_(1, resource_),
      worker_(requirePostgreSqlWorker(worker)),
      cancellationMailbox_(makeWorkerCancellationMailbox(*this, worker_)) {
    if (config_.driver != DbDriver::kPostgreSql) {
        throw std::invalid_argument("PostgreSQL pool requires the PostgreSQL driver");
    }
    slots_.reserve(1);
    slots_.emplace_back(ioContext_, resource_);
}

PostgreSqlPool::~PostgreSqlPool() {
    cancellationMailbox_->detach(*this);
    closeNow();
}

Task<void> PostgreSqlPool::connect() {
    const OperationTimeout operationTimeout(std::nullopt);
    for (auto& slot : slots_) {
        co_await connectUnlocked(slot, operationTimeout);
    }
}

void PostgreSqlPool::closeNow() noexcept {
    (void)scheduler_.close();
    for (auto& slot : slots_) {
        closeSlot(slot);
    }
}

void PostgreSqlPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    // A per-operation timeout may constrain acquire even when DbConfig does not.
    scheduler_.scanDeadlines(now);
    for (auto& slot : slots_) {
        if (!slot.deadline.expire(now).has_value()) {
            continue;
        }
        const auto* kind = slot.deadline.kind();
        if (kind != nullptr && *kind == ConnectionSlot::DeadlineKind::kResolve) {
            slot.resolver.cancel();
        } else if (slot.waitSocket != nullptr) {
            slot.waitSocket->cancel();
        }
    }
}

Task<std::size_t> PostgreSqlPool::acquireSlot(
    const OperationTimeout& timeout, StopToken stopToken) {
    return acquireDbSlot(*this, timeout, std::move(stopToken));
}

void PostgreSqlPool::releaseSlot(std::size_t slot) noexcept {
    releaseDbSlot(*this, slot);
}

void PostgreSqlPool::closeSlot(ConnectionSlot& slot) noexcept {
    slot.closeRequested = true;
    slot.resolver.cancel();
    if (slot.waitActive) {
        if (slot.waitSocket != nullptr) {
            // The wait callback lives in the suspended Task frame and still
            // refers to this wrapper. Cancel it now; final disposal follows
            // after the callback drains and releases the borrowed socket.
            slot.waitSocket->cancel();
        }
        return;
    }

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
    clearSlotDeadline(slot);
    if (slot.connection != nullptr) {
        PQfinish(slot.connection);
        slot.connection = nullptr;
    }
    slot.connected = false;
    slot.closeRequested = false;
}

void PostgreSqlPool::cancelOperationById(std::uint64_t cancellationId) noexcept {
    for (auto& slot : slots_) {
        if (slot.cancellationId != cancellationId) {
            continue;
        }
        slot.abortReason = DbSlotAbortReason::kCancelled;
        closeSlot(slot);
        return;
    }
}

void PostgreSqlPool::throwIfCancelled(const ConnectionSlot& slot) const {
    if (slot.abortReason == DbSlotAbortReason::kCancelled) {
        throw DbError(
            DbError::Code::kCancelled, DbDriver::kPostgreSql, "database operation cancelled");
    }
}

void PostgreSqlPool::setSlotDeadline(
    ConnectionSlot& slot, std::optional<std::chrono::milliseconds> timeout) noexcept {
    if (!timeout.has_value() || timeout->count() <= 0) {
        slot.deadline.reset();
        return;
    }
    slot.deadline.arm(workerTimerDeadlineAfter(*timeout), ConnectionSlot::DeadlineKind::kSocket);
}

void PostgreSqlPool::clearSlotDeadline(ConnectionSlot& slot) noexcept {
    (void)slot.deadline.clear();
}

}  // namespace ruvia::detail
