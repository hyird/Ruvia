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
    for (std::size_t i = 0; i < options.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (options[i].name_ == options[j].name_) {
                throw std::invalid_argument("duplicate table option");
            }
        }
    }
    std::pmr::string sql(resource_);
    sql.reserve(32U + table_value.size() + options.size() * 48U);
    sql += "ALTER TABLE ";
    detail::append_db_qualified_identifier(sql, table_value, driver_);
    sql += " SET (";
    for (std::size_t i = 0; i < options.size(); ++i) {
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
    std::pmr::string sql(resource_);
    sql.reserve(32U + database_value.size() + name.size() + value.size());
    sql += "ALTER DATABASE ";
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
    std::pmr::string table(resource_);
    table.reserve(options.table_.size() + 4U);
    detail::append_db_qualified_identifier(table, options.table_, driver_);
    detail::validate_db_identifier(options.time_column_, driver_);
    std::pmr::string sql(resource_);
    sql.reserve(160U + table.size() + options.time_column_.size());
    sql += "PERFORM \"create_hypertable\"(";
    detail::append_db_string_literal(sql, table, driver_);
    sql += ", \"by_range\"(";
    detail::append_db_string_literal(sql, options.time_column_, driver_);
    if (!options.chunk_interval_.empty()) {
        sql += ", ";
        append_expression(sql, options.chunk_interval_);
    }
    sql += "), \"create_default_indexes\" => ";
    sql += options.create_default_indexes_ ? "TRUE" : "FALSE";
    sql += ", \"if_not_exists\" => ";
    sql += options.if_not_exists_ ? "TRUE" : "FALSE";
    sql += ", \"migrate_data\" => ";
    sql += options.migrate_data_ ? "TRUE" : "FALSE";
    sql += ')';
    append(std::move(sql), db_migration_atomicity::transactional, true);
}
void db_schema::set_chunk_time_interval(std::string_view table_value, db_expression interval) {
    require_postgresql();
    std::pmr::string name(resource_);
    name.reserve(table_value.size() + 4U);
    detail::append_db_qualified_identifier(name, table_value, driver_);
    std::pmr::string sql(resource_);
    sql.reserve(64U + name.size());
    sql += "PERFORM \"set_chunk_time_interval\"(";
    detail::append_db_string_literal(sql, name, driver_);
    sql += ", ";
    append_expression(sql, interval);
    sql += ')';
    append(std::move(sql), db_migration_atomicity::transactional, true);
}
void db_schema::set_compression(std::string_view table_value, const db_compression_options& options) {
    require_postgresql();
    if (!options.enabled_ && (!options.segment_by_.empty() || !options.order_by_.empty())) {
        throw std::invalid_argument("disabled compression cannot specify segment or order columns");
    }
    std::array<db_table_option, 3> values{db_table_option{"timescaledb.compress", options.enabled_}, {}, {}};
    std::size_t value_count = 1U;
    std::pmr::string segments(resource_);
    if (!options.segment_by_.empty()) {
        segments.reserve(options.segment_by_.size() * 16U);
        for (const auto& column : options.segment_by_) {
            if (!segments.empty()) {
                segments += ", ";
            }
            detail::append_db_identifier(segments, column, driver_);
        }
        values[value_count++] = {"timescaledb.compress_segmentby", segments};
    }
    std::pmr::string order(resource_);
    if (!options.order_by_.empty()) {
        order.reserve(options.order_by_.size() * 24U);
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
        values[value_count++] = {"timescaledb.compress_orderby", order};
    }
    set_table_options(table_value, std::span<const db_table_option>(values.data(), value_count));
}
void db_schema::add_compression_policy(std::string_view table_value, db_expression after, bool if_not_exists) {
    require_postgresql();
    std::pmr::string name(resource_);
    name.reserve(table_value.size() + 4U);
    detail::append_db_qualified_identifier(name, table_value, driver_);
    std::pmr::string sql(resource_);
    sql.reserve(96U + name.size());
    sql += "PERFORM \"add_compression_policy\"(";
    detail::append_db_string_literal(sql, name, driver_);
    sql += ", \"compress_after\" => ";
    append_expression(sql, after);
    sql += ", \"if_not_exists\" => ";
    sql += if_not_exists ? "TRUE" : "FALSE";
    sql += ')';
    append(std::move(sql), db_migration_atomicity::transactional, true);
}
void db_schema::remove_compression_policy(std::string_view table_value, bool if_exists) {
    require_postgresql();
    std::pmr::string name(resource_);
    name.reserve(table_value.size() + 4U);
    detail::append_db_qualified_identifier(name, table_value, driver_);
    std::pmr::string sql(resource_);
    sql.reserve(72U + name.size());
    sql += "PERFORM \"remove_compression_policy\"(";
    detail::append_db_string_literal(sql, name, driver_);
    sql += ", \"if_exists\" => ";
    sql += if_exists ? "TRUE" : "FALSE";
    sql += ')';
    append(std::move(sql), db_migration_atomicity::transactional, true);
}
void db_schema::add_retention_policy(std::string_view table_value, db_expression after, bool if_not_exists) {
    require_postgresql();
    std::pmr::string name(resource_);
    name.reserve(table_value.size() + 4U);
    detail::append_db_qualified_identifier(name, table_value, driver_);
    std::pmr::string sql(resource_);
    sql.reserve(96U + name.size());
    sql += "PERFORM \"add_retention_policy\"(";
    detail::append_db_string_literal(sql, name, driver_);
    sql += ", \"drop_after\" => ";
    append_expression(sql, after);
    sql += ", \"if_not_exists\" => ";
    sql += if_not_exists ? "TRUE" : "FALSE";
    sql += ')';
    append(std::move(sql), db_migration_atomicity::transactional, true);
}
void db_schema::remove_retention_policy(std::string_view table_value, bool if_exists) {
    require_postgresql();
    std::pmr::string name(resource_);
    name.reserve(table_value.size() + 4U);
    detail::append_db_qualified_identifier(name, table_value, driver_);
    std::pmr::string sql(resource_);
    sql.reserve(72U + name.size());
    sql += "PERFORM \"remove_retention_policy\"(";
    detail::append_db_string_literal(sql, name, driver_);
    sql += ", \"if_exists\" => ";
    sql += if_exists ? "TRUE" : "FALSE";
    sql += ')';
    append(std::move(sql), db_migration_atomicity::transactional, true);
}

}  // namespace ruvia
