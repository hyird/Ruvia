#pragma once

#include <memory_resource>
#include <utility>

namespace ruvia::detail {

// Equivalent resources permit cross-deallocation but do not share lifetimes.
// Transfer storage only when every member retains the exact target resource;
// otherwise prepare allocator-aware copies before changing the source.
template <typename container_type, typename... peer_types>
[[nodiscard]] container_type rebind_owned_storage(container_type& source,
    std::pmr::memory_resource* target, const peer_types&... peers) {
    if (source.get_allocator().resource() == target &&
        ((peers.get_allocator().resource() == target) && ...)) {
        return container_type(std::move(source));
    }
    return container_type(source, target);
}

}  // namespace ruvia::detail
