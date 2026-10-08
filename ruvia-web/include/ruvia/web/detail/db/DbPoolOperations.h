#pragma once

#include <array>
#include <charconv>
#include <chrono>
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

#include "ruvia/core/Async.h"
#include "ruvia/core/OperationTimeout.h"
#include "ruvia/core/PoolLeaseScheduler.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerTimer.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/web/db/DbRows.h"
#include "ruvia/web/db/DbTransaction.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbHostResolution.h"
#include "ruvia/web/detail/db/DbTransactionStart.h"

// The parts of a pooled database operation that do not depend on the driver:
// which slot it runs on, and what happens to that slot when it fails.

namespace ruvia::detail {

enum class DbSlotAbortReason : std::uint8_t {
    kNone,
    kCancelled,
};

template <typename Pool>
using db_cancellation_target = worker_cancellation_target<Pool>;

template <typename Pool>
class DbSlotCancellationGuard final {
public:
    DbSlotCancellationGuard(Pool& pool, std::size_t slot, const StopToken& stopToken)
        : cancellation_(pool.cancellation_target_, pool.slots_[slot].cancellationId) {
        auto& connection = pool.slots_[slot];
        connection.abortReason = DbSlotAbortReason::kNone;
        cancellation_.arm(stopToken);
    }

    DbSlotCancellationGuard(const DbSlotCancellationGuard&) = delete;
    DbSlotCancellationGuard& operator=(const DbSlotCancellationGuard&) = delete;

    ~DbSlotCancellationGuard() {
        finish();
    }

    void finish() noexcept {
        cancellation_.reset();
    }

private:
    worker_cancellation_registration<db_cancellation_target<Pool>> cancellation_;
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

// Slots keep their existing address-stable timer registration. Only the expiry
// action is driver-specific; installation, rollback and retirement are shared.
template <typename slot_type>
void clear_db_slot_deadline(slot_type& slot) noexcept {
    slot.deadlineTimer->cancel();
    (void)slot.deadline.clear();
    if constexpr (requires { slot.deadlineContinuation; }) {
        slot.deadlineContinuation = {};
    }
}

template <typename slot_type, typename deadline_kind, auto expiry_action = &slot_type::expire_deadline>
void arm_db_slot_deadline(const WorkerHandle& worker, slot_type& slot,
    std::chrono::milliseconds timeout, deadline_kind kind) {
    static_assert(noexcept(expiry_action(slot, kind)));
    clear_db_slot_deadline(slot);
    if (timeout.count() <= 0) {
        return;
    }
    const auto deadline = workerTimerDeadlineAfter(timeout);
    slot.deadline.arm(deadline, kind);
    try {
        worker.schedule_timer(*slot.deadlineTimer, deadline,
            [&slot](WorkerTimerOutcome outcome) noexcept {
                if (outcome != WorkerTimerOutcome::kExpired) {
                    return;
                }
                const auto expired = slot.deadline.expire(std::chrono::steady_clock::now());
                if (expired.has_value()) {
                    expiry_action(slot, *expired);
                }
            });
    } catch (...) {
        slot.deadlineTimer->cancelQuietly();
        slot.deadline.reset();
        throw;
    }
}

// Taking and giving back a slot is pure lease bookkeeping: no driver is
// involved, so both pools share these. A release that names no live lease is a
// bug in the caller, not a runtime condition, and cannot be reported through a
// noexcept path.
template <typename Pool>
Task<std::size_t> acquireDbSlot(Pool& pool, ruvia::OperationTimeout timeout, StopToken stopToken) {
    const auto acquireTimeout = timeout.constrainedBy(pool.config_.acquireTimeout).remaining();
    const auto result =
        co_await pool.scheduler_.acquire(acquireTimeout, std::move(stopToken));
    switch (result.status()) {
        case PoolWaiterResult::Status::kAcquired:
            co_return result.index();
        case PoolWaiterResult::Status::kTimedOut:
            throw DbError(DbError::Code::kTimeout, pool.config_.driver,
                "database connection pool acquire timed out");
        case PoolWaiterResult::Status::kCancelled:
            throw DbError(
                DbError::Code::kCancelled, pool.config_.driver, "database operation cancelled");
        case PoolWaiterResult::Status::kClosed:
            throw DbError(
                DbError::Code::kClosing, pool.config_.driver, "database client is closing");
    }
    std::terminate();
}

template <typename Pool>
void releaseDbSlot(Pool& pool, std::size_t slot) noexcept {
    const auto status = pool.scheduler_.release(slot);
    if (status == PoolLeaseReleaseStatus::kInvalidSlot ||
        status == PoolLeaseReleaseStatus::kAlreadyReleased) {
        std::terminate();
    }
}

// Driver pools compose this worker-affine lifecycle policy. It borrows an
// address-stable backend; slot connect/close stay driver-specific, while lease
// scheduling and cancellation have one implementation.
template <typename Pool>
class db_pool_lifecycle final {
public:
    explicit db_pool_lifecycle(Pool& owner) noexcept
        : owner_(owner) {}

