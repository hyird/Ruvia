#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/web/detail/entity/value_storage.h"
#include "ruvia/web/detail/redis/redis_entity_codec.h"

namespace ruvia::detail {

// Every ORM entity key is `redis_entity_key_root + lower-hex(prefix) + ':' + id`.
inline constexpr std::string_view redis_entity_key_root = "ruvia:orm:";

[[nodiscard]] inline std::pmr::string redis_entity_storage_prefix(
    std::string_view prefix, std::pmr::memory_resource* resource, std::size_t extra_capacity = 0) {
    resource = pmr_resource_or_default(resource);
    std::pmr::string result(resource);
    result.reserve(redis_entity_key_root.size() + prefix.size() * 2U + 1U + extra_capacity);
    result.append(redis_entity_key_root);
    http_append_lower_hex(result, prefix);
    result.push_back(':');
    return result;
}

[[nodiscard]] inline std::pmr::string redis_entity_key(
    std::string_view prefix, std::string_view id, std::pmr::memory_resource* resource) {
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    auto key = redis_entity_storage_prefix(prefix, resource, id.size());
    key.append(id.data(), id.size());
    return key;
}

[[nodiscard]] inline bool redis_entity_key_matches(
    std::string_view prefix, std::string_view id, std::string_view key) noexcept {
    const std::size_t hex_size = prefix.size() * 2U;
    const std::size_t prefix_end = redis_entity_key_root.size() + hex_size;
    if (id.empty() || key.size() != prefix_end + 1U + id.size() || !key.starts_with(redis_entity_key_root) ||
        key[prefix_end] != ':' || key.substr(prefix_end + 1U) != id) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto byte = static_cast<unsigned char>(prefix[i]);
        if (key[redis_entity_key_root.size() + i * 2U] != http_lower_hex_digit(byte >> 4U) ||
            key[redis_entity_key_root.size() + i * 2U + 1U] != http_lower_hex_digit(byte)) {
            return false;
        }
    }
    return true;
}

template <typename entity_type>
[[nodiscard]] std::optional<std::string_view> try_redis_entity_id(
    const entity_type& entity, std::span<char, redis_scalar_scratch_size> scratch) {
    std::optional<std::string_view> id;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        if constexpr (column_type::options.primary_key_) {
            if (entity.template is_set<column_type::name>() && !entity.template is_null<column_type::name>()) {
                const auto encoded = encode_redis_scalar(entity.template get<column_type::name>(), scratch);
                if (!encoded.empty()) {
                    id = encoded;
                }
            }
        }
    });
    return id;
}

template <typename entity_type>
[[nodiscard]] std::string_view redis_entity_id(
    const entity_type& entity, std::span<char, redis_scalar_scratch_size> scratch) {
    const auto id = try_redis_entity_id(entity, scratch);
    if (!id) {
        throw std::invalid_argument("Redis entity requires an ID");
    }
    return *id;
}

}  // namespace ruvia::detail
