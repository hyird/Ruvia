#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/number_format.h"

namespace ruvia::detail {

inline void emplace_redis_string(std::pmr::vector<std::pmr::string>& target, std::string_view value) {
    target.emplace_back(value.data(), value.size());
}

inline void append_redis_number(std::pmr::string& output, std::uint64_t value) {
    append_formatted_number(output, value, "failed to format redis number");
}

inline void append_redis_number(std::pmr::string& output, std::int64_t value) {
    append_formatted_number(output, value, "failed to format redis number");
}

[[nodiscard]] inline std::pmr::string redis_int_string(
    std::int64_t value, std::pmr::memory_resource* resource) {
    std::pmr::string output(pmr_resource_or_default(resource));
    append_redis_number(output, value);
    return output;
}

// Redis accepts "+inf"/"-inf" as sorted-set scores and range bounds, and
// replies them back, so the client round-trips infinities. NaN has no Redis
// spelling and is rejected.
[[nodiscard]] inline std::pmr::string redis_score_string(
    double value, std::pmr::memory_resource* resource) {
    std::pmr::string output(pmr_resource_or_default(resource));
    if (std::isinf(value)) {
        output.append(value > 0 ? "+inf" : "-inf");
        return output;
    }
    append_formatted_finite_number(output, value, "redis sorted set score must not be NaN",
        "redis sorted set score is invalid");
    return output;
}

}  // namespace ruvia::detail
