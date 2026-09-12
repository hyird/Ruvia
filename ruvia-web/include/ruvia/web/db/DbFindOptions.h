#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ruvia/web/db/DbCache.h"
#include "ruvia/web/db/DbPredicate.h"

namespace ruvia {

struct DbFindOrder final {
    std::string column{};
    DbOrderDirection direction{DbOrderDirection::kAsc};
    DbNullsOrder nulls{DbNullsOrder::kDefault};
};

struct DbFindOptions final {
    DbPredicate where{};
    std::vector<std::string> relations{};
    std::vector<DbFindOrder> order{};
    std::optional<std::uint64_t> skip{};
    std::optional<std::uint64_t> take{};
    std::optional<DbLockOptions> lock{};
    DbCacheSetting cache{};
};

}  // namespace ruvia
