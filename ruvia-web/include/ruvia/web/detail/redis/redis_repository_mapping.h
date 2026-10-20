#pragma once

#include <array>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "ruvia/web/detail/redis/redis_entity_codec.h"
#include "ruvia/web/detail/redis/redis_entity_key.h"
#include "ruvia/web/detail/redis/redis_repository_commands.h"
#include "ruvia/web/entity_rows.h"

namespace ruvia::detail {

template <typename entity_type>
entity_type decode_redis_entity(std::span<const redis_value> fields_value, std::string_view key,
    std::pmr::memory_resource* resource, std::string_view prefix) {
    if (fields_value.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity hash reply");
    }
    std::array<std::optional<std::string_view>, std::tuple_size_v<typename entity_type::columns_type>> values{};
    bool marker = false;
    for (std::size_t i = 0; i < fields_value.size(); i += 2) {
        const auto name = redis_orm_string(fields_value[i]);
        const auto value = redis_orm_string(fields_value[i + 1]);
        if (name == "__ruvia_entity") {
            if (marker || value != "1") {
                throw redis_error(redis_error::code_type::protocol_error, "invalid entity storage marker");
            }
            marker = true;
            continue;
        }
        for_each_descriptor<typename entity_type::columns_type>([&]<typename field_type, std::size_t index> {
            if (name == field_type::name.view()) {
                if (values[index].has_value()) {
                    throw redis_error(redis_error::code_type::protocol_error, "duplicate entity field");
                }
                values[index] = value;
            }
        });
    }
    if (!marker) {
        throw redis_error(redis_error::code_type::protocol_error, "entity storage marker missing");
    }
    entity_type entity(resource);
    for_each_descriptor<typename entity_type::columns_type>([&]<typename field_type, std::size_t index> {
        if (values[index].has_value()) {
            entity.template set<field_type::name>(
                decode_redis_scalar<typename field_type::value_type>(*values[index], resource));
        } else if constexpr (field_type::options.nullable_) {
            entity.template set_null<field_type::name>();
        } else {
            throw redis_error(redis_error::code_type::protocol_error, "required entity field missing from hash");
        }
    });
    try {
        const auto id = redis_entity_id(entity, resource);
        if (redis_entity_key(prefix, id, resource) != key) {
            throw redis_error(redis_error::code_type::protocol_error, "entity ID does not match its Redis key");
        }
    } catch (const std::invalid_argument&) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid stored entity ID");
    }
    return entity;
}

template <typename entity_type>
entity_type decode_redis_entity(const redis_value& reply, std::string_view key,
    std::pmr::memory_resource* resource, std::string_view prefix) {
    return decode_redis_entity<entity_type>(redis_orm_array(reply), key, resource, prefix);
}

template <typename entity_type>
struct redis_map_one final {
    std::pmr::string key_;
    std::pmr::string prefix_;
    std::optional<entity_type> operator()(redis_value&& reply, std::pmr::memory_resource* resource) const {
        const auto fields_value = redis_orm_array(reply);
        if (fields_value.empty()) {
            return std::nullopt;
        }
        return decode_redis_entity<entity_type>(fields_value, key_, resource, prefix_);
    }
};

template <typename entity_type>
struct redis_map_search final {
    std::pmr::string prefix_;
    std::pair<entity_rows<entity_type>, std::uint64_t> decode(
        std::span<const redis_value> values, std::pmr::memory_resource* resource) const {
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
    std::pair<entity_rows<entity_type>, std::uint64_t> operator()(
        redis_value&& reply, std::pmr::memory_resource* resource) const {
        return decode(redis_orm_array(reply), resource);
    }
};

template <typename entity_type>
struct redis_map_many final {
    redis_map_search<entity_type> search_;
    // Set when the caller left `take_` unset: the reply must then hold every
    // match after this skip, or find() would silently truncate.
    std::optional<std::uint64_t> unpaged_skip_;
    entity_rows<entity_type> operator()(
        redis_value&& reply, std::pmr::memory_resource* resource) const {
        const auto values = redis_orm_array(reply);
        const auto documents = values.size() / 2;
        auto [rows, count] = search_.decode(values, resource);
        if (unpaged_skip_.has_value() && count > *unpaged_skip_ && count - *unpaged_skip_ > documents) {
            throw redis_error(redis_error::code_type::command_error,
                "redis find() matched more entities than one default page; set take_ to page the results");
        }
        return std::move(rows);
    }
};

template <typename entity_type>
struct redis_map_first final {
    redis_map_search<entity_type> search_;
    std::optional<entity_type> operator()(redis_value&& reply, std::pmr::memory_resource* resource) const {
        auto result_value = search_(std::move(reply), resource);
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

struct redis_map_exists final {
    bool operator()(redis_value&& reply, std::pmr::memory_resource*) const {
        const auto values = redis_orm_array(reply);
        if (values.size() != 1) {
            throw redis_error(redis_error::code_type::protocol_error, "invalid entity exists reply");
        }
        return redis_orm_count(values.front()) != 0;
    }
};

}  // namespace ruvia::detail
