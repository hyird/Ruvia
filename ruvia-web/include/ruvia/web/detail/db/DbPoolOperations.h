#pragma once

#include <array>
#include <charconv>
#include <exception>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/io/OperationDeadline.h"
#include "ruvia/core/detail/pool/PoolLeaseScheduler.h"
#include "ruvia/core/detail/worker/WorkerTimer.h"
#include "ruvia/web/db/DbQueryResult.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbHostResolution.h"

// The parts of a pooled database operation that do not depend on the driver:
// which slot it runs on, and what happens to that slot when it fails.

namespace ruvia::detail {

// A pool slot held for the duration of one operation. Releasing it is the
// caller's obligation however the operation ends, so the guard owns that: the
// driver code between acquire and release can throw freely.
template <typename Pool>
class DbSlotGuard final {
public:
    DbSlotGuard(Pool& pool, std::size_t slot) noexcept
        : pool_(&pool), slot_(slot) {}
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
Task<std::size_t> acquireDbSlot(Pool& pool) {
    const auto result = co_await pool.scheduler_.acquire(pool.config_.acquireTimeout);
    if (const auto* acquired = result.acquired()) {
        co_return acquired->index();
    }
    if (result.timedOut() != nullptr) {
        throw std::runtime_error("database connection pool acquire timed out");
    }
    throw std::runtime_error("database client is closing");
}

template <typename Pool>
void releaseDbSlot(Pool& pool, std::size_t slot) noexcept {
    const auto status = pool.scheduler_.release(slot);
    if (status == PoolLeaseReleaseStatus::kInvalidSlot ||
        status == PoolLeaseReleaseStatus::kAlreadyReleased) {
        std::terminate();
    }
}

// Whether any operation on this pool can time out, and therefore whether the
// pool needs the deadline scanner running at all. Every timeout DbConfig can
// carry is listed here, so adding one is a single edit.
[[nodiscard]] inline bool dbConfigHasAnyTimeout(const DbConfig& config) noexcept {
    return config.connectTimeout.has_value() ||
        config.queryTimeout.has_value() ||
        config.readTimeout.has_value() ||
        config.writeTimeout.has_value() ||
        config.acquireTimeout.has_value();
}

// Ending a transaction is the same for every driver: run the control statement
// on the slot the transaction holds, and release that slot exactly once. A
// failure closes the connection before releasing, because a slot whose COMMIT
// or ROLLBACK did not complete cannot be handed to the next caller.
//
// `Pool` supplies slots_, executeControl(), closeSlot() and releaseSlot(); it
// declares this a friend so the shared rule stays out of the drivers.
template <typename Pool>
Task<void> finishDbTransaction(
    Pool& pool,
    std::size_t slot,
    std::string_view command,
    std::pmr::memory_resource* resource) {
    if (slot >= pool.slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    try {
        co_await pool.executeControl(pool.slots_[slot], command, resource);
    } catch (...) {
        pool.closeSlot(pool.slots_[slot]);
        pool.releaseSlot(slot);
        throw;
    }
    pool.releaseSlot(slot);
}


// One buffered statement on a pooled connection. The shape is the same for every
// driver: refuse empty SQL before taking a slot, hold the slot for the duration,
// and close the connection if the statement throws -- a slot whose statement
// failed mid-protocol cannot be reused. The guard releases the slot either way.
template <typename Pool>
Task<QueryResult> executeDbQuery(
    Pool& pool,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }

    const auto slotIndex = co_await pool.acquireSlot();
    typename Pool::SlotGuard guard(pool, slotIndex);
    try {
        co_return co_await pool.executeOnSlot(
            pool.slots_[slotIndex],
            sql,
            std::span<const DbValue>(params),
            resource);
    } catch (...) {
        pool.closeSlot(pool.slots_[slotIndex]);
        throw;
    }
}


// The pool's configured port as a NUL-terminated buffer, the form asio's
// resolver takes it in.
[[nodiscard]] inline std::array<char, 6> formatDbPort(
    std::uint16_t port,
    std::string_view backend) {
    std::array<char, 6> output{};
    const auto parsed = std::to_chars(
        output.data(), output.data() + output.size() - 1, port);
    if (parsed.ec != std::errc{}) {
        throw std::runtime_error(
            std::string("failed to format ").append(backend).append(" port"));
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
    Pool& pool,
    Slot& slot,
    const OperationTimeout& deadline,
    std::string_view backend) {
    const auto timedOut = [backend] {
        return std::runtime_error(
            std::string(backend).append(" host resolve timed out"));
    };

    const auto remaining = deadline.remaining();
    if (remaining.has_value() && remaining->count() <= 0) {
        throw timedOut();
    }
    if (remaining.has_value()) {
        slot.deadline.arm(
            workerTimerDeadlineAfter(*remaining),
            Slot::DeadlineKind::kResolve);
    } else {
        slot.deadline.reset();
    }

    const auto port = formatDbPort(pool.config_.port, backend);
    try {
        auto completion =
            co_await asyncAsio<asio::ip::tcp::resolver::results_type>(
                [&pool, &slot, &port](auto handler) mutable {
                    slot.resolver.async_resolve(
                        pool.config_.host,
                        std::string_view(port.data()),
                        std::move(handler));
                });
        const auto resolveError = completion.errorCode();
        auto results = std::move(completion).takeResult();
        const auto afterResolve = deadline.remaining();
        if (slot.deadline.clear() ||
            (afterResolve.has_value() && afterResolve->count() <= 0)) {
            throw timedOut();
        }
        if (resolveError) {
            throw std::system_error(
                resolveError,
                std::string("resolving ").append(backend).append(" host failed"));
        }
        co_return collectDbResolvedAddresses(results, pool.resource_);
    } catch (...) {
        pool.clearSlotDeadline(slot);
        throw;
    }
}

}  // namespace ruvia::detail
