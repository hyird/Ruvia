#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <libpq-fe.h>

#include <exception>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {

PostgreSqlPool::ConnectionSlot::ConnectionSlot(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource)
    : resolver(ioContext),
      waitSocket(nullptr, SlotSocketDeleter{pmrResourceOrDefault(resource)}) {}

PostgreSqlPool::ConnectionSlot::~ConnectionSlot() {
    if (waitActive) {
        std::terminate();
    }
}
PostgreSqlPool::ConnectionSlot::ConnectionSlot(ConnectionSlot&&) noexcept = default;
PostgreSqlPool::ConnectionSlot& PostgreSqlPool::ConnectionSlot::operator=(ConnectionSlot&&) noexcept = default;

PostgreSqlPool::PostgreSqlPool(
    asio::io_context& ioContext,
    DbConfig config,
    std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(pmrResourceOrDefault(resource)),
      slots_(resource_),
      scheduler_(1, resource_) {
    validateDbConfig(config_);
    if (config_.driver != DbDriver::kPostgreSql) {
        throw std::invalid_argument("PostgreSQL pool requires the PostgreSQL driver");
    }
    slots_.reserve(1);
    slots_.emplace_back(ioContext_, resource_);
}

PostgreSqlPool::~PostgreSqlPool() {
    closeNow();
}

Task<void> PostgreSqlPool::connect() {
    for (auto& slot : slots_) {
        co_await connectUnlocked(slot);
    }
}

void PostgreSqlPool::closeNow() noexcept {
    (void)scheduler_.close();
    for (auto& slot : slots_) {
        closeSlot(slot);
    }
}

void PostgreSqlPool::scanDeadlines(
    std::chrono::steady_clock::time_point now) noexcept {
    if (config_.acquireTimeout.has_value()) {
        scheduler_.scanDeadlines(now);
    }
    for (auto& slot : slots_) {
        if (!slot.deadline.expire(now).has_value()) {
            continue;
        }
        const auto* kind = slot.deadline.kind();
        if (kind != nullptr &&
            *kind == ConnectionSlot::DeadlineKind::kResolve) {
            slot.resolver.cancel();
        } else if (slot.waitSocket != nullptr) {
            slot.waitSocket->cancel();
        }
    }
}

bool PostgreSqlPool::hasAnyTimeout() const noexcept {
    return config_.connectTimeout.has_value() ||
        config_.queryTimeout.has_value() ||
        config_.readTimeout.has_value() ||
        config_.writeTimeout.has_value() ||
        config_.acquireTimeout.has_value();
}

Task<std::size_t> PostgreSqlPool::acquireSlot() {
    const auto result = co_await scheduler_.acquire(config_.acquireTimeout);
    if (const auto* acquired = result.acquired()) {
        co_return acquired->index();
    }
    if (result.timedOut() != nullptr) {
        throw std::runtime_error(
            "database connection pool acquire timed out");
    }
    throw std::runtime_error("database client is closing");
}

void PostgreSqlPool::releaseSlot(std::size_t slot) noexcept {
    const auto status = scheduler_.release(slot);
    if (status == PoolLeaseReleaseStatus::kInvalidSlot ||
        status == PoolLeaseReleaseStatus::kAlreadyReleased) {
        std::terminate();
    }
}

void PostgreSqlPool::closeSlot(ConnectionSlot& slot) noexcept {
    slot.closeRequested = true;
    slot.resolver.cancel();
    if (slot.waitActive) {
        if (slot.waitSocket != nullptr) {
            // The wait callback lives in the suspended Task frame and still
            // refers to this wrapper. Detaching cancels the Asio operation
            // without closing the driver-owned fd; final disposal is performed
            // by the coroutine's failure path after the callback.
            if (!slot.waitSocket->release()) {
                std::terminate();
            }
        }
        return;
    }

    if (slot.waitSocket != nullptr) {
        slot.waitSocket->cancel();
        if (!slot.waitSocket->release()) {
            std::terminate();
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

void PostgreSqlPool::setSlotDeadline(
    ConnectionSlot& slot,
    std::optional<std::chrono::milliseconds> timeout) noexcept {
    if (!timeout.has_value() || timeout->count() <= 0) {
        slot.deadline.reset();
        return;
    }
    slot.deadline.arm(
        workerTimerDeadlineAfter(*timeout),
        ConnectionSlot::DeadlineKind::kSocket);
}

void PostgreSqlPool::clearSlotDeadline(ConnectionSlot& slot) noexcept {
    (void)slot.deadline.clear();
}

PostgreSqlPool::SlotGuard::SlotGuard(
    PostgreSqlPool& client,
    std::size_t slot) noexcept
    : client_(&client), slot_(slot) {}

PostgreSqlPool::SlotGuard::~SlotGuard() {
    if (client_ != nullptr) {
        client_->releaseSlot(slot_);
    }
}

}  // namespace ruvia::detail
