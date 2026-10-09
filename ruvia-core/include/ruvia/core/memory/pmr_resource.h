#pragma once

#include <memory_resource>

#include "ruvia/core/memory/process_resource.h"

namespace ruvia::detail {

struct resolved_pmr_resource_tag final {};

// Resolve a caller-supplied resource once at the ownership boundary. Runtime
// objects with no explicit resource use the process-wide pool.
[[nodiscard]] inline std::pmr::memory_resource* pmr_resource_or_default(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? process_resource() : resource;
}

}  // namespace ruvia::detail
