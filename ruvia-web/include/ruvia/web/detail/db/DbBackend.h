#pragma once

#include <variant>

namespace ruvia::detail {

class MariaDbPool;
class PostgreSqlPool;

// Closed, allocation-free reference to one concrete database pool. Database
// operations cross exactly one explicit branch; there is no virtual dispatch,
// shared ownership, or request-path allocation.
using DbPoolRef = std::variant<
    std::monostate,
    MariaDbPool*,
    PostgreSqlPool*>;

}  // namespace ruvia::detail
