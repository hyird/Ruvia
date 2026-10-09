#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia::detail {

struct redis_owned_command final {
    std::pmr::vector<std::pmr::string> args_;
};

[[nodiscard]] redis_owned_command make_owned_redis_command(
    std::pmr::memory_resource* resource, std::span<const std::string_view> args);
[[nodiscard]] redis_owned_command make_owned_redis_command(std::pmr::memory_resource* resource,
    std::string_view first, std::span<const std::string_view> rest = {});

}  // namespace ruvia::detail
