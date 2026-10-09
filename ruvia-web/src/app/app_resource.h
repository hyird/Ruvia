#pragma once

#include "ruvia/core/memory/process_resource.h"

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::memory_resource* app_resource() noexcept {
    return process_resource();
}

}  // namespace ruvia::detail
