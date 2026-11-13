#pragma once

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace ruvia::detail {

// Ensures capacity for `required` elements before a mutation that must not
// reallocate. Growth at least doubles the current capacity because an exact
// reserve(n) (libc++) would turn a sequence of small appends into quadratic copying.
template <typename container_type>
void reserve_geometric(container_type& container, std::size_t required) {
    const auto capacity = container.capacity();
    if (required <= capacity) {
        return;
    }
    const auto max_size = container.max_size();
    if (required > max_size) {
        throw std::length_error("container capacity overflow");
    }
    const auto doubled = capacity > max_size / 2 ? max_size : capacity * 2;
    container.reserve((std::max)(required, doubled));
}

}  // namespace ruvia::detail
