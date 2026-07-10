#pragma once

#include <memory_resource>

#include "ruvia/memory/ProcessResource.h"

namespace ruvia::detail {

struct ResolvedPmrResourceTag final {};

// Resolve a caller-supplied resource once at the ownership boundary. Runtime
// objects with no explicit resource use the process-wide mimalloc-backed pool.
[[nodiscard]] inline std::pmr::memory_resource* pmrResourceOrDefault(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? processResource() : resource;
}

}  // namespace ruvia::detail
