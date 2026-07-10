#pragma once

#include <memory_resource>

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::memory_resource* fileResponseResource() noexcept {
    return std::pmr::get_default_resource();
}

}  // namespace ruvia::detail
