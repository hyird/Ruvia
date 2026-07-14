#include "ruvia/web/detail/db/DbInternal.h"

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
    return scheduler_.acquire(config_.acquireTimeout);
}

void detail::MariaDbPool::releaseSlot(std::size_t slot) noexcept {
    scheduler_.release(slot);
}

}  // namespace ruvia
