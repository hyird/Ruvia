#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/DbExecResult.h"
#include "ruvia/web/redis/RedisRepositoryTypes.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia::detail {

using RedisOrmArguments = std::pmr::vector<std::pmr::string>;

// DbExecResult deliberately keeps its constructor private. Redis command
// mappers use this narrow access point so Redis cannot manufacture an insert
// id that the backend does not provide.
struct RedisOrmResultAccess final {
    [[nodiscard]] static constexpr DbExecResult makeExecResult(
        std::uint64_t affectedRows) noexcept {
        return DbExecResult(affectedRows, std::nullopt);
    }
};

// Wire commands own their strings; views are only used during the synchronous
// handoff to RedisHandle, which normalizes them into its own worker storage.
[[nodiscard]] std::pmr::vector<std::string_view> redisOrmArgumentViews(const RedisOrmArguments& args);
[[nodiscard]] RedisOrmArguments redisOrmWriteArguments(std::string_view key, std::string_view mode,
    RedisWriteOptions options, std::pmr::memory_resource* resource);
[[nodiscard]] RedisOrmArguments redisOrmDeleteArguments(std::string_view key, std::pmr::memory_resource* resource);
[[nodiscard]] DbExecResult redisOrmExecResult(RedisValue&& reply, std::pmr::memory_resource* resource);
[[nodiscard]] DbExecResult redisOrmDeleteResult(RedisValue&& reply, std::pmr::memory_resource* resource);
[[nodiscard]] bool redisOrmBooleanResult(RedisValue&& reply, std::pmr::memory_resource* resource);
void redisOrmStatusResult(RedisValue&& reply, std::pmr::memory_resource* resource);
[[nodiscard]] std::span<const RedisValue> redisOrmArray(const RedisValue& reply);
[[nodiscard]] std::string_view redisOrmString(const RedisValue& reply);
[[nodiscard]] std::uint64_t redisOrmCount(const RedisValue& reply);

}  // namespace ruvia::detail
