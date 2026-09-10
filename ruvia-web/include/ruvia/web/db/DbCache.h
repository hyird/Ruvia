#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <variant>

#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia {

struct DbCacheConfig final {
    RedisConfig options{};
    std::chrono::milliseconds duration{1000};
    bool alwaysEnabled{false};
    bool ignoreErrors{false};
    // Use a distinct namespace for databases sharing one Redis database.
    std::string nameSpace{"ruvia:query-result-cache"};
};

struct DbCacheOptions final {
    std::string id{};
    std::optional<std::chrono::milliseconds> milliseconds{};
};

using DbCacheSetting = std::variant<std::monostate, bool, std::chrono::milliseconds, DbCacheOptions>;

}  // namespace ruvia
