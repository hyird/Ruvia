#pragma once

#include <memory_resource>

namespace ruvia {

[[nodiscard]] std::pmr::memory_resource* process_resource() noexcept;

}  // namespace ruvia
