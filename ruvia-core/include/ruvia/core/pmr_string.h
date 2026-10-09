#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

namespace ruvia {

inline void resize_pmr_string_for_overwrite(std::pmr::string& target, std::size_t size) {
    target.resize(size);
}

// Clear a temporary buffer, returning unusually large storage to its owner
// while retaining small allocations for repeated worker-local operations.
inline void clear_pmr_string_retaining_small(
    std::pmr::string& target, std::size_t retained_bytes = 4096) {
    target.clear();
    if (target.capacity() <= retained_bytes) {
        return;
    }
    std::pmr::string empty(target.get_allocator());
    target.swap(empty);
}

}  // namespace ruvia
