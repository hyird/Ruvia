#include <libpq-fe.h>

#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/db/DbUtils.h"

#include "db/DbRegistry.h"
#include "db/DbSlotSocket.h"

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
      socketQuarantine(makePmrObject<DbSlotSocketQuarantine>(processResource(), ioContext)),
      deadlineTimer(makePmrObject<WorkerTimerRegistration>(pmrResourceOrDefault(resource))) {}

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
      worker_(requirePostgreSqlWorker(worker)),
      slots_(resource_),
      scheduler_(1, worker_, resource_),
      cancellation_target_(make_worker_cancellation_target(*this, worker_)) {
    if (config_.driver != DbDriver::kPostgreSql) {
        throw std::invalid_argument("PostgreSQL pool requires the PostgreSQL driver");
    }
    slots_.reserve(1);
    slots_.emplace_back(ioContext_, resource_);
}

PostgreSqlPool::~PostgreSqlPool() {
    cancellation_target_->detach(*this);
    closeNow();
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
    clear_db_slot_deadline(slot);
    if (slot.connection != nullptr) {
        PQfinish(slot.connection);
        slot.connection = nullptr;
    }
    slot.connected = false;
    slot.closeRequested = false;
}

void PostgreSqlPool::ConnectionSlot::expire_deadline(
    ConnectionSlot& slot, DeadlineKind kind) noexcept {
    if (kind == DeadlineKind::kResolve) {
        slot.resolver.cancel();
    } else if (slot.waitSocket != nullptr) {
        slot.waitSocket->cancel();
    }
}

}  // namespace ruvia::detail
