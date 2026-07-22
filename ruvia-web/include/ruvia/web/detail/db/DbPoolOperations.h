#pragma once

#include <memory_resource>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/web/db/DbQueryResult.h"
#include "ruvia/web/db/DbTypes.h"

// The parts of a pooled database operation that do not depend on the driver:
// which slot it runs on, and what happens to that slot when it fails.

namespace ruvia::detail {

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

}  // namespace ruvia::detail
