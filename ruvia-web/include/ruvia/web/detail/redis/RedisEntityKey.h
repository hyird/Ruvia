#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/redis/RedisEntityCodec.h"

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::string redisEntityStoragePrefix(
    std::string_view prefix, std::pmr::memory_resource* resource = nullptr) {
    resource = pmrResourceOrDefault(resource);
    constexpr std::string_view root = "ruvia:orm:";
    constexpr char hex[] = "0123456789abcdef";
    std::pmr::string result(root, resource);
    result.reserve(root.size() + prefix.size() * 2U + 1U);
    for (const unsigned char byte : prefix) {
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0fU]);
    }
    result.push_back(':');
    return result;
}

template <typename Entity>
[[nodiscard]] inline std::pmr::string redisEntityStoragePrefix(
    std::string_view prefix, std::pmr::memory_resource* resource = nullptr) {
    (void)sizeof(Entity);
    return redisEntityStoragePrefix(prefix, resource);
}

template <typename Entity>
[[nodiscard]] inline std::pmr::string redisEntityStoragePrefix(
    std::pmr::memory_resource* resource = nullptr) {
    return redisEntityStoragePrefix(Entity::tableName(), resource);
}

template <typename Entity>
[[nodiscard]] inline std::pmr::string redisEntityKey(std::string_view id,
    std::pmr::memory_resource* resource, std::string_view prefix = {}) {
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    auto key = redisEntityStoragePrefix(
        prefix.empty() ? Entity::tableName() : prefix, resource);
    key.append(id.data(), id.size());
    return key;
}

template <typename Entity>
[[nodiscard]] std::pmr::string redisEntityId(
    const Entity& entity, std::pmr::memory_resource* resource) {
    validateRedisEntity<Entity>();
    std::pmr::string id(pmrResourceOrDefault(resource));
    forEachRedisField<Entity>([&]<typename Field> {
        if constexpr (Field::options.id) {
            if (!entity.template isSet<Field::name>() || entity.template isNull<Field::name>()) {
                throw std::invalid_argument("Redis entity requires an ID");
            }
            id = encodeRedisScalar(entity.template get<Field::name>(), resource);
        }
    });
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    return id;
}

}  // namespace ruvia::detail