    db_pool_lifecycle(const db_pool_lifecycle&) = delete;
    db_pool_lifecycle& operator=(const db_pool_lifecycle&) = delete;
    db_pool_lifecycle(db_pool_lifecycle&&) = delete;
    db_pool_lifecycle& operator=(db_pool_lifecycle&&) = delete;

    [[nodiscard]] Task<void> connect() {
        auto& pool = owner_;
        const ruvia::OperationTimeout operationTimeout(std::nullopt);
        for (auto& slot : pool.slots_) {
            co_await pool.connectUnlocked(slot, operationTimeout);
        }
    }

    void closeNow() noexcept {
        auto& pool = owner_;
        (void)pool.scheduler_.close();
        for (auto& slot : pool.slots_) {
            pool.closeSlot(slot);
        }
    }

    [[nodiscard]] Task<std::size_t> acquireSlot(ruvia::OperationTimeout timeout, StopToken stopToken) {
        return acquireDbSlot(owner_, timeout, std::move(stopToken));
    }

    void releaseSlot(std::size_t slot) noexcept {
        releaseDbSlot(owner_, slot);
    }

    void cancelOperationById(std::uint64_t cancellationId) noexcept {
        auto& pool = owner_;
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
                DbError::Code::kCancelled, owner_.config_.driver, "database operation cancelled");
        }
    }

private:
    Pool& owner_;
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
    const ruvia::OperationTimeout operationTimeout(options.timeout);
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
Task<DbTransaction> beginDbTransaction(Pool& pool, std::pmr::memory_resource* resource,
    OperationOptions operationOptions,
    DbTransactionStartPlan plan) {
    const ruvia::OperationTimeout operationTimeout(operationOptions.timeout);
    const auto slotIndex = co_await pool.acquireSlot(operationTimeout, operationOptions.stopToken);
    DbSlotCancellationGuard cancellation(pool, slotIndex, operationOptions.stopToken);
    try {
        if (!plan.configure.empty()) {
            co_await pool.executeControl(pool.slots_[slotIndex], plan.configure, resource, operationTimeout);
        }
        co_await pool.executeControl(pool.slots_[slotIndex], plan.begin, resource, operationTimeout);
        co_return DbTransaction(DbPoolRef{&pool}, slotIndex, resource, std::move(operationOptions));
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
    const ruvia::OperationTimeout operationTimeout(options.timeout);
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
    const ruvia::OperationTimeout operationTimeout(options.timeout);
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

    const ruvia::OperationTimeout operationTimeout(options.timeout);
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

    const ruvia::OperationTimeout operationTimeout(options.timeout);
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
// `Pool` supplies config_, resource_ and worker_;
// it declares this a friend so the shared rule stays out of the drivers.
template <typename Pool, typename Slot>
Task<DbResolvedAddresses> resolveDbHost(
    Pool& pool, Slot& slot, ruvia::OperationTimeout deadline, std::string_view backend) {
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
        arm_db_slot_deadline(pool.worker_, slot, *remaining, Slot::DeadlineKind::kResolve);
    } else {
        clear_db_slot_deadline(slot);
    }

    DbSlotActiveWaitGuard activeResolve(slot);

    const auto port = formatDbPort(pool.config_.port, backend);
    try {
        auto completion = co_await ruvia::asyncAsio<asio::ip::tcp::resolver::results_type>(
            [&pool, &slot, &port](auto handler) mutable {
                slot.resolver.async_resolve(
                    pool.config_.host, std::string_view(port.data()), std::move(handler));
            });
        const auto resolveError = completion.errorCode();
        auto results = std::move(completion).takeResult();
        const auto afterResolve = deadline.remaining();
        const bool slotDeadlineExpired = slot.deadline.expired();
        clear_db_slot_deadline(slot);
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
        clear_db_slot_deadline(slot);
        throw;
    }
}

}  // namespace ruvia::detail
