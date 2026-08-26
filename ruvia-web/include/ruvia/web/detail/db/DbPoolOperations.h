#pragma once

#include <array>
#include <charconv>
#include <exception>
#include <memory>
#include <memory_resource>
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
#include "ruvia/core/detail/worker/WorkerTimer.h"
#include "ruvia/core/memory/PmrResource.h"
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

// One state object is allocated per pool slot at startup. Stop callbacks retain
// this stable bridge, not the pool itself, and stale generations become no-ops.
// The owner pointer is read and cleared only on the bound worker; once that
// worker is detached no queued cancellation continuation can run.
class DbOperationCancellationState final
    : public std::enable_shared_from_this<DbOperationCancellationState> {
public:
    using Cancel = void (*)(void*, std::size_t, std::uint64_t) noexcept;

    DbOperationCancellationState(
        WorkerHandle worker, void* owner, std::size_t slot, Cancel cancel) noexcept
        : worker_(std::move(worker)),
          owner_(owner),
          slot_(slot),
          cancel_(cancel) {}

    void request(std::uint64_t generation) noexcept {
        if (worker_.isCurrent()) {
            dispatch(generation);
            return;
        }
        (void)WorkerHandleAccess::deferIfAttached(
            worker_, [state = shared_from_this(), generation] { state->dispatch(generation); });
    }

    void detach(void* owner) noexcept {
        if (owner_ == owner) {
            owner_ = nullptr;
        }
    }

private:
    void dispatch(std::uint64_t generation) noexcept {
        if (owner_ != nullptr) {
            cancel_(owner_, slot_, generation);
        }
    }

    WorkerHandle worker_;
    void* owner_;
    std::size_t slot_;
    Cancel cancel_;
};

template <typename Pool>
[[nodiscard]] std::shared_ptr<DbOperationCancellationState> makeDbOperationCancellationState(
    const WorkerHandle& worker, Pool& pool, std::size_t slot) {
    return std::allocate_shared<DbOperationCancellationState>(
        std::pmr::polymorphic_allocator<DbOperationCancellationState>(processResource()), worker,
        &pool, slot, [](void* owner, std::size_t index, std::uint64_t generation) noexcept {
            static_cast<Pool*>(owner)->cancelOperation(index, generation);
        });
}

template <typename Slot>
[[nodiscard]] std::uint64_t beginDbSlotOperation(Slot& slot) noexcept {
    slot.abortReason = DbSlotAbortReason::kNone;
    if (++slot.operationGeneration == 0) {
        ++slot.operationGeneration;
    }
    return slot.operationGeneration;
}

template <typename Slot>
void finishDbSlotOperation(Slot& slot, std::uint64_t generation) noexcept {
    if (slot.operationGeneration == generation) {
        if (++slot.operationGeneration == 0) {
            ++slot.operationGeneration;
        }
    }
}

template <typename Pool>
class DbSlotCancellationGuard final {
public:
    DbSlotCancellationGuard(Pool& pool, std::size_t slot, const StopToken& stopToken)
        : pool_(&pool),
          slot_(slot),
          generation_(beginDbSlotOperation(pool.slots_[slot])),
          state_(pool.slots_[slot].cancellationState.get()) {
        if (stopToken.stoppable() && state_ == nullptr) {
            throw std::logic_error("cancellable database operation requires a valid worker");
        }
        stopToken.registerCallback(stopRegistration_,
            [state = state_, generation = generation_]() noexcept { state->request(generation); });
        if (stopToken.stopRequested()) {
            pool_->cancelOperation(slot_, generation_);
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
            finishDbSlotOperation(pool_->slots_[slot_], generation_);
            pool_ = nullptr;
        }
    }

private:
    Pool* pool_;
    std::size_t slot_;
    std::uint64_t generation_;
    DbOperationCancellationState* state_;
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

// Taking and giving back a slot is pure lease bookkeeping: no driver is
// involved, so both pools share these. A release that names no live lease is a
// bug in the caller, not a runtime condition, and cannot be reported through a
// noexcept path.
template <typename Pool>
Task<std::size_t> acquireDbSlot(Pool& pool, const OperationTimeout& timeout, StopToken stopToken) {
    if (stopToken.stoppable() && !pool.worker_.valid()) {
        throw std::logic_error("cancellable database operation requires a valid worker");
    }
    const auto acquireTimeout = timeout.constrainedBy(pool.config_.acquireTimeout).remaining();
    const auto result = pool.worker_.valid() ? co_await pool.scheduler_.acquire(acquireTimeout,
                                                   std::move(stopToken), pool.worker_)
                                             : co_await pool.scheduler_.acquire(acquireTimeout);
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
// scanner may have fired while this coroutine was suspended.
//
// Only the backend's name in the diagnostics differs, so it is the argument.
// `Pool` supplies config_, resource_ and clearSlotDeadline(); it declares this
// a friend so the shared rule stays out of the drivers.
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
        slot.deadline.arm(workerTimerDeadlineAfter(*remaining), Slot::DeadlineKind::kResolve);
    } else {
        slot.deadline.reset();
    }

    struct ActiveResolve final {
        explicit ActiveResolve(Slot& value) noexcept
            : slot(value) {
            if (slot.waitActive) {
                std::terminate();
            }
            slot.waitActive = true;
        }

        ~ActiveResolve() {
            slot.waitActive = false;
        }

        Slot& slot;
    } activeResolve(slot);

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
        const bool deadlineExpired =
            slot.deadline.clear() || (afterResolve.has_value() && afterResolve->count() <= 0);
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
