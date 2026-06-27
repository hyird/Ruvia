#pragma once

#include <memory_resource>

#include "ruvia/memory/ProcessResource.h"

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::memory_resource* pmrResourceOrDefault(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? processResource() : resource;
}

}  // namespace ruvia::detail
