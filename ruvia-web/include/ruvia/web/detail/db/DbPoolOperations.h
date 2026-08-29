#pragma once

#include <array>
#include <charconv>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/io/OperationDeadline.h"
#include "ruvia/core/detail/pool/PoolLeaseScheduler.h"
#include "ruvia/core/detail/worker/WorkerCancellationPost.h"
#include "ruvia/core/detail/worker/WorkerTimer.h"
#include "ruvia/web/db/DbRows.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbHostResolution.h"

// The parts of a pooled database operation that do not depend on the driver:
// which slot it runs on, and what happens to that slot when it fails.

namespace ruvia::detail {

enum class DbSlotAbortReason : std::uint8_t {
    kNone,
    kCancelled,
};

template <typename Pool>
using DbOperationCancellationMailbox = WorkerCancellationMailbox<Pool>;

template <typename Pool>
class DbSlotCancellationGuard final {
public:
    DbSlotCancellationGuard(Pool& pool, std::size_t slot, const StopToken& stopToken)
        : pool_(&pool),
          slot_(slot) {
        auto& connection = pool.slots_[slot];
        connection.abortReason = DbSlotAbortReason::kNone;
        if (!stopToken.stoppable()) {
            return;
        }
        cancellationId_ = pool.cancellationMailbox_->nextOperationId();
        connection.cancellationId = cancellationId_;
        stopToken.registerCallback(
            stopRegistration_, WorkerCancellationPost<DbOperationCancellationMailbox<Pool>>(
                                   pool.cancellationMailbox_, cancellationId_));
        if (stopToken.stopRequested()) {
            pool_->cancelOperationById(cancellationId_);
        }
    }

    DbSlotCancellationGuard(const DbSlotCancellationGuard&) = delete;
    DbSlotCancellationGuard& operator=(const DbSlotCancellationGuard&) = delete;

    ~DbSlotCancellationGuard() {
        finish();
    }

    void finish() noexcept {
        stopRegistration_.reset();
        if (pool_ != nullptr) {
            auto& connection = pool_->slots_[slot_];
            if (connection.cancellationId == cancellationId_) {
                connection.cancellationId = 0;
            }
            pool_ = nullptr;
        }
    }

private:
    Pool* pool_;
    std::size_t slot_;
    std::uint64_t cancellationId_{0};
    StopRegistration stopRegistration_;
};

// A pool slot held for the duration of one operation. Releasing it is the
// caller's obligation however the operation ends, so the guard owns that: the
// driver code between acquire and release can throw freely.
template <typename Pool>
class DbSlotGuard final {
public:
    DbSlotGuard(Pool& pool, std::size_t slot) noexcept
        : pool_(&pool),
          slot_(slot) {}
    DbSlotGuard(const DbSlotGuard&) = delete;
    DbSlotGuard& operator=(const DbSlotGuard&) = delete;
    ~DbSlotGuard() {
        if (pool_ != nullptr) {
            pool_->releaseSlot(slot_);
        }
    }

private:
    Pool* pool_;
    std::size_t slot_;
};

template <typename Slot>
class DbSlotActiveWaitGuard final {
public:
    explicit DbSlotActiveWaitGuard(Slot& slot) noexcept
        : slot_(slot) {
        if (slot_.waitActive) {
            std::terminate();
        }
        slot_.waitActive = true;
    }

    DbSlotActiveWaitGuard(const DbSlotActiveWaitGuard&) = delete;
    DbSlotActiveWaitGuard& operator=(const DbSlotActiveWaitGuard&) = delete;

