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
    std::pmr::string result(root, resource);
    result.reserve(root.size() + prefix.size() * 2U + 1U);
    http_append_lower_hex(result, prefix);
    result.push_back(':');
    return result;
}

[[nodiscard]] inline std::pmr::string redis_entity_key(
    std::string_view prefix, std::string_view id, std::pmr::memory_resource* resource) {
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    auto key = redis_entity_storage_prefix(prefix, resource);
    key.append(id.data(), id.size());
    return key;
}

template <typename entity_type>
[[nodiscard]] std::pmr::string redis_entity_id(
    const entity_type& entity, std::pmr::memory_resource* resource) {
    validate_redis_entity<entity_type>();
    std::pmr::string id(pmr_resource_or_default(resource));
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        if constexpr (column_type::options.primary_key_) {
            if (!entity.template is_set<column_type::name>() || entity.template is_null<column_type::name>()) {
                throw std::invalid_argument("Redis entity requires an ID");
            }
            id = encode_redis_scalar(entity.template get<column_type::name>(), resource);
        }
    });
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    return id;
}

}  // namespace ruvia::detail
