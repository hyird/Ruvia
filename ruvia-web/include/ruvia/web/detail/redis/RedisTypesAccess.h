#pragma once

#include "ruvia/web/redis/RedisTypes.h"

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <utility>
#include <vector>

namespace ruvia::detail {

struct RedisTypesAccess final {
    [[nodiscard]] static RedisKeyValue keyValue(std::string_view key, std::string_view value, std::pmr::memory_resource* resource) {
        return RedisKeyValue(key, value, resource);
    }

    [[nodiscard]] static RedisScoredValue scoredValue(std::string_view value, double score, std::pmr::memory_resource* resource) {
        return RedisScoredValue(value, score, resource);
    }

    [[nodiscard]] static RedisScanResult scanResult(std::pmr::memory_resource* resource) {
        return RedisScanResult(resource);
    }

    [[nodiscard]] static std::uint64_t& cursor(RedisScanResult& result) noexcept {
        return result.cursor_;
    }

    [[nodiscard]] static std::pmr::vector<std::pmr::string>& values(RedisScanResult& result) noexcept {
        return result.values_;
    }

    [[nodiscard]] static RedisHashScanResult hashScanResult(std::pmr::memory_resource* resource) {
        return RedisHashScanResult(resource);
    }

    [[nodiscard]] static std::uint64_t& cursor(RedisHashScanResult& result) noexcept {
        return result.cursor_;
    }

    [[nodiscard]] static std::pmr::vector<RedisKeyValue>& entries(RedisHashScanResult& result) noexcept {
        return result.entries_;
    }

    [[nodiscard]] static RedisZScanResult zScanResult(std::pmr::memory_resource* resource) {
        return RedisZScanResult(resource);
    }

    [[nodiscard]] static std::uint64_t& cursor(RedisZScanResult& result) noexcept {
        return result.cursor_;
    }

    [[nodiscard]] static std::pmr::vector<RedisScoredValue>& entries(RedisZScanResult& result) noexcept {
        return result.entries_;
    }

    [[nodiscard]] static RedisValue nullValue(std::pmr::memory_resource* resource) {
        return RedisValue::nullValue(resource);
    }

    [[nodiscard]] static RedisValue stringValue(std::string_view value, std::pmr::memory_resource* resource) {
        return RedisValue::stringValue(value, resource);
    }

    [[nodiscard]] static RedisValue errorValue(std::string_view value, std::pmr::memory_resource* resource) {
        return RedisValue::errorValue(value, resource);
    }

    [[nodiscard]] static RedisValue integerValue(std::int64_t value, std::pmr::memory_resource* resource) {
        return RedisValue::integerValue(value, resource);
    }

    [[nodiscard]] static RedisValue arrayValue(std::pmr::vector<RedisValue> values, std::pmr::memory_resource* resource) {
        return RedisValue::arrayValue(std::move(values), resource);
    }
};

}  // namespace ruvia::detail
