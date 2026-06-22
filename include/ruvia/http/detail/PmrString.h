#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

namespace ruvia::detail {

inline constexpr std::size_t kRetainedPmrStringBytes = 4096;

inline void resizePmrStringForOverwrite(std::pmr::string& target, std::size_t size) {
    target.resize_and_overwrite(size, [](char*, std::size_t count) noexcept {
        return count;
    });
}

inline void clearPmrStringRetainingSmall(
    std::pmr::string& target,
    std::size_t retainedBytes = kRetainedPmrStringBytes) {
    target.clear();
    if (target.capacity() <= retainedBytes) {
        return;
    }

    std::pmr::string empty(target.get_allocator());
    target.swap(empty);
}

}  // namespace ruvia::detail
