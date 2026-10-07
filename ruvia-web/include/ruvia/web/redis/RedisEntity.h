#pragma once

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/entity/ValueStorage.h"
#include "ruvia/web/detail/redis/RedisEntityTraits.h"
#include "ruvia/web/redis/RedisPredicate.h"

namespace ruvia {

struct RedisColumnOptions final {
    bool primaryKey{false};
    bool nullable{false};
};

template <FixedString Name, typename T, RedisColumnOptions Options = {}>
struct RedisColumn final {
    static_assert(detail::isRedisEntityScalar<T>, "Redis columns require owning scalar values");
    static_assert(!Options.primaryKey || (detail::isRedisEntityPrimaryKeyScalar<T> && !Options.nullable),
        "Redis primary keys must be non-nullable strings or integers");
    static constexpr auto name = Name;
    using value_type = T;
    static constexpr auto options = Options;
};

namespace detail {
template <typename T>
struct IsRedisColumn : std::false_type {};
template <FixedString Name, typename T, RedisColumnOptions Options>
struct IsRedisColumn<RedisColumn<Name, T, Options>> : std::true_type {};
}  // namespace detail

template <FixedString Prefix, typename... ColumnTypes>
class RedisEntity final {
    static_assert((detail::IsRedisColumn<ColumnTypes>::value && ...),
        "Redis entities require RUVIA_REDIS_COLUMN descriptors");
    static_assert((std::size_t{ColumnTypes::options.primaryKey} + ... + std::size_t{0}) == 1,
        "Redis entities require exactly one primary key");
    static_assert(detail::uniqueEntityColumns<ColumnTypes...>(), "duplicate Redis entity column name");
    using column_storage = detail::entity_value_storage<ColumnTypes...>;
    column_storage values_;

public:
    using RedisEntityType = RedisEntity;
    using Columns = std::tuple<ColumnTypes...>;
    static constexpr std::string_view prefix() noexcept {
        return Prefix.view();
    }
    template <FixedString Name>
    static consteval std::size_t columnIndex() {
        return column_storage::template index<Name>();
    }
    template <FixedString Name>
    static consteval std::string_view columnName() {
        (void)columnIndex<Name>();
        return Name.view();
    }
    template <FixedString Name>
    static redis_field_reference<RedisEntity, Name> field() {
        return {};
    }

    explicit RedisEntity(std::pmr::memory_resource* resource = nullptr)
        : values_(resource) {}
    RedisEntity(const RedisEntity&) = delete;
    RedisEntity& operator=(const RedisEntity&) = delete;
    RedisEntity(RedisEntity&&) noexcept = default;
    RedisEntity& operator=(RedisEntity&&) = delete;

    RUVIA_DETAIL_ENTITY_VALUE_API(values_, set_null, is_set, is_null)
};

#define RUVIA_REDIS_COLUMN(Name, Type, ...) ::ruvia::RedisColumn<::ruvia::FixedString{#Name}, Type __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_REDIS_ENTITY(Name, Prefix, ...)                                                 \
    struct Name final {                                                                       \
    private:                                                                                  \
        using storage_type = ::ruvia::RedisEntity<::ruvia::FixedString{Prefix}, __VA_ARGS__>; \
        storage_type entity_;                                                                 \
                                                                                              \
    public:                                                                                   \
        using RedisEntityType = Name;                                                         \
        using Columns = typename storage_type::Columns;                                       \
        explicit Name(std::pmr::memory_resource* resource = nullptr)                          \
            : entity_(resource) {}                                                            \
        static constexpr std::string_view prefix() noexcept {                                 \
            return storage_type::prefix();                                                    \
        }                                                                                     \
        template <::ruvia::FixedString name>                                                  \
        static consteval std::size_t columnIndex() {                                          \
            return storage_type::template columnIndex<name>();                                \
        }                                                                                     \
        template <::ruvia::FixedString name>                                                  \
        static consteval std::string_view columnName() {                                      \
            return storage_type::template columnName<name>();                                 \
        }                                                                                     \
        template <::ruvia::FixedString name>                                                  \
        static ::ruvia::redis_field_reference<Name, name> field() {                           \
            return {};                                                                        \
        }                                                                                     \
        RUVIA_DETAIL_ENTITY_VALUE_API(entity_, setNull, isSet, isNull)                        \
    };

}  // namespace ruvia