    ~DbSlotActiveWaitGuard() {
        slot_.waitActive = false;
    }

private:
    Slot& slot_;
};

// Taking and giving back a slot is pure lease bookkeeping: no driver is
// involved, so both pools share these. A release that names no live lease is a
// bug in the caller, not a runtime condition, and cannot be reported through a
// noexcept path.
template <typename Pool>
Task<std::size_t> acquireDbSlot(Pool& pool, const OperationTimeout& timeout, StopToken stopToken) {
    const auto acquireTimeout = timeout.constrainedBy(pool.config_.acquireTimeout).remaining();
    const auto result =
        co_await pool.scheduler_.acquire(acquireTimeout, std::move(stopToken), pool.worker_);
    if (const auto* acquired = result.acquired()) {
        co_return acquired->index();
    }
    if (result.timedOut() != nullptr) {
        throw DbError(DbError::Code::kTimeout, pool.config_.driver,
            "database connection pool acquire timed out");
    }
    if (result.cancelled() != nullptr) {
        throw DbError(
            DbError::Code::kCancelled, pool.config_.driver, "database operation cancelled");
    }
    throw DbError(DbError::Code::kClosing, pool.config_.driver, "database client is closing");
}

template <typename Pool>
void releaseDbSlot(Pool& pool, std::size_t slot) noexcept {
    const auto status = pool.scheduler_.release(slot);
    if (status == PoolLeaseReleaseStatus::kInvalidSlot ||
        status == PoolLeaseReleaseStatus::kAlreadyReleased) {
        std::terminate();
    }
}

// Driver pools share their worker-affine lifecycle. The driver-specific slot
// close and connect operations remain on the pool so this base only owns the
// scheduling and cancellation rules.
template <typename Pool>
class DbPoolLifecycleBase {
public:
    [[nodiscard]] Task<void> connect() {
        auto& pool = derived();
        const OperationTimeout operationTimeout(std::nullopt);
        for (auto& slot : pool.slots_) {
            co_await pool.connectUnlocked(slot, operationTimeout);
        }
    }

    void closeNow() noexcept {
        auto& pool = derived();
        (void)pool.scheduler_.close();
        for (auto& slot : pool.slots_) {
            pool.closeSlot(slot);
        }
    }

    [[nodiscard]] Task<std::size_t> acquireSlot(
        const OperationTimeout& timeout, StopToken stopToken) {
        return acquireDbSlot(derived(), timeout, std::move(stopToken));
    }

    void releaseSlot(std::size_t slot) noexcept {
        releaseDbSlot(derived(), slot);
    }

    void cancelOperationById(std::uint64_t cancellationId) noexcept {
        auto& pool = derived();
        for (auto& slot : pool.slots_) {
            if (slot.cancellationId != cancellationId) {
                continue;
            }
            slot.abortReason = DbSlotAbortReason::kCancelled;
            pool.closeSlot(slot);
            return;
        }
    }

