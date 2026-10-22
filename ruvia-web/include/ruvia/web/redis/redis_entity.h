#pragma once

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/entity/value_storage.h"
#include "ruvia/web/detail/redis/redis_entity_traits.h"
#include "ruvia/web/redis/redis_predicate.h"

namespace ruvia {

struct redis_column_options final {
    bool primary_key_{false};
    bool nullable_{false};
};

template <fixed_string field_name, typename t_type, redis_column_options options_type = {}>
struct redis_column final {
    static_assert(detail::is_redis_entity_scalar<t_type>, "Redis columns require owning scalar values");
    static_assert(!options_type.primary_key_ || (detail::is_redis_entity_primary_key_scalar<t_type> && !options_type.nullable_),
        "Redis primary keys must be non-nullable strings or integers");
    static constexpr auto name = field_name;
    using value_type = t_type;
    static constexpr auto options = options_type;
};

namespace detail {
template <typename t_type>
struct is_redis_column : std::false_type {};
template <fixed_string name, typename t_type, redis_column_options options_type>
struct is_redis_column<redis_column<name, t_type, options_type>> : std::true_type {};
}  // namespace detail

template <fixed_string key_prefix, typename... column_types>
class redis_entity final {
    static_assert((detail::is_redis_column<column_types>::value && ...),
        "Redis entities require RUVIA_REDIS_COLUMN descriptors");
    static_assert((std::size_t{column_types::options.primary_key_} + ... + std::size_t{0}) == 1,
        "Redis entities require exactly one primary key");
    static_assert(detail::unique_entity_columns<column_types...>(), "duplicate Redis entity column name");
    using column_storage = detail::entity_value_storage<column_types...>;
    column_storage values_;

public:
    using redis_entity_type = redis_entity;
    using columns_type = std::tuple<column_types...>;
    static constexpr std::string_view prefix() noexcept {
        return key_prefix.view();
    }
    template <fixed_string name>
    static consteval std::size_t column_index() {
        return column_storage::template index<name>();
    }
    template <fixed_string name>
    static constexpr redis_field_reference<redis_entity, name> column() noexcept {
        return {};
    }

    explicit redis_entity(std::pmr::memory_resource* resource = nullptr)
        : values_(resource) {}
    template <typename owner_type>
        requires requires(owner_type& owner) {
            { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;
        }
    explicit redis_entity(owner_type& owner)
        : values_(owner.resource()) {}
    redis_entity(const redis_entity&) = delete;
    redis_entity& operator=(const redis_entity&) = delete;
    redis_entity(redis_entity&&) noexcept = default;
    redis_entity& operator=(redis_entity&&) = delete;

    RUVIA_DETAIL_ENTITY_VALUE_API(values_)
};

#define RUVIA_REDIS_COLUMN(name, value_type, ...) ::ruvia::redis_column<::ruvia::fixed_string{#name}, value_type __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_REDIS_ENTITY(name, key_prefix, ...)                                                   \
    struct name final {                                                                             \
    private:                                                                                        \
        using storage_type = ::ruvia::redis_entity<::ruvia::fixed_string{key_prefix}, __VA_ARGS__>; \
        storage_type entity_;                                                                       \
                                                                                                    \
    public:                                                                                         \
        using redis_entity_type = name;                                                             \
        using columns_type = typename storage_type::columns_type;                                   \
        explicit name(std::pmr::memory_resource* resource = nullptr)                                \
            : entity_(resource) {}                                                                  \
        template <typename owner_type>                                                              \
            requires requires(owner_type& owner) {                                                  \
                { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;            \
            }                                                                                       \
        explicit name(owner_type& owner)                                                            \
            : entity_(owner.resource()) {}                                                          \
        static constexpr std::string_view prefix() noexcept {                                       \
            return storage_type::prefix();                                                          \
        }                                                                                           \
        template <::ruvia::fixed_string field_name>                                                 \
        static consteval std::size_t column_index() {                                               \
            return storage_type::template column_index<field_name>();                               \
        }                                                                                           \
        template <::ruvia::fixed_string field_name>                                                 \
        static constexpr ::ruvia::redis_field_reference<name, field_name> column() noexcept {       \
            return {};                                                                              \
        }                                                                                           \
        RUVIA_DETAIL_ENTITY_VALUE_API(entity_)                                                      \
    };

}  // namespace ruvia
