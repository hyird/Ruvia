#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

namespace ruvia::detail {

inline void resizePmrStringForOverwrite(std::pmr::string& target, std::size_t size) {
    target.resize_and_overwrite(size, [](char*, std::size_t count) noexcept {
        return count;
    });
}

}  // namespace ruvia::detail
