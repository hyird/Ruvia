#pragma once

#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::memory_resource* appResource() noexcept {
    return processResource();
}

}  // namespace ruvia::detail
