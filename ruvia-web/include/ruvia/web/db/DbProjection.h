#pragma once

#include "ruvia/web/db/DbEntity.h"

namespace ruvia {

// Result schema without SQL table or relation metadata.
template <typename... ColumnTypes>
class DbProjection final {
    static_assert((is_db_column<ColumnTypes>::value && ...));
    detail::entity_value_storage<ColumnTypes...> values_;

public:
    using DbProjectionType = DbProjection;
    using Columns = std::tuple<ColumnTypes...>;
    explicit DbProjection(std::pmr::memory_resource* resource = nullptr)
        : values_(resource) {}
    RUVIA_DETAIL_ENTITY_VALUE_API(values_, set_null, is_set, is_null)
};

}  // namespace ruvia

#define RUVIA_DB_PROJECTION(Name, ...)                                 \
    struct Name final {                                                \
    private:                                                           \
        using storage_type = ::ruvia::DbProjection<__VA_ARGS__>;       \
        storage_type values_;                                          \
                                                                       \
    public:                                                            \
        using DbProjectionType = Name;                                 \
        using Columns = typename storage_type::Columns;                \
        explicit Name(std::pmr::memory_resource* resource = nullptr)   \
            : values_(resource) {}                                     \
        RUVIA_DETAIL_ENTITY_VALUE_API(values_, setNull, isSet, isNull) \
    };
