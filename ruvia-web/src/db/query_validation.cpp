#include <stdexcept>

#include "query_storage.h"

namespace ruvia::detail {

void validate_query_shape(const DbQueryStorage& s) {
    const bool select = s.kind == DbQueryKind::kSelect;
    const bool values = s.kind == DbQueryKind::kValues;
    const bool insert = s.kind == DbQueryKind::kInsert;
    const bool update = s.kind == DbQueryKind::kUpdate;
    if (!select && (!s.projections.empty() || s.distinct || !s.distinctOn.empty() || !s.groups.empty() || s.having != noDbNode)) {
        throw std::invalid_argument("SELECT clauses cannot be attached to a non-SELECT query");
    }
    if ((select || values) && (!s.returning.empty() || !s.target.empty())) {
        throw std::invalid_argument("a row query cannot have a DML target or RETURNING");
    }
    if ((insert || values) && (s.source || !s.joins.empty() || s.predicate != noDbNode)) {
        throw std::invalid_argument("INSERT/VALUES cannot have FROM/JOIN/WHERE clauses");
    }
    if (!s.source && !s.joins.empty()) {
        throw std::invalid_argument("JOIN requires a FROM source");
    }
    if (!insert && (s.conflict || s.insertQuery != noDbNode || !s.columns.empty())) {
        throw std::invalid_argument("INSERT clauses on a non-INSERT query");
    }
    if (!insert && !values && !s.rows.empty()) {
        throw std::invalid_argument("VALUES on an incompatible query");
    }
    if (!update && !s.assignments.empty()) {
        throw std::invalid_argument("SET on a non-UPDATE query");
    }
    if (!select && !values && (!s.orders.empty() || s.limit || s.offset || s.lock || !s.setOperations.empty())) {
        throw std::invalid_argument("ordering, pagination, locks and set operations require a row query");
    }
    if (s.lock && (!select || !s.setOperations.empty() || s.distinct || !s.distinctOn.empty() || !s.groups.empty() || s.having != noDbNode)) {
        throw std::invalid_argument("row locks require an ungrouped SELECT without DISTINCT or set operations");
    }
}

}  // namespace ruvia::detail
