#pragma once

#include <variant>

namespace ruvia::detail {

class mariadb_pool;
class postgresql_pool;

// Closed, allocation-free reference to one concrete database pool. Database
// operations cross exactly one explicit branch; there is no virtual dispatch,
// shared ownership, or request-path allocation.
using db_pool_ref_type = std::variant<std::monostate, mariadb_pool*, postgresql_pool*>;

}  // namespace ruvia::detail
