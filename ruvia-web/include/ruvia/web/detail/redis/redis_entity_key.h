#pragma once

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/web/detail/redis/redis_entity_codec.h"

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::string redis_entity_storage_prefix(
    std::string_view prefix, std::pmr::memory_resource* resource) {
    resource = pmr_resource_or_default(resource);
    constexpr std::string_view root = "ruvia:orm:";
    std::pmr::string result(resource);
    result.reserve(root.size() + prefix.size() * 2U + 1U);
    result.append(root);
    http_append_lower_hex(result, prefix);
    result.push_back(':');
    return result;
}

[[nodiscard]] inline std::pmr::string redis_entity_key(
    std::string_view prefix, std::string_view id, std::pmr::memory_resource* resource) {
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    resource = pmr_resource_or_default(resource);
    constexpr std::string_view root = "ruvia:orm:";
    std::pmr::string key(resource);
    key.reserve(root.size() + prefix.size() * 2U + 1U + id.size());
    key.append(root);
    http_append_lower_hex(key, prefix);
    key.push_back(':');
    key.append(id.data(), id.size());
    return key;
}

[[nodiscard]] inline bool redis_entity_key_matches(
    std::string_view prefix, std::string_view id, std::string_view key) noexcept {
    constexpr std::string_view root = "ruvia:orm:";
    const std::size_t hex_size = prefix.size() * 2U;
    const std::size_t prefix_end = root.size() + hex_size;
    if (id.empty() || key.size() != prefix_end + 1U + id.size() || !key.starts_with(root) ||
        key[prefix_end] != ':' || key.substr(prefix_end + 1U) != id) {
        return false;
    }
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto byte = static_cast<unsigned char>(prefix[i]);
        if (key[root.size() + i * 2U] != digits[byte >> 4U] ||
            key[root.size() + i * 2U + 1U] != digits[byte & 0x0fU]) {
            return false;
        }
    }
    return true;
}

template <typename entity_type>
[[nodiscard]] std::string_view redis_entity_id(
    const entity_type& entity, std::span<char, redis_scalar_scratch_size> scratch) {
    validate_redis_entity<entity_type>();
    std::string_view id;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        if constexpr (column_type::options.primary_key_) {
            if (!entity.template is_set<column_type::name>() || entity.template is_null<column_type::name>()) {
                throw std::invalid_argument("Redis entity requires an ID");
            }
            id = encode_redis_scalar(entity.template get<column_type::name>(), scratch);
        }
    });
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    return id;
}

}  // namespace ruvia::detail