    template <typename Slot>
    void throwIfCancelled(const Slot& slot) const {
        if (slot.abortReason == DbSlotAbortReason::kCancelled) {
            throw DbError(
                DbError::Code::kCancelled, derived().config_.driver, "database operation cancelled");
        }
    }

private:
    [[nodiscard]] Pool& derived() noexcept {
        return static_cast<Pool&>(*this);
    }
    [[nodiscard]] const Pool& derived() const noexcept {
        return static_cast<const Pool&>(*this);
    }
};

// Ending a transaction is the same for every driver: run the control statement
// on the slot the transaction holds, and release that slot exactly once. A
// failure closes the connection before releasing, because a slot whose COMMIT
// or ROLLBACK did not complete cannot be handed to the next caller.
//
// `Pool` supplies slots_, executeControl(), closeSlot() and releaseSlot(); it
// declares this a friend so the shared rule stays out of the drivers.
template <typename Pool>
Task<void> finishDbTransaction(Pool& pool, std::size_t slot, std::string_view command,
    std::pmr::memory_resource* resource, const OperationOptions& options) {
    if (slot >= pool.slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    const OperationTimeout operationTimeout(options.timeout);
    DbSlotCancellationGuard cancellation(pool, slot, options.stopToken);
    try {
        co_await pool.executeControl(pool.slots_[slot], command, resource, operationTimeout);
    } catch (...) {
        pool.closeSlot(pool.slots_[slot]);
        cancellation.finish();
        pool.releaseSlot(slot);
        throw;
    }
    cancellation.finish();
    pool.releaseSlot(slot);
}

// Starting a transaction has the same lease transfer for every driver. The
// backend control statement owns cancellation checks and connect-on-demand;
// duplicating those here would run the same policy twice on the call chain.
template <typename Pool>
Task<DbTransaction> beginDbTransaction(Pool& pool, std::string_view command,
    std::pmr::memory_resource* resource, OperationOptions options) {
    const OperationTimeout operationTimeout(options.timeout);
    const auto slotIndex = co_await pool.acquireSlot(operationTimeout, options.stopToken);
    DbSlotCancellationGuard cancellation(pool, slotIndex, options.stopToken);
    try {
        co_await pool.executeControl(
            pool.slots_[slotIndex], command, resource, operationTimeout);
        co_return DbTransaction(DbPoolRef{&pool}, slotIndex, resource, std::move(options));
    } catch (...) {
        pool.closeSlot(pool.slots_[slotIndex]);
        cancellation.finish();
        pool.releaseSlot(slotIndex);
        throw;
    }
}

// A statement inside an open transaction: it runs on the slot the transaction
// already holds, so it neither acquires nor releases one. A failure closes the
// connection and gives the slot back, because a transaction whose statement
// failed mid-protocol cannot continue on it.
template <typename Pool>
Task<DbRows> queryOnDbTransactionSlot(Pool& pool, std::size_t slot, std::pmr::string sql,
    std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
    const OperationOptions& options) {
    if (slot >= pool.slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    const OperationTimeout operationTimeout(options.timeout);
    DbSlotCancellationGuard cancellation(pool, slot, options.stopToken);
    try {
        co_return co_await pool.queryOnSlot(
            pool.slots_[slot], sql, std::span<const DbValue>(params), resource, operationTimeout);
    } catch (...) {
        pool.closeSlot(pool.slots_[slot]);
        cancellation.finish();
        pool.releaseSlot(slot);
        throw;
    }
}

template <typename Pool>
Task<DbExecResult> executeOnDbTransactionSlot(Pool& pool, std::size_t slot, std::pmr::string sql,
    std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
    const OperationOptions& options) {
    if (slot >= pool.slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    const OperationTimeout operationTimeout(options.timeout);
    DbSlotCancellationGuard cancellation(pool, slot, options.stopToken);
    try {
        co_return co_await pool.executeOnSlot(
            pool.slots_[slot], sql, std::span<const DbValue>(params), resource, operationTimeout);
    } catch (...) {
        pool.closeSlot(pool.slots_[slot]);
        cancellation.finish();
        pool.releaseSlot(slot);
        throw;
    }
}

// One buffered statement on a pooled connection. The shape is the same for every
// driver: refuse empty SQL before taking a slot, hold the slot for the duration,
// and close the connection if the statement throws -- a slot whose statement
// failed mid-protocol cannot be reused. The guard releases the slot either way.
template <typename Pool>
Task<DbRows> executeDbQuery(Pool& pool, std::pmr::string sql, std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource, OperationOptions options) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }

    const OperationTimeout operationTimeout(options.timeout);
    const auto slotIndex = co_await pool.acquireSlot(operationTimeout, options.stopToken);
    typename Pool::SlotGuard guard(pool, slotIndex);
    DbSlotCancellationGuard cancellation(pool, slotIndex, options.stopToken);
    try {
        co_return co_await pool.queryOnSlot(pool.slots_[slotIndex], sql,
            std::span<const DbValue>(params), resource, operationTimeout);
    } catch (...) {
        pool.closeSlot(pool.slots_[slotIndex]);
        throw;
    }
}

template <typename Pool>
Task<DbExecResult> executeDbCommand(Pool& pool, std::pmr::string sql,
    std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
    OperationOptions options) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }

