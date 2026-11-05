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

namespace ruvia::detail {

inline void emplace_redis_string(std::pmr::vector<std::pmr::string>& target, std::string_view value) {
    target.emplace_back(value.data(), value.size());
}

inline constexpr std::size_t redis_int_scratch_size = 20;
inline constexpr std::size_t redis_score_scratch_size = 64;

[[nodiscard]] inline std::string_view format_redis_int(
    std::span<char, redis_int_scratch_size> scratch, std::int64_t value) noexcept {
    const auto end = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value).ptr;
    return {scratch.data(), static_cast<std::size_t>(end - scratch.data())};
}

[[nodiscard]] inline std::string_view format_redis_score(
    std::span<char, redis_score_scratch_size> scratch, double value) {
    if (std::isinf(value)) {
        return value > 0 ? "+inf" : "-inf";
    }
    if (!std::isfinite(value)) {
        throw std::invalid_argument("redis sorted set score must not be NaN");
    }
    const auto [ptr, ec] = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("redis sorted set score is invalid");
    }
    return {scratch.data(), static_cast<std::size_t>(ptr - scratch.data())};
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
    http_append_formatted_finite_number(output, value, "redis sorted set score must not be NaN",
        "redis sorted set score is invalid");
    return output;
}

}  // namespace ruvia::detail
