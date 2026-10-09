#pragma once
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/detail/redis/redis_entity_codec.h"
#include "ruvia/web/redis/redis_repository_types.h"
namespace ruvia::detail {
struct redis_index_definition final {
    std::pmr::string field_;
    redis_index_kind kind_{redis_index_kind::none};
    bool sortable_{false};
};
struct redis_mapping final {
    std::pmr::string prefix_;
    std::pmr::vector<redis_index_definition> indexes_;
    [[nodiscard]] redis_index_kind index_kind(std::string_view column) const noexcept {
        for (const auto& index : indexes_) {
            if (index.field_ == column) {
                return index.kind_;
            }
        }
        return redis_index_kind::none;
    }
    [[nodiscard]] bool sortable(std::string_view column) const noexcept {
        for (const auto& index : indexes_) {
            if (index.field_ == column) {
                return index.sortable_;
            }
        }
        return false;
    }
};
template <typename entity_type>
redis_mapping normalize_redis_mapping(const redis_repository_config& config, std::pmr::memory_resource* resource) {
    validate_redis_entity<entity_type>();
    redis_mapping result_value{std::pmr::string(config.prefix_.empty() ? entity_type::prefix() : std::string_view(config.prefix_), resource), std::pmr::vector<redis_index_definition>(resource)};
    if (result_value.prefix_.empty()) {
        throw std::invalid_argument("Redis entity prefix cannot be empty");
    }
    for_each_redis_field<entity_type>([]<typename field_type> {
        if (field_type::name.view().starts_with("__ruvia_")) {
            throw std::invalid_argument("Redis entity column uses reserved __ruvia_ namespace");
        }
    });
    for (const auto& index : config.indexes_) {
        for (const auto& existing : result_value.indexes_) {
            if (std::string_view(existing.field_) == std::string_view(index.field_)) {
                throw std::invalid_argument("duplicate Redis index column");
            }
        }
        bool found = false;
        for_each_redis_field<entity_type>([&]<typename field_type> {
            if (field_type::name.view() != index.field_) {
                return;
            }
            found = true;
            using t_type = typename field_type::value_type;
            switch (index.kind_) {
                case redis_index_kind::tag:
                    if constexpr (!(is_redis_entity_string<t_type> || std::is_same_v<t_type, bool> || std::is_same_v<t_type, bool_value>)) {
                        throw std::invalid_argument("Redis TAG indexes require string or boolean columns");
                    }
                    break;
                case redis_index_kind::text:
                    if constexpr (!is_redis_entity_string<t_type>) {
                        throw std::invalid_argument("Redis TEXT indexes require string columns");
                    }
                    break;
                case redis_index_kind::numeric:
                    if constexpr (!is_redis_entity_number<t_type>) {
                        throw std::invalid_argument("Redis NUMERIC indexes require numeric columns");
                    }
                    break;
                default:
                    throw std::invalid_argument("Redis index kind must be TAG, TEXT or NUMERIC");
            }
        });
        if (!found) {
            throw std::invalid_argument("unknown Redis index column");
        }
        result_value.indexes_.push_back({std::pmr::string(index.field_, resource), index.kind_, index.sortable_});
    }
    return result_value;
}
}  // namespace ruvia::detail
