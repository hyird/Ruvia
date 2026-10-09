#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/redis/redis_entity_codec.h"

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::string redis_entity_storage_prefix(
    std::string_view prefix, std::pmr::memory_resource* resource = nullptr) {
    resource = pmr_resource_or_default(resource);
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

template <typename entity_type>
[[nodiscard]] inline std::pmr::string redis_entity_storage_prefix(
    std::string_view prefix, std::pmr::memory_resource* resource = nullptr) {
    (void)sizeof(entity_type);
    return redis_entity_storage_prefix(prefix, resource);
}

template <typename entity_type>
[[nodiscard]] inline std::pmr::string redis_entity_storage_prefix(
    std::pmr::memory_resource* resource = nullptr) {
    return redis_entity_storage_prefix(entity_type::prefix(), resource);
}

template <typename entity_type>
[[nodiscard]] inline std::pmr::string redis_entity_key(std::string_view id,
    std::pmr::memory_resource* resource, std::string_view prefix = {}) {
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    auto key = redis_entity_storage_prefix(
        prefix.empty() ? entity_type::prefix() : prefix, resource);
    key.append(id.data(), id.size());
    return key;
}

template <typename entity_type>
[[nodiscard]] std::pmr::string redis_entity_id(
    const entity_type& entity, std::pmr::memory_resource* resource) {
    validate_redis_entity<entity_type>();
    std::pmr::string id(pmr_resource_or_default(resource));
    for_each_redis_field<entity_type>([&]<typename field_type> {
        if constexpr (field_type::options.id_) {
            if (!entity.template is_set<field_type::name>() || entity.template is_null<field_type::name>()) {
                throw std::invalid_argument("Redis entity requires an ID");
            }
            id = encode_redis_scalar(entity.template get<field_type::name>(), resource);
        }
    });
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    return id;
}

}  // namespace ruvia::detail
