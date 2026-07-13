#pragma once

#include <memory_resource>

namespace ruvia::detail {

[[nodiscard]] std::pmr::memory_resource* processResource() noexcept;

}  // namespace ruvia::detail
