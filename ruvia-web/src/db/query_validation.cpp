#include <stdexcept>

#include "query_storage.h"

namespace ruvia::detail {

void validate_query_shape(const db_query_storage& s) {
    const bool select = s.kind_ == db_query_kind::select;
    const bool values = s.kind_ == db_query_kind::values;
    const bool insert = s.kind_ == db_query_kind::insert;
    const bool update = s.kind_ == db_query_kind::update;
    if (!select && (!s.projections_.empty() || s.distinct_ || !s.distinct_on_.empty() || !s.groups_.empty() || s.having_ != no_db_node)) {
        throw std::invalid_argument("SELECT clauses cannot be attached to a non-SELECT query");
    }
    if ((select || values) && (!s.returning_.empty() || !s.target_.empty())) {
        throw std::invalid_argument("a row query cannot have a DML target or RETURNING");
    }
    if ((insert || values) && (s.source_ || !s.joins_.empty() || s.predicate_ != no_db_node)) {
        throw std::invalid_argument("INSERT/VALUES cannot have FROM/JOIN/WHERE clauses");
    }
    if (!s.source_ && !s.joins_.empty()) {
        throw std::invalid_argument("JOIN requires a FROM source");
    }
    if (!insert && (s.conflict_ || s.insert_query_ != no_db_node || !s.columns_.empty())) {
        throw std::invalid_argument("INSERT clauses on a non-INSERT query");
    }
    if (!insert && !values && !s.rows_.empty()) {
        throw std::invalid_argument("VALUES on an incompatible query");
    }
    if (!update && !s.assignments_.empty()) {
        throw std::invalid_argument("SET on a non-UPDATE query");
    }
    if (!select && !values && (!s.orders_.empty() || s.limit_ || s.offset_ || s.lock_ || !s.set_operations_.empty())) {
        throw std::invalid_argument("ordering, pagination, locks and set operations require a row query");
    }
    if (s.lock_ && (!select || !s.set_operations_.empty() || s.distinct_ || !s.distinct_on_.empty() || !s.groups_.empty() || s.having_ != no_db_node)) {
        throw std::invalid_argument("row locks require an ungrouped SELECT without DISTINCT or set operations");
    }
}

}  // namespace ruvia::detail
