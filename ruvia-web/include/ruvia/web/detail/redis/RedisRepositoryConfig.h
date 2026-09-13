#pragma once
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/detail/redis/RedisEntityCodec.h"
#include "ruvia/web/redis/RedisRepositoryTypes.h"
namespace ruvia::detail {
struct RedisIndexDefinition final {
    std::pmr::string column;
    RedisIndexKind kind{RedisIndexKind::kNone};
    bool sortable{false};
};
struct RedisMapping final {
    std::pmr::string prefix;
    std::pmr::vector<RedisIndexDefinition> indexes;
    [[nodiscard]] RedisIndexKind indexKind(std::string_view column) const noexcept {
        for (const auto& index : indexes) {
            if (index.column == column) {
                return index.kind;
            }
        }
        return RedisIndexKind::kNone;
    }
    [[nodiscard]] bool sortable(std::string_view column) const noexcept {
        for (const auto& index : indexes) {
            if (index.column == column) {
                return index.sortable;
            }
        }
        return false;
    }
};
template <typename Entity>
RedisMapping normalizeRedisMapping(const RedisRepositoryConfig& config, std::pmr::memory_resource* resource) {
    validateRedisEntity<Entity>();
    RedisMapping result{std::pmr::string(config.prefix.empty() ? Entity::tableName() : std::string_view(config.prefix), resource), std::pmr::vector<RedisIndexDefinition>(resource)};
    if (result.prefix.empty()) {
        throw std::invalid_argument("Redis entity prefix cannot be empty");
    }
    forEachRedisField<Entity>([]<typename Field> {
        if (Field::name.view().starts_with("__ruvia_")) {
            throw std::invalid_argument("Redis entity column uses reserved __ruvia_ namespace");
        }
    });
    for (const auto& index : config.indexes) {
        for (const auto& existing : result.indexes) {
            if (std::string_view(existing.column) == std::string_view(index.column)) {
                throw std::invalid_argument("duplicate Redis index column");
            }
        }
        bool found = false;
        forEachRedisField<Entity>([&]<typename Field> {
            if (Field::name.view() != index.column) {
                return;
            }
            found = true;
            using T = typename Field::value_type;
            switch (index.kind) {
                case RedisIndexKind::kTag:
                    if constexpr (!(isRedisEntityString<T> || std::is_same_v<T, bool> || std::is_same_v<T, Bool>)) {
                        throw std::invalid_argument("Redis TAG indexes require string or boolean columns");
                    }
                    break;
                case RedisIndexKind::kText:
                    if constexpr (!isRedisEntityString<T>) {
                        throw std::invalid_argument("Redis TEXT indexes require string columns");
                    }
                    break;
                case RedisIndexKind::kNumeric:
                    if constexpr (!isRedisEntityNumber<T>) {
                        throw std::invalid_argument("Redis NUMERIC indexes require numeric columns");
                    }
                    break;
                default:
                    throw std::invalid_argument("Redis index kind must be TAG, TEXT or NUMERIC");
            }
        });
        if (!found) {
            throw std::invalid_argument("unknown Redis index column");
        }
        result.indexes.push_back({std::pmr::string(index.column, resource), index.kind, index.sortable});
    }
    return result;
}
}  // namespace ruvia::detail