    const OperationTimeout operationTimeout(options.timeout);
    const auto slotIndex = co_await pool.acquireSlot(operationTimeout, options.stopToken);
    typename Pool::SlotGuard guard(pool, slotIndex);
    DbSlotCancellationGuard cancellation(pool, slotIndex, options.stopToken);
    try {
        co_return co_await pool.executeOnSlot(pool.slots_[slotIndex], sql,
            std::span<const DbValue>(params), resource, operationTimeout);
    } catch (...) {
        pool.closeSlot(pool.slots_[slotIndex]);
        throw;
    }
}

// The pool's configured port as a NUL-terminated buffer, the form asio's
// resolver takes it in.
[[nodiscard]] inline std::array<char, 6> formatDbPort(
    std::uint16_t port, std::string_view backend) {
    std::array<char, 6> output{};
    const auto parsed = std::to_chars(output.data(), output.data() + output.size() - 1, port);
    if (parsed.ec != std::errc{}) {
        throw std::logic_error(std::string("failed to format ").append(backend).append(" port"));
    }
    *parsed.ptr = '\0';
    return output;
}

// Resolving the configured host is the same for every driver: asio's resolver
// under the operation deadline, with the slot's resolve deadline armed around
// it so a stalled resolve is torn down with everything else on that slot. The
// deadline is checked once before arming and once after resuming, because the
// timer may have fired while this coroutine was suspended.
//
// Only the backend's name in the diagnostics differs, so it is the argument.
// `Pool` supplies config_, resource_, setSlotDeadline() and clearSlotDeadline();
// it declares this a friend so the shared rule stays out of the drivers.
template <typename Pool, typename Slot>
Task<DbResolvedAddresses> resolveDbHost(
    Pool& pool, Slot& slot, const OperationTimeout& deadline, std::string_view backend) {
    const auto timedOut = [&pool, backend] {
        return DbError(DbError::Code::kTimeout, pool.config_.driver,
            std::string(backend).append(" host resolve timed out"));
    };

    pool.throwIfCancelled(slot);
    const auto remaining = deadline.remaining();
    if (remaining.has_value() && remaining->count() <= 0) {
        throw timedOut();
    }
    if (remaining.has_value()) {
        pool.setSlotDeadline(slot, *remaining, Slot::DeadlineKind::kResolve);
    } else {
        pool.clearSlotDeadline(slot);
    }

    DbSlotActiveWaitGuard activeResolve(slot);

    const auto port = formatDbPort(pool.config_.port, backend);
    try {
        auto completion = co_await asyncAsio<asio::ip::tcp::resolver::results_type>(
            [&pool, &slot, &port](auto handler) mutable {
                slot.resolver.async_resolve(
                    pool.config_.host, std::string_view(port.data()), std::move(handler));
            });
        const auto resolveError = completion.errorCode();
        auto results = std::move(completion).takeResult();
        const auto afterResolve = deadline.remaining();
        const bool slotDeadlineExpired = slot.deadline.expired();
        pool.clearSlotDeadline(slot);
        const bool deadlineExpired =
            slotDeadlineExpired || (afterResolve.has_value() && afterResolve->count() <= 0);
        pool.throwIfCancelled(slot);
        if (slot.closeRequested) {
            throw DbError(
                DbError::Code::kClosing, pool.config_.driver, "database client is closing");
        }
        if (deadlineExpired) {
            throw timedOut();
        }
        if (resolveError) {
            throw DbError(DbError::Code::kResolveFailed, pool.config_.driver,
                std::system_error(
                    resolveError, std::string("resolving ").append(backend).append(" host failed"))
                    .what(),
                resolveError.value());
        }
        co_return collectDbResolvedAddresses(results, pool.config_.driver, pool.resource_);
    } catch (...) {
        pool.clearSlotDeadline(slot);
        throw;
    }
}

}  // namespace ruvia::detail
