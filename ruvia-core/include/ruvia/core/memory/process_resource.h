#pragma once

#include <memory_resource>

namespace ruvia::detail {

[[nodiscard]] std::pmr::memory_resource* process_resource() noexcept;

}  // namespace ruvia::detail
