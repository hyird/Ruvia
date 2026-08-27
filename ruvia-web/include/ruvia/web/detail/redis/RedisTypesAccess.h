#pragma once

#include "ruvia/web/redis/RedisTypes.h"

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <utility>
#include <vector>

namespace ruvia::detail {

struct RedisTypesAccess final {
    [[nodiscard]] static RedisSetResult setResult(
        bool applied, std::optional<std::pmr::string> previous = std::nullopt) {
        return RedisSetResult(applied, std::move(previous));
    }

    [[nodiscard]] static constexpr RedisScanCursor scanCursor(std::uint64_t value) noexcept {
        return RedisScanCursor(value);
    }

    [[nodiscard]] static constexpr std::uint64_t cursorValue(RedisScanCursor cursor) noexcept {
        return cursor.value_;
    }

    [[nodiscard]] static RedisKeyValue keyValue(
        std::string_view key, std::string_view value, std::pmr::memory_resource* resource) {
        return RedisKeyValue(key, value, resource);
    }

    [[nodiscard]] static RedisScoredValue scoredValue(
        std::string_view value, double score, std::pmr::memory_resource* resource) {
        return RedisScoredValue(value, score, resource);
    }

    [[nodiscard]] static RedisScanResult scanResult(std::pmr::memory_resource* resource) {
        return RedisScanResult(resource);
    }

    [[nodiscard]] static std::optional<RedisScanCursor>& nextCursor(
        RedisScanResult& result) noexcept {
        return result.nextCursor_;
    }

    [[nodiscard]] static std::pmr::vector<std::pmr::string>& values(
        RedisScanResult& result) noexcept {
        return result.values_;
    }

    [[nodiscard]] static RedisHashScanResult hashScanResult(std::pmr::memory_resource* resource) {
        return RedisHashScanResult(resource);
    }

    [[nodiscard]] static std::optional<RedisScanCursor>& nextCursor(
        RedisHashScanResult& result) noexcept {
        return result.nextCursor_;
    }

    [[nodiscard]] static std::pmr::vector<RedisKeyValue>& entries(
        RedisHashScanResult& result) noexcept {
        return result.entries_;
    }

    [[nodiscard]] static RedisZScanResult zScanResult(std::pmr::memory_resource* resource) {
        return RedisZScanResult(resource);
    }

    [[nodiscard]] static std::optional<RedisScanCursor>& nextCursor(
        RedisZScanResult& result) noexcept {
        return result.nextCursor_;
    }

    [[nodiscard]] static std::pmr::vector<RedisScoredValue>& entries(
        RedisZScanResult& result) noexcept {
        return result.entries_;
    }

    [[nodiscard]] static RedisStreamEntry streamEntry(
        std::string_view id, std::pmr::memory_resource* resource) {
        return RedisStreamEntry(id, resource);
    }

    [[nodiscard]] static std::pmr::vector<RedisKeyValue>& fields(RedisStreamEntry& entry) noexcept {
        return entry.fields_;
    }

    [[nodiscard]] static RedisStreamReadResult streamReadResult(
        std::string_view stream, std::pmr::memory_resource* resource) {
        return RedisStreamReadResult(stream, resource);
    }

    [[nodiscard]] static std::pmr::vector<RedisStreamEntry>& entries(
        RedisStreamReadResult& result) noexcept {
        return result.entries_;
    }

    [[nodiscard]] static RedisXReadGroupResult xreadGroupResult(
        std::pmr::memory_resource* resource) {
        return RedisXReadGroupResult(resource);
    }

    [[nodiscard]] static std::pmr::vector<RedisStreamReadResult>& streams(
        RedisXReadGroupResult& result) noexcept {
        return result.streams_;
    }

    [[nodiscard]] static constexpr RedisTtl ttl(RedisTtlState state,
        std::optional<std::chrono::milliseconds> remaining = std::nullopt) noexcept {
        return RedisTtl(state, remaining);
    }

    [[nodiscard]] static RedisValue nullValue(std::pmr::memory_resource* resource) {
        return RedisValue::nullValue(resource);
    }

    [[nodiscard]] static RedisValue stringValue(
        std::string_view value, std::pmr::memory_resource* resource) {
        return RedisValue::stringValue(value, resource);
    }

    [[nodiscard]] static RedisValue errorValue(
        std::string_view value, std::pmr::memory_resource* resource) {
        return RedisValue::errorValue(value, resource);
    }

    [[nodiscard]] static RedisValue integerValue(
        std::int64_t value, std::pmr::memory_resource* resource) {
        return RedisValue::integerValue(value, resource);
    }

    [[nodiscard]] static RedisValue arrayValue(
        std::pmr::vector<RedisValue> values, std::pmr::memory_resource* resource) {
        return RedisValue::arrayValue(std::move(values), resource);
    }
};

}  // namespace ruvia::detail
