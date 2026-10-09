#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ruvia/web/redis/redis_predicate.h"

namespace ruvia {

enum class redis_order_direction : std::uint8_t { ascending,
    descending };

struct redis_find_order final {
    std::string field_{};
    redis_order_direction direction_{redis_order_direction::ascending};
};

// Redis filters and pagination have no SQL relation, lock or cache options.
struct redis_find_options final {
    redis_predicate where_{};
    std::vector<redis_find_order> order_{};
    std::optional<std::uint64_t> skip_{};
    std::optional<std::uint64_t> take_{};
};

}  // namespace ruvia
