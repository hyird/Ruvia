#pragma once

#include <memory_resource>

namespace ruvia::detail {

struct HttpResolvedPmrResourceTag final {};

[[nodiscard]] inline std::pmr::memory_resource* httpPmrResourceOrDefault(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

}  // namespace ruvia::detail
