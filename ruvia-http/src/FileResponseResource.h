#pragma once

#include "ruvia/memory/ProcessResource.h"

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::memory_resource* fileResponseResource() noexcept {
    return processResource();
}

}  // namespace ruvia::detail
