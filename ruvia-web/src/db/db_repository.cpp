#include "ruvia/web/db/db_repository.h"

#include <algorithm>
#include <stdexcept>

namespace ruvia::detail {

std::pmr::vector<std::pmr::string> apply_projection(db_query& query, std::span<const db_selection> selection,
    std::string_view qualifier, bool returning, std::string_view root_table) {
    if (selection.empty()) {
        throw std::invalid_argument("projection requires at least one field");
    }
    std::pmr::vector<std::pmr::string> columns(query.resource());
    std::pmr::vector<db_expression> values(query.resource());
    const auto alias = qualifier == root_table ? std::string_view{} : qualifier;
    for (const auto& field : selection) {
        if (field.column_.empty() || std::ranges::find(columns, std::string_view(field.column_)) != columns.end()) {
            throw std::invalid_argument("projection requires unique nonempty field names");
        }
        columns.emplace_back(field.column_);
        auto expression = !field.has_expression()
                              ? query.column(field.column_, qualifier)
                              : field.expression(query, root_table, alias);
        values.push_back(query.alias(expression, field.column_));
    }
    if (returning) {
        query.returning(values);
    } else {
        query.select(values);
    }
    return columns;
}

}  // namespace ruvia::detail
