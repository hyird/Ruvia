#pragma once

#include <memory_resource>

namespace ruvia::detail {

[[nodiscard]] std::pmr::memory_resource* registrationResource() noexcept;

[[nodiscard]] inline std::pmr::memory_resource* registrationResourceOrDefault(std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? registrationResource() : resource;
}

}  // namespace ruvia::detail
