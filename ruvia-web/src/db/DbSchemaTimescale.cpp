#include <array>
#include <type_traits>

#include "ruvia/web/db/DbSchema.h"
#include "ruvia/web/detail/db/DbSqlFormat.h"

namespace ruvia {

void DbSchema::setTableOptions(std::string_view table, std::span<const DbTableOption> options) {
    requirePostgreSql();
    if (options.empty()) {
        throw std::invalid_argument("table options cannot be empty");
    }
    std::pmr::string sql("ALTER TABLE ", resource_);
    detail::appendDbQualifiedIdentifier(sql, table, driver_);
    sql += " SET (";
    for (std::size_t i = 0; i < options.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (options[i].name == options[j].name) {
                throw std::invalid_argument("duplicate table option");
            }
        }
        if (i != 0) {
            sql += ", ";
        }
        detail::appendDbQualifiedIdentifier(sql, options[i].name, driver_);
        sql += " = ";
        std::visit([&](const auto& value) {
            using T = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<T, bool>) {
                sql += value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                detail::appendDbNumber(sql, value);
            } else {
                detail::appendDbStringLiteral(sql, value, driver_);
            }
        },
            options[i].value);
    }
    sql += ')';
    append(std::move(sql));
}
void DbSchema::setDatabaseOption(std::string_view database, std::string_view name, std::string_view value) {
    requirePostgreSql();
    std::pmr::string sql("ALTER DATABASE ", resource_);
    detail::appendDbIdentifier(sql, database, driver_);
    sql += " SET ";
    detail::appendDbQualifiedIdentifier(sql, name, driver_);
    sql += " TO ";
    detail::appendDbStringLiteral(sql, value, driver_);
    append(std::move(sql));
}
void DbSchema::createHypertable(const DbHypertableOptions& options) {
    requirePostgreSql();
    // Validate names separately from the SQL text values consumed by Timescale.
    std::pmr::string table(resource_), column(resource_);
    detail::appendDbQualifiedIdentifier(table, options.table, driver_);
    detail::appendDbIdentifier(column, options.timeColumn, driver_);
    DbQuery query(resource_);
    std::pmr::vector<DbExpression> dimensions(resource_);
    dimensions.push_back(query.value(DbValue(options.timeColumn)));
    if (!options.chunkInterval.empty()) {
        dimensions.push_back(query.importExpression(options.chunkInterval));
    }
    const auto dimension = query.call("by_range", dimensions);
    const std::array args{query.value(DbValue(std::string_view(table))), dimension};
    const std::array named{DbNamedArgument{"create_default_indexes", query.value(DbValue(options.createDefaultIndexes))},
        DbNamedArgument{"if_not_exists", query.value(DbValue(options.ifNotExists))},
        DbNamedArgument{"migrate_data", query.value(DbValue(options.migrateData))}};
    perform(query.call("create_hypertable", args, named));
}
void DbSchema::setChunkTimeInterval(std::string_view table, DbExpression interval) {
    requirePostgreSql();
    std::pmr::string name(resource_);
    detail::appendDbQualifiedIdentifier(name, table, driver_);
    DbQuery query(resource_);
    perform(query.call("set_chunk_time_interval", {query.value(DbValue(std::string_view(name))), query.importExpression(interval)}));
}
void DbSchema::setCompression(std::string_view table, const DbCompressionOptions& options) {
    requirePostgreSql();
    if (!options.enabled && (!options.segmentBy.empty() || !options.orderBy.empty())) {
        throw std::invalid_argument("disabled compression cannot specify segment or order columns");
    }
    std::vector<DbTableOption> values{{"timescaledb.compress", options.enabled}};
    if (!options.segmentBy.empty()) {
        std::pmr::string segments(resource_);
        for (const auto& column : options.segmentBy) {
            if (!segments.empty()) {
                segments += ", ";
            }
            detail::appendDbIdentifier(segments, column, driver_);
        }
        values.push_back({"timescaledb.compress_segmentby", std::string(segments)});
    }
    if (!options.orderBy.empty()) {
        std::pmr::string order(resource_);
        for (const auto& column : options.orderBy) {
            if (!order.empty()) {
                order += ", ";
            }
            detail::appendDbIdentifier(order, column.column, driver_);
            order += column.direction == DbOrderDirection::kAsc ? " ASC" : " DESC";
            if (column.nulls == DbNullsOrder::kFirst) {
                order += " NULLS FIRST";
            }
            if (column.nulls == DbNullsOrder::kLast) {
                order += " NULLS LAST";
            }
        }
        values.push_back({"timescaledb.compress_orderby", std::string(order)});
    }
    setTableOptions(table, values);
}
void DbSchema::addCompressionPolicy(std::string_view table, DbExpression after, bool ifNotExists) {
    requirePostgreSql();
    std::pmr::string name(resource_);
    detail::appendDbQualifiedIdentifier(name, table, driver_);
    DbQuery query(resource_);
    const std::array args{query.value(DbValue(std::string_view(name)))};
    const std::array named{DbNamedArgument{"compress_after", query.importExpression(after)}, DbNamedArgument{"if_not_exists", query.value(DbValue(ifNotExists))}};
    perform(query.call("add_compression_policy", args, named));
}
void DbSchema::removeCompressionPolicy(std::string_view table, bool ifExists) {
    requirePostgreSql();
    std::pmr::string name(resource_);
    detail::appendDbQualifiedIdentifier(name, table, driver_);
    DbQuery query(resource_);
    const std::array args{query.value(DbValue(std::string_view(name)))};
    const std::array named{DbNamedArgument{"if_exists", query.value(DbValue(ifExists))}};
    perform(query.call("remove_compression_policy", args, named));
}
void DbSchema::addRetentionPolicy(std::string_view table, DbExpression after, bool ifNotExists) {
    requirePostgreSql();
    std::pmr::string name(resource_);
    detail::appendDbQualifiedIdentifier(name, table, driver_);
    DbQuery query(resource_);
    const std::array args{query.value(DbValue(std::string_view(name)))};
    const std::array named{DbNamedArgument{"drop_after", query.importExpression(after)}, DbNamedArgument{"if_not_exists", query.value(DbValue(ifNotExists))}};
    perform(query.call("add_retention_policy", args, named));
}
void DbSchema::removeRetentionPolicy(std::string_view table, bool ifExists) {
    requirePostgreSql();
    std::pmr::string name(resource_);
    detail::appendDbQualifiedIdentifier(name, table, driver_);
    DbQuery query(resource_);
    const std::array args{query.value(DbValue(std::string_view(name)))};
    const std::array named{DbNamedArgument{"if_exists", query.value(DbValue(ifExists))}};
    perform(query.call("remove_retention_policy", args, named));
}

}  // namespace ruvia
