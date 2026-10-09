#include <array>
#include <type_traits>

#include "ruvia/web/db/db_schema.h"

#include "db/db_sql_format.h"

namespace ruvia {

void db_schema::set_table_options(std::string_view table_value, std::span<const db_table_option> options) {
    require_postgresql();
    if (options.empty()) {
        throw std::invalid_argument("table options cannot be empty");
    }
    std::pmr::string sql("ALTER TABLE ", resource_);
    detail::append_db_qualified_identifier(sql, table_value, driver_);
    sql += " SET (";
    for (std::size_t i = 0; i < options.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (options[i].name_ == options[j].name_) {
                throw std::invalid_argument("duplicate table option");
            }
        }
        if (i != 0) {
            sql += ", ";
        }
        detail::append_db_qualified_identifier(sql, options[i].name_, driver_);
        sql += " = ";
        std::visit([&](const auto& value) {
            using t_type = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<t_type, bool>) {
                sql += value ? "true" : "false";
            } else if constexpr (std::is_same_v<t_type, std::int64_t>) {
                detail::append_db_number(sql, value);
            } else {
                detail::append_db_string_literal(sql, value, driver_);
            }
        },
            options[i].value_);
    }
    sql += ')';
    append(std::move(sql));
}
void db_schema::set_database_option(std::string_view database_value, std::string_view name, std::string_view value) {
    require_postgresql();
    std::pmr::string sql("ALTER DATABASE ", resource_);
    detail::append_db_identifier(sql, database_value, driver_);
    sql += " SET ";
    detail::append_db_qualified_identifier(sql, name, driver_);
    sql += " TO ";
    detail::append_db_string_literal(sql, value, driver_);
    append(std::move(sql));
}
void db_schema::create_hypertable(const db_hypertable_options& options) {
    require_postgresql();
    // Validate names separately from the SQL text values consumed by Timescale.
    std::pmr::string table(resource_), column(resource_);
    detail::append_db_qualified_identifier(table, options.table_, driver_);
    detail::append_db_identifier(column, options.time_column_, driver_);
    db_query query(resource_);
    std::pmr::vector<db_expression> dimensions(resource_);
    dimensions.push_back(query.value(db_value(options.time_column_)));
    if (!options.chunk_interval_.empty()) {
        dimensions.push_back(query.import_expression(options.chunk_interval_));
    }
    const auto dimension = query.call("by_range", dimensions);
    const std::array args{query.value(db_value(std::string_view(table))), dimension};
    const std::array named{db_named_argument{"create_default_indexes", query.value(db_value(options.create_default_indexes_))},
        db_named_argument{"if_not_exists", query.value(db_value(options.if_not_exists_))},
        db_named_argument{"migrate_data", query.value(db_value(options.migrate_data_))}};
    perform(query.call("create_hypertable", args, named));
}
void db_schema::set_chunk_time_interval(std::string_view table_value, db_expression interval) {
    require_postgresql();
    std::pmr::string name(resource_);
    detail::append_db_qualified_identifier(name, table_value, driver_);
    db_query query(resource_);
    perform(query.call("set_chunk_time_interval", {query.value(db_value(std::string_view(name))), query.import_expression(interval)}));
}
void db_schema::set_compression(std::string_view table_value, const db_compression_options& options) {
    require_postgresql();
    if (!options.enabled_ && (!options.segment_by_.empty() || !options.order_by_.empty())) {
        throw std::invalid_argument("disabled compression cannot specify segment or order columns");
    }
    std::vector<db_table_option> values{{"timescaledb.compress", options.enabled_}};
    if (!options.segment_by_.empty()) {
        std::pmr::string segments(resource_);
        for (const auto& column : options.segment_by_) {
            if (!segments.empty()) {
                segments += ", ";
            }
            detail::append_db_identifier(segments, column, driver_);
        }
        values.push_back({"timescaledb.compress_segmentby", std::string(segments)});
    }
    if (!options.order_by_.empty()) {
        std::pmr::string order(resource_);
        for (const auto& column : options.order_by_) {
            if (!order.empty()) {
                order += ", ";
            }
            detail::append_db_identifier(order, column.column_, driver_);
            order += column.direction_ == db_order_direction::asc ? " ASC" : " DESC";
            if (column.nulls_ == db_nulls_order::first) {
                order += " NULLS FIRST";
            }
            if (column.nulls_ == db_nulls_order::last) {
                order += " NULLS LAST";
            }
        }
        values.push_back({"timescaledb.compress_orderby", std::string(order)});
    }
    set_table_options(table_value, values);
}
void db_schema::add_compression_policy(std::string_view table_value, db_expression after, bool if_not_exists) {
    require_postgresql();
    std::pmr::string name(resource_);
    detail::append_db_qualified_identifier(name, table_value, driver_);
    db_query query(resource_);
    const std::array args{query.value(db_value(std::string_view(name)))};
    const std::array named{db_named_argument{"compress_after", query.import_expression(after)}, db_named_argument{"if_not_exists", query.value(db_value(if_not_exists))}};
    perform(query.call("add_compression_policy", args, named));
}
void db_schema::remove_compression_policy(std::string_view table_value, bool if_exists) {
    require_postgresql();
    std::pmr::string name(resource_);
    detail::append_db_qualified_identifier(name, table_value, driver_);
    db_query query(resource_);
    const std::array args{query.value(db_value(std::string_view(name)))};
    const std::array named{db_named_argument{"if_exists", query.value(db_value(if_exists))}};
    perform(query.call("remove_compression_policy", args, named));
}
void db_schema::add_retention_policy(std::string_view table_value, db_expression after, bool if_not_exists) {
    require_postgresql();
    std::pmr::string name(resource_);
    detail::append_db_qualified_identifier(name, table_value, driver_);
    db_query query(resource_);
    const std::array args{query.value(db_value(std::string_view(name)))};
    const std::array named{db_named_argument{"drop_after", query.import_expression(after)}, db_named_argument{"if_not_exists", query.value(db_value(if_not_exists))}};
    perform(query.call("add_retention_policy", args, named));
}
void db_schema::remove_retention_policy(std::string_view table_value, bool if_exists) {
    require_postgresql();
    std::pmr::string name(resource_);
    detail::append_db_qualified_identifier(name, table_value, driver_);
    db_query query(resource_);
    const std::array args{query.value(db_value(std::string_view(name)))};
    const std::array named{db_named_argument{"if_exists", query.value(db_value(if_exists))}};
    perform(query.call("remove_retention_policy", args, named));
}

}  // namespace ruvia
