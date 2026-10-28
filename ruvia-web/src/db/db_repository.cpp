#include "ruvia/web/db/db_repository.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "db/db_sql_format.h"

namespace ruvia::detail {

db_projection apply_projection(db_query& query, std::span<const db_selection> selection,
    std::string_view qualifier, bool returning, std::string_view root_table, const db_projection* previous) {
    if (selection.empty()) {
        throw std::invalid_argument("projection requires at least one field");
    }
    db_projection result(query.resource());
    const auto named_count = static_cast<std::size_t>(std::ranges::count_if(selection, [](const auto& field) { return !field.is_wildcard(); }));
    result.columns_.reserve((previous ? previous->columns_.size() : 0) + named_count);
    if (previous) {
        result.columns_ = previous->columns_;
        result.wildcard_ = previous->wildcard_;
    }
    static constexpr std::size_t inline_capacity = 16;
    std::array<db_expression, inline_capacity> inline_values{};
    std::pmr::vector<db_expression> heap_values(query.resource());
    if (selection.size() > inline_capacity) {
        heap_values.reserve(selection.size());
    }
    auto* const values_data = selection.size() <= inline_capacity ? inline_values.data() : nullptr;
    std::size_t value_index = 0;
    const auto push_value = [&](db_expression expr) {
        if (values_data != nullptr) {
            values_data[value_index++] = expr;
        } else {
            heap_values.push_back(expr);
        }
    };
    const auto alias = qualifier == root_table ? std::string_view{} : qualifier;
    for (const auto& field : selection) {
        if (field.is_wildcard()) {
            if (!field.column_.empty()) {
                throw std::invalid_argument("wildcard projections cannot have a result alias");
            }
            push_value(field.expression(query, root_table, alias));
            result.wildcard_ = true;
            continue;
        }
        if (field.column_.empty() || std::ranges::find(result.columns_, std::string_view(field.column_)) != result.columns_.end()) {
            throw std::invalid_argument("projection requires unique nonempty field names");
        }
        result.columns_.emplace_back(field.column_);
        auto expression = !field.has_expression()
                              ? query.column(field.column_, qualifier)
                              : field.expression(query, root_table, alias);
        push_value(query.alias(expression, field.column_));
    }
    const std::span<const db_expression> values =
        values_data != nullptr ? std::span<const db_expression>(values_data, value_index)
                               : std::span<const db_expression>(heap_values);
    if (returning) {
        query.returning(values);
    } else if (previous) {
        for (const auto value : values) {
            query.add_select(value);
        }
    } else {
        query.select(values);
    }
    return result;
}

std::pmr::string format_truncate_table(std::string_view table, db_driver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(pmr_resource_or_default(resource));
    sql.reserve(17U + table.size());
    sql.append("TRUNCATE TABLE ");
    append_db_qualified_identifier(sql, table, driver);
    return sql;
}

}  // namespace ruvia::detail
