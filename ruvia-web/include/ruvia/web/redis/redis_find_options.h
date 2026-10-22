#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ruvia/web/redis/redis_predicate.h"

namespace ruvia {

enum class redis_order_direction : std::uint8_t { asc,
    desc };

struct redis_find_order final {
    std::string field_{};
    redis_order_direction direction_{redis_order_direction::asc};
};

// RediSearch always pages results; this page size applies when `take_` is unset.
inline constexpr std::uint64_t redis_find_default_take = 100;

// Redis filters and pagination have no SQL relation, lock or cache options.
// With `take_` unset, find() returns every match or throws redis_error
// (command_error) when more than redis_find_default_take entities match after
// `skip_`; it never truncates silently. find_and_count() returns one page of
// that size together with the total count.
struct redis_find_options final {
    redis_predicate where_{};
    std::vector<redis_find_order> order_{};
    std::optional<std::uint64_t> skip_{};
    std::optional<std::uint64_t> take_{};
};

}  // namespace ruvia
