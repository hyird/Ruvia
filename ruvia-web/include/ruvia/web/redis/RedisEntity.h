#pragma once

#include <tuple>
#include <type_traits>

#include "ruvia/web/db/DbPredicate.h"
#include "ruvia/web/detail/redis/RedisEntityTraits.h"

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

// Reuse the owning value slots without exposing SQL column configuration or
// conversion to a SQL entity. This adapter never participates in query mapping.
template <typename Column>
using RedisColumnStorage = DbColumn<Column::name, typename Column::value_type,
    DbColumnOptions{.primaryKey = Column::options.primaryKey, .nullable = Column::options.nullable}>;
}  // namespace detail

template <FixedString Name, typename... ColumnTypes>
class RedisEntity : private DbEntity<Name, detail::RedisColumnStorage<ColumnTypes>...> {
    static_assert((detail::IsRedisColumn<ColumnTypes>::value && ...),
        "Redis entities require RUVIA_REDIS_COLUMN descriptors");
    static_assert((std::size_t{ColumnTypes::options.primaryKey} + ... + std::size_t{0}) == 1,
        "Redis entities require exactly one primary key");
    using Storage = DbEntity<Name, detail::RedisColumnStorage<ColumnTypes>...>;

public:
    using RedisEntityType = RedisEntity;
    using Columns = std::tuple<ColumnTypes...>;
    using Relations = std::tuple<>;
    using Storage::columnIndex;
    using Storage::columnName;
    using Storage::get;
    using Storage::isNull;
    using Storage::isSet;
    using Storage::reset;
    using Storage::resource;
    using Storage::set;
    using Storage::setNull;
    using Storage::Storage;
    using Storage::tableName;

    template <FixedString Field>
    static DbFieldReference<RedisEntity, Field> column() {
        return {};
    }
};

#define RUVIA_REDIS_COLUMN(Name, Type, ...) ::ruvia::RedisColumn<::ruvia::FixedString{#Name}, Type __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_REDIS_ENTITY(Name, Prefix, ...)                                               \
    struct Name final : ::ruvia::RedisEntity<::ruvia::FixedString{Prefix}, __VA_ARGS__> {   \
        using ::ruvia::RedisEntity<::ruvia::FixedString{Prefix}, __VA_ARGS__>::RedisEntity; \
    };

}  // namespace ruvia
