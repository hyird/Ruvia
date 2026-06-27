#pragma once

#include <memory_resource>

namespace ruvia::detail {

[[nodiscard]] std::pmr::memory_resource* registrationResource() noexcept;

}  // namespace ruvia::detail
