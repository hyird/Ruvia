#include "ruvia/web/detail/db/DbPreparedStatement.h"

#include <stdexcept>

#include "ruvia/web/detail/db/DbSqlScan.h"
#include "ruvia/web/detail/db/DbUtils.h"

namespace ruvia {

PreparedDbStatement prepareDbStatement(
    std::string_view sql, std::span<const DbValue> params, std::pmr::memory_resource* resource) {
    if (!detail::hasSqlNonWhitespace(sql)) {
        throw std::invalid_argument("SQL must not be empty");
    }
    auto* resolved = detail::pmrResourceOrDefault(resource);
    return PreparedDbStatement{
        std::pmr::string(sql, resolved), detail::cloneDbValues(params, resolved)};
}

}  // namespace ruvia
