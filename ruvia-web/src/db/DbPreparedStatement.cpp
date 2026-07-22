#include "ruvia/web/detail/db/DbPreparedStatement.h"

#include "ruvia/web/detail/db/DbUtils.h"

namespace ruvia {

PreparedDbStatement prepareDbStatement(
    std::string_view sql,
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource) {
    return PreparedDbStatement{
        std::pmr::string(sql, resource),
        detail::cloneDbValues(params, resource)};
}

}  // namespace ruvia
