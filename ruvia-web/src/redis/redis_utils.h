#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/detail/util/finite_double.h"

namespace ruvia::detail {

inline void emplace_redis_string(std::pmr::vector<std::pmr::string>& target, std::string_view value) {
    target.emplace_back(value.data(), value.size());
}

inline void append_redis_number(std::pmr::string& output, std::uint64_t value) {
    http_append_unsigned(output, value);
}

inline void append_redis_number(std::pmr::string& output, std::int64_t value) {
    http_append_signed(output, value);
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
    append_finite_double(output, value, "redis sorted set score must not be NaN");
    return output;
}

}  // namespace ruvia::detail
