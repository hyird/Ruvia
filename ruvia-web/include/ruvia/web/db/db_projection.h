#pragma once

#include "ruvia/web/db/db_entity.h"

namespace ruvia {

// Result schema without SQL table or relation metadata.
template <typename... column_types_type>
class db_projection final {
    static_assert((is_db_column<column_types_type>::value && ...));
    detail::entity_value_storage<column_types_type...> values_;

public:
    using db_projection_type = db_projection;
    using columns_type = std::tuple<column_types_type...>;
    explicit db_projection(std::pmr::memory_resource* resource = nullptr)
        : values_(resource) {}
    RUVIA_DETAIL_ENTITY_VALUE_API(values_, set_null, is_set, is_null)
};

}  // namespace ruvia

#define RUVIA_DB_PROJECTION(name, ...)                                    \
    struct name final {                                                   \
    private:                                                              \
        using storage_type = ::ruvia::db_projection<__VA_ARGS__>;         \
        storage_type values_;                                             \
                                                                          \
    public:                                                               \
        using db_projection_type = name;                                  \
        using columns_type = typename storage_type::columns_type;         \
        explicit name(std::pmr::memory_resource* resource = nullptr)      \
            : values_(resource) {}                                        \
        RUVIA_DETAIL_ENTITY_VALUE_API(values_, set_null, is_set, is_null) \
    };
