#pragma once

#include <memory_resource>
namespace ruvia::detail {

[[nodiscard]] std::pmr::memory_resource* registration_resource() noexcept;

}  // namespace ruvia::detail
