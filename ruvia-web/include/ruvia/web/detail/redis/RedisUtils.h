#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/detail/HttpNumberFormat.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

inline void emplaceRedisString(std::pmr::vector<std::pmr::string>& target, std::string_view value) {
    target.emplace_back(value.data(), value.size());
}

inline void appendRedisNumber(std::pmr::string& output, std::uint64_t value) {
    appendHttpFormattedNumber(output, value, "failed to format redis number");
}

inline void appendRedisNumber(std::pmr::string& output, std::int64_t value) {
    appendHttpFormattedNumber(output, value, "failed to format redis number");
}

[[nodiscard]] inline std::pmr::string redisIntString(
    std::int64_t value,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(pmrResourceOrDefault(resource));
    appendRedisNumber(output, value);
    return output;
}

[[nodiscard]] inline std::pmr::string redisScoreString(
    double value,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(pmrResourceOrDefault(resource));
    appendHttpFormattedFiniteNumber(
        output,
        value,
        "redis sorted set score must be finite",
        "redis sorted set score is invalid");
    return output;
}

}  // namespace ruvia::detail
