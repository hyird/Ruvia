#pragma once
#include "ruvia/web/db/DbEntity.h"
namespace ruvia {
// Result schema without a table binding. Columns retain the entity field/value API.
template <typename... ColumnTypes>
class DbProjection : private DbEntity<"", ColumnTypes...> {
    static_assert((is_db_column<ColumnTypes>::value && ...));
    using Storage = DbEntity<"", ColumnTypes...>;

public:
    using DbProjectionType = DbProjection;
    using Columns = std::tuple<ColumnTypes...>;
    using Storage::get;
    using Storage::isNull;
    using Storage::isSet;
    using Storage::reset;
    using Storage::resource;
    using Storage::set;
    using Storage::setNull;
    using Storage::Storage;
};
}  // namespace ruvia
#define RUVIA_DB_PROJECTION(Name, ...)                          \
    struct Name final : ::ruvia::DbProjection<__VA_ARGS__> {    \
        using ::ruvia::DbProjection<__VA_ARGS__>::DbProjection; \
    };
