#include "ruvia/web/detail/db/DbInternal.h"

#include <exception>
#include <stdexcept>

namespace ruvia {

detail::MariaDbPool::SlotGuard::SlotGuard(MariaDbPool& client, std::size_t slot) noexcept
    : client_(&client),
      slot_(slot) {}

detail::MariaDbPool::SlotGuard::~SlotGuard() {
    if (client_ != nullptr) {
        client_->releaseSlot(slot_);
    }
}

Task<std::size_t> detail::MariaDbPool::acquireSlot() {
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

void detail::MariaDbPool::releaseSlot(std::size_t slot) noexcept {
    const auto status = scheduler_.release(slot);
    if (status == detail::PoolLeaseReleaseStatus::kInvalidSlot ||
        status == detail::PoolLeaseReleaseStatus::kAlreadyReleased) {
        std::terminate();
    }
}

}  // namespace ruvia
