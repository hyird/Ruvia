#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/Task.h"

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

}  // namespace ruvia::detail
