#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

namespace ruvia {

inline constexpr std::size_t http_retained_pmr_string_bytes = 4096;

// Clear a temporary buffer, returning unusually large storage to its owner
// while retaining small allocations for repeated worker-local operations.
inline void http_clear_pmr_string_retaining_small(
    std::pmr::string& target, std::size_t retained_bytes = http_retained_pmr_string_bytes) {
    target.clear();
    if (target.capacity() <= retained_bytes) {
        return;
    }
    std::pmr::string empty(target.get_allocator());
    target.swap(empty);
}

}  // namespace ruvia
