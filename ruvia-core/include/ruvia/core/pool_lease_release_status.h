#pragma once

#include <cstdint>

namespace ruvia {
enum class pool_lease_release_status : std::uint8_t {
    released,
    transferred_to_waiter,
    invalid_slot,
    already_released,
};
}  // namespace ruvia
