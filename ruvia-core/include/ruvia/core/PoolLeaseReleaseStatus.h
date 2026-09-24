#pragma once

#include <cstdint>

namespace ruvia {
enum class PoolLeaseReleaseStatus : std::uint8_t {
    kReleased,
    kTransferredToWaiter,
    kInvalidSlot,
    kAlreadyReleased,
};
}  // namespace ruvia
