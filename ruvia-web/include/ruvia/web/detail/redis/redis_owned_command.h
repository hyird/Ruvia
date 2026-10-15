#pragma once

#include <cstdint>
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

// Shared typed command layouts for direct execution, pipelines and transactions.
[[nodiscard]] redis_owned_command make_redis_integer_command(std::string_view command,
    std::string_view key, std::int64_t value, std::pmr::memory_resource* resource);
[[nodiscard]] redis_owned_command make_redis_range_command(std::string_view command,
    std::string_view key, std::int64_t start, std::int64_t stop, std::pmr::memory_resource* resource,
    bool with_scores = false);
[[nodiscard]] redis_owned_command make_redis_zadd_command(std::string_view key, double score,
    std::string_view member, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
