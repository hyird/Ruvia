#pragma once

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/redis/redis_types.h"

namespace ruvia::detail {

struct redis_types_access final {
    [[nodiscard]] static redis_set_result set_result(
        bool applied, std::optional<std::pmr::string> previous = std::nullopt) {
        return redis_set_result(applied, std::move(previous));
    }

    [[nodiscard]] static constexpr redis_scan_cursor scan_cursor(std::uint64_t value) noexcept {
        return redis_scan_cursor(value);
    }

    [[nodiscard]] static constexpr std::uint64_t cursor_value(redis_scan_cursor cursor_value) noexcept {
        return cursor_value.value_;
    }

    [[nodiscard]] static redis_key_value key_value(
        std::string_view key, std::string_view value, std::pmr::memory_resource* resource) {
        return redis_key_value(key, value, resource);
    }

    [[nodiscard]] static redis_scored_value scored_value(
        std::string_view value, double score, std::pmr::memory_resource* resource) {
        return redis_scored_value(value, score, resource);
    }

    [[nodiscard]] static redis_scan_result scan_result(std::pmr::memory_resource* resource) {
        return redis_scan_result(resource);
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::pmr::vector<std::pmr::string>& values(
        redis_scan_result& result_value) noexcept {
        return result_value.values_;
    }

    [[nodiscard]] static redis_hash_scan_result hash_scan_result(std::pmr::memory_resource* resource) {
        return redis_hash_scan_result(resource);
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_hash_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::pmr::vector<redis_key_value>& entries(
        redis_hash_scan_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_z_scan_result z_scan_result(std::pmr::memory_resource* resource) {
        return redis_z_scan_result(resource);
    }

    [[nodiscard]] static std::optional<redis_scan_cursor>& next_cursor(
        redis_z_scan_result& result_value) noexcept {
        return result_value.next_cursor_;
    }

    [[nodiscard]] static std::pmr::vector<redis_scored_value>& entries(
        redis_z_scan_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_stream_entry stream_entry(
        std::string_view id, std::pmr::memory_resource* resource) {
        return redis_stream_entry(id, resource);
    }

    [[nodiscard]] static std::pmr::vector<redis_key_value>& fields(redis_stream_entry& entry_value) noexcept {
        return entry_value.fields_;
    }

    [[nodiscard]] static redis_stream_read_result stream_read_result(
        std::string_view stream, std::pmr::memory_resource* resource) {
        return redis_stream_read_result(stream, resource);
    }

    [[nodiscard]] static std::pmr::vector<redis_stream_entry>& entries(
        redis_stream_read_result& result_value) noexcept {
        return result_value.entries_;
    }

    [[nodiscard]] static redis_x_read_group_result xread_group_result(
        std::pmr::memory_resource* resource) {
        return redis_x_read_group_result(resource);
    }

    [[nodiscard]] static std::pmr::vector<redis_stream_read_result>& streams(
        redis_x_read_group_result& result_value) noexcept {
        return result_value.streams_;
    }

    [[nodiscard]] static constexpr redis_ttl ttl(redis_ttl_state state_value,
        std::optional<std::chrono::milliseconds> remaining = std::nullopt) noexcept {
        return redis_ttl(state_value, remaining);
    }

    [[nodiscard]] static redis_value null_value(std::pmr::memory_resource* resource) {
        return redis_value::null_value(resource);
    }

    [[nodiscard]] static redis_value string_value(
        std::string_view value, std::pmr::memory_resource* resource) {
        return redis_value::string_value(value, resource);
    }

    [[nodiscard]] static redis_value error_value(
        std::string_view value, std::pmr::memory_resource* resource) {
        return redis_value::error_value(value, resource);
    }

    [[nodiscard]] static redis_value integer_value(
        std::int64_t value, std::pmr::memory_resource* resource) {
        return redis_value::integer_value(value, resource);
    }

    [[nodiscard]] static redis_value array_value(
        std::pmr::vector<redis_value> values, std::pmr::memory_resource* resource) {
        return redis_value::array_value(std::move(values), resource);
    }
};

}  // namespace ruvia::detail
