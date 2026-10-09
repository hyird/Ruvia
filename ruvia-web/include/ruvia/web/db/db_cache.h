#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <variant>

namespace ruvia {

struct db_cache_config final {
    std::chrono::milliseconds duration_{1000};
    bool always_enabled_{false};
    bool ignore_errors_{false};
    // Use a distinct namespace for databases sharing one Redis database.
    std::string name_space_{"ruvia:query-result-cache"};
};

struct db_cache_options final {
    std::string id_{};
    std::optional<std::chrono::milliseconds> milliseconds_{};
};

using db_cache_setting_type = std::variant<std::monostate, bool, std::chrono::milliseconds, db_cache_options>;

// application startup resolves this alias to its existing worker-local Redis capability.
struct db_query_cache_registration final {
    std::string redis_alias_{"default"};
    db_cache_config policy_{};
};

}  // namespace ruvia
