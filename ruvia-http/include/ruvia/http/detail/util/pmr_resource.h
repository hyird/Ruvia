#pragma once

#include <memory_resource>

namespace ruvia::detail {

struct http_resolved_pmr_resource_tag final {};

// The HTTP target stays independent of core, so its null-resource fallback is
// std::pmr::get_default_resource() rather than core's process resource.
[[nodiscard]] inline std::pmr::memory_resource* http_pmr_resource_or_default(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

}  // namespace ruvia::detail
