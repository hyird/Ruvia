#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/redis/redis_entity_codec.h"
#include "ruvia/web/detail/redis/redis_entity_key.h"
#include "ruvia/web/detail/redis/redis_repository_commands.h"
#include "ruvia/web/entity_rows.h"

namespace ruvia::detail {

template <typename entity_type>
entity_type decode_redis_entity(const redis_value& reply, std::string_view key,
    std::pmr::memory_resource* resource, std::string_view prefix = {}) {
    const auto fields_value = redis_orm_array(reply);
    if (fields_value.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity hash reply");
    }
    bool marker = false;
    for (std::size_t i = 0; i < fields_value.size(); i += 2) {
        const auto name = redis_orm_string(fields_value[i]);
        (void)redis_orm_string(fields_value[i + 1]);
        if (name == "__ruvia_entity") {
            if (marker || redis_orm_string(fields_value[i + 1]) != "1") {
                throw redis_error(redis_error::code_type::protocol_error, "invalid entity storage marker");
            }
            marker = true;
        }
    }
    if (!marker) {
        throw redis_error(redis_error::code_type::protocol_error, "entity storage marker missing");
    }
    entity_type entity(resource);
    for_each_redis_field<entity_type>([&]<typename field_type> {
        std::optional<std::string_view> value;
        for (std::size_t i = 0; i < fields_value.size(); i += 2) {
            if (redis_orm_string(fields_value[i]) == field_type::name.view()) {
                if (value) {
                    throw redis_error(redis_error::code_type::protocol_error, "duplicate entity field");
                }
                value = redis_orm_string(fields_value[i + 1]);
            }
        }
        if (value) {
            entity.template set<field_type::name>(decode_redis_scalar<typename field_type::value_type>(*value, resource));
        } else if constexpr (field_type::options.nullable_) {
            entity.template set_null<field_type::name>();
        } else {
            throw redis_error(redis_error::code_type::protocol_error, "required entity field missing from hash");
        }
    });
    try {
        const auto id = redis_entity_id(entity, resource);
        if (redis_entity_key<entity_type>(id, resource, prefix) != key) {
            throw redis_error(redis_error::code_type::protocol_error, "entity ID does not match its Redis key");
        }
    } catch (const std::invalid_argument&) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid stored entity ID");
    }
    return entity;
}

template <typename entity_type>
struct redis_map_one final {
    std::pmr::string key_;
    std::pmr::string prefix_;
    std::optional<entity_type> operator()(redis_value&& reply, std::pmr::memory_resource* resource) const {
        if (redis_orm_array(reply).empty()) {
            return std::nullopt;
        }
        return decode_redis_entity<entity_type>(reply, key_, resource, prefix_);
    }
};

template <typename entity_type>
struct redis_map_search final {
    std::pmr::string prefix_;
    std::pair<entity_rows<entity_type>, std::uint64_t> operator()(
        redis_value&& reply, std::pmr::memory_resource* resource) const {
        const auto values = redis_orm_array(reply);
        if (values.empty() || values.size() % 2 != 1) {
            throw redis_error(redis_error::code_type::protocol_error, "invalid entity search reply");
        }
        const auto count = redis_orm_count(values.front());
        if (count < (values.size() - 1) / 2) {
            throw redis_error(redis_error::code_type::protocol_error, "invalid entity search count");
        }
        entity_rows<entity_type> rows(resource);
        for (std::size_t i = 1; i < values.size(); i += 2) {
            const auto key = redis_orm_string(values[i]);
            // A document may expire between index matching and field loading.
            if (values[i + 1].null()) {
                continue;
            }
            rows.push_back(decode_redis_entity<entity_type>(values[i + 1], key, resource, prefix_));
        }
        return {std::move(rows), count};
    }
};

template <typename entity_type>
struct redis_map_many final {
    std::pmr::string prefix_;
    entity_rows<entity_type> operator()(
        redis_value&& reply, std::pmr::memory_resource* resource) const {
        return redis_map_search<entity_type>{std::pmr::string(prefix_, resource)}(
            std::move(reply), resource)
            .first;
    }
};

template <typename entity_type>
struct redis_map_first final {
    std::pmr::string prefix_;
    std::optional<entity_type> operator()(redis_value&& reply, std::pmr::memory_resource* resource) const {
        auto result_value = redis_map_search<entity_type>{std::pmr::string(prefix_, resource)}(
            std::move(reply), resource);
        if (result_value.first.empty()) {
            return std::nullopt;
        }
        if (result_value.first.size() != 1) {
            throw redis_error(redis_error::code_type::protocol_error, "multiple entities in find_one reply");
        }
        return std::move(result_value.first[0]);
    }
};

struct redis_map_count final {
    std::uint64_t operator()(redis_value&& reply, std::pmr::memory_resource*) const {
        const auto values = redis_orm_array(reply);
        if (values.size() != 1) {
            throw redis_error(redis_error::code_type::protocol_error, "invalid entity count reply");
        }
        return redis_orm_count(values.front());
    }
};

}  // namespace ruvia::detail
