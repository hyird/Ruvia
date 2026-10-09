#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ruvia/web/db/db_cache.h"
#include "ruvia/web/db/db_predicate.h"

namespace ruvia {

struct db_find_order final {
    std::string column_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};

struct db_find_options final {
    db_predicate where_{};
    std::vector<std::string> relations_{};
    std::vector<db_find_order> order_{};
    std::optional<std::uint64_t> skip_{};
    std::optional<std::uint64_t> take_{};
    std::optional<db_lock_options> lock_{};
    db_cache_setting_type cache_{};
};

}  // namespace ruvia
