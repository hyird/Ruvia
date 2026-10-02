#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <variant>

namespace ruvia {

struct DbCacheConfig final {
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

// App startup resolves this alias to its existing worker-local Redis capability.
struct db_query_cache_registration final {
    std::string redis_alias{"default"};
    DbCacheConfig policy{};
};

}  // namespace ruvia
