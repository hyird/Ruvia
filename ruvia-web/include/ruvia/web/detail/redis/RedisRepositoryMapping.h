#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/detail/redis/RedisEntityCodec.h"
#include "ruvia/web/detail/redis/RedisEntityKey.h"
#include "ruvia/web/detail/redis/RedisRepositoryCommands.h"

namespace ruvia::detail {

template <typename Entity>
Entity decodeRedisEntity(const RedisValue& reply, std::string_view key,
    std::pmr::memory_resource* resource, std::string_view prefix = {}) {
    const auto fields = redisOrmArray(reply);
    if (fields.size() % 2 != 0) {
        throw RedisError(RedisError::Code::kProtocolError, "invalid entity hash reply");
    }
    bool marker = false;
    for (std::size_t i = 0; i < fields.size(); i += 2) {
        const auto name = redisOrmString(fields[i]);
        (void)redisOrmString(fields[i + 1]);
        if (name == "__ruvia_entity") {
            if (marker || redisOrmString(fields[i + 1]) != "1") {
                throw RedisError(RedisError::Code::kProtocolError, "invalid entity storage marker");
            }
            marker = true;
        }
    }
    if (!marker) {
        throw RedisError(RedisError::Code::kProtocolError, "entity storage marker missing");
    }
    Entity entity(resource);
    forEachRedisField<Entity>([&]<typename Field> {
        std::optional<std::string_view> value;
        for (std::size_t i = 0; i < fields.size(); i += 2) {
            if (redisOrmString(fields[i]) == Field::name.view()) {
                if (value) {
                    throw RedisError(RedisError::Code::kProtocolError, "duplicate entity field");
                }
                value = redisOrmString(fields[i + 1]);
            }
        }
        if (value) {
            entity.template set<Field::name>(decodeRedisScalar<typename Field::value_type>(*value, resource));
        } else if constexpr (Field::options.nullable) {
            entity.template setNull<Field::name>();
        } else {
            throw RedisError(RedisError::Code::kProtocolError, "required entity field missing from hash");
        }
    });
    try {
        const auto id = redisEntityId(entity, resource);
        if (redisEntityKey<Entity>(id, resource, prefix) != key) {
            throw RedisError(RedisError::Code::kProtocolError, "entity ID does not match its Redis key");
        }
    } catch (const std::invalid_argument&) {
        throw RedisError(RedisError::Code::kProtocolError, "invalid stored entity ID");
    }
    return entity;
}

template <typename Entity>
struct RedisMapOne final {
    std::pmr::string key;
    std::pmr::string prefix;
    std::optional<Entity> operator()(RedisValue&& reply, std::pmr::memory_resource* resource) const {
        if (redisOrmArray(reply).empty()) {
            return std::nullopt;
        }
        return decodeRedisEntity<Entity>(reply, key, resource, prefix);
    }
};

template <typename Entity>
struct RedisMapSearch final {
    std::pmr::string prefix;
    std::pair<DbEntityRows<Entity>, std::uint64_t> operator()(
        RedisValue&& reply, std::pmr::memory_resource* resource) const {
        const auto values = redisOrmArray(reply);
        if (values.empty() || values.size() % 2 != 1) {
            throw RedisError(RedisError::Code::kProtocolError, "invalid entity search reply");
        }
        const auto count = redisOrmCount(values.front());
        if (count < (values.size() - 1) / 2) {
            throw RedisError(RedisError::Code::kProtocolError, "invalid entity search count");
        }
        DbEntityRows<Entity> rows(resource);
        for (std::size_t i = 1; i < values.size(); i += 2) {
            const auto key = redisOrmString(values[i]);
            // A document may expire between index matching and field loading.
            if (values[i + 1].null()) {
                continue;
            }
            rows.push_back(decodeRedisEntity<Entity>(values[i + 1], key, resource, prefix));
        }
        return {std::move(rows), count};
    }
};

template <typename Entity>
struct RedisMapMany final {
    std::pmr::string prefix;
    DbEntityRows<Entity> operator()(
        RedisValue&& reply, std::pmr::memory_resource* resource) const {
        return RedisMapSearch<Entity>{std::pmr::string(prefix, resource)}(
            std::move(reply), resource)
            .first;
    }
};

template <typename Entity>
struct RedisMapFirst final {
    std::pmr::string prefix;
    std::optional<Entity> operator()(RedisValue&& reply, std::pmr::memory_resource* resource) const {
        auto result = RedisMapSearch<Entity>{std::pmr::string(prefix, resource)}(
            std::move(reply), resource);
        if (result.first.empty()) {
            return std::nullopt;
        }
        if (result.first.size() != 1) {
            throw RedisError(RedisError::Code::kProtocolError, "multiple entities in findOne reply");
        }
        return std::move(result.first[0]);
    }
};

struct RedisMapCount final {
    std::uint64_t operator()(RedisValue&& reply, std::pmr::memory_resource*) const {
        const auto values = redisOrmArray(reply);
        if (values.size() != 1) {
            throw RedisError(RedisError::Code::kProtocolError, "invalid entity count reply");
        }
        return redisOrmCount(values.front());
    }
};

}  // namespace ruvia::detail
