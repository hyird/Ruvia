#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/detail/NumberFormat.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::memory_resource* resolveRedisResource(
    std::pmr::memory_resource* resource) noexcept {
    return pmrResourceOrDefault(resource);
}

inline void emplaceRedisString(std::pmr::vector<std::pmr::string>& target, std::string_view value) {
    target.emplace_back();
    target.back().assign(value.data(), value.size());
}

inline void appendRedisNumber(std::pmr::string& output, std::uint64_t value) {
    appendFormattedNumber(output, value, "failed to format redis number");
}

inline void appendRedisNumber(std::pmr::string& output, std::int64_t value) {
    appendFormattedNumber(output, value, "failed to format redis number");
}

[[nodiscard]] inline std::pmr::string redisIntString(
    std::int64_t value,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(resolveRedisResource(resource));
    appendRedisNumber(output, value);
    return output;
}

[[nodiscard]] inline std::pmr::string redisScoreString(
    double value,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(resolveRedisResource(resource));
    appendFormattedFiniteNumber(
        output,
        value,
        "redis sorted set score must be finite",
        "redis sorted set score is invalid");
    return output;
}

}  // namespace ruvia::detail
