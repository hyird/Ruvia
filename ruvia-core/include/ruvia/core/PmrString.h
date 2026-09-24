#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

namespace ruvia {

inline void resizePmrStringForOverwrite(std::pmr::string& target, std::size_t size) {
    target.resize(size);
}

// Clear a temporary buffer, returning unusually large storage to its owner
// while retaining small allocations for repeated worker-local operations.
inline void clearPmrStringRetainingSmall(
    std::pmr::string& target, std::size_t retainedBytes = 4096) {
    target.clear();
    if (target.capacity() <= retainedBytes) {
        return;
    }
    std::pmr::string empty(target.get_allocator());
    target.swap(empty);
}

}  // namespace ruvia
