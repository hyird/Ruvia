#pragma once

#include <memory_resource>

#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::memory_resource* fileResponseResource() noexcept {
    return processResource();
}

}  // namespace ruvia::detail
