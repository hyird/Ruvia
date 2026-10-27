#include <algorithm>

#include "ruvia/web/db/db_schema.h"

#include "db/db_sql_format.h"

namespace ruvia {
namespace {
constexpr auto pg = db_driver::postgresql;
void require_sql_state(std::string_view state_value) {
    if (state_value.size() != 5 || state_value == "00000" || !std::ranges::all_of(state_value, [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z'); })) {
        throw std::invalid_argument("SQLSTATE must have five uppercase alphanumeric characters and indicate failure");
    }
}
std::string_view table_lock_mode_name(db_table_lock_mode mode) {
    switch (mode) {
        case db_table_lock_mode::access_share:
            return "ACCESS SHARE";
        case db_table_lock_mode::row_share:
            return "ROW SHARE";
        case db_table_lock_mode::row_exclusive:
            return "ROW EXCLUSIVE";
        case db_table_lock_mode::share_update_exclusive:
            return "SHARE UPDATE EXCLUSIVE";
        case db_table_lock_mode::share:
            return "SHARE";
        case db_table_lock_mode::share_row_exclusive:
            return "SHARE ROW EXCLUSIVE";
        case db_table_lock_mode::exclusive:
            return "EXCLUSIVE";
        case db_table_lock_mode::access_exclusive:
            return "ACCESS EXCLUSIVE";
    }
    throw std::invalid_argument("unknown table lock mode");
}
}  // namespace

db_procedure::db_procedure(std::pmr::memory_resource* resource)
    : resource_(pmr_resource_or_default(resource)),
      declarations_(resource_),
      statements_(resource_),
      variables_(resource_),
      blocks_(resource_) {}
void db_procedure::append_expression(std::pmr::string& output, db_expression value) const {
    output += db_query::render_expression(value, pg, resource_);
}
void db_procedure::register_variable(std::string_view name) {
    if (std::ranges::any_of(variables_, [&](const auto& existing) { return existing == name; })) {
        throw std::invalid_argument("duplicate procedure variable");
    }
    variables_.emplace_back(name);
}
void db_procedure::declare_variable(const db_variable_definition& variable) {
    std::pmr::string sql(resource_);
    detail::append_db_identifier(sql, variable.name_, pg);
    sql += ' ';
    const auto& t = variable.type_;
    detail::append_db_type_name(sql, t.data_type_, t.custom_name_, t.length_, t.precision_, t.scale_, t.array_, pg);
    if (!variable.default_value_.empty()) {
        sql += " := ";
        append_expression(sql, variable.default_value_);
    }
    sql += ";\n";
    register_variable(variable.name_);
    declarations_ += sql;
}
void db_procedure::declare_row(std::string_view name, std::string_view table_value) {
    std::pmr::string sql(resource_);
    detail::append_db_identifier(sql, name, pg);
    sql += ' ';
    detail::append_db_qualified_identifier(sql, table_value, pg);
    sql += "%ROWTYPE;\n";
    register_variable(name);
    declarations_ += sql;
}
void db_procedure::declare_record(std::string_view name) {
    detail::validate_db_identifier(name, pg);
    std::pmr::string sql(resource_);
    sql.reserve(name.size() + 2U + 9U);
    detail::append_db_identifier(sql, name, pg);
    sql += " RECORD;\n";
    register_variable(name);
    declarations_ += sql;
}
void db_procedure::assign(std::string_view target, db_expression value) {
    std::pmr::string sql(resource_);
    detail::append_db_qualified_identifier(sql, target, pg);
    sql += " := ";
    append_expression(sql, value);
    sql += ";\n";
    statements_ += sql;
}
void db_procedure::select_into(const db_query& query, std::span<const std::string_view> targets, bool strict) {
    if (targets.empty() || !query.returns_rows()) {
        throw std::invalid_argument("SELECT INTO requires returned rows and target variables");
    }
    const auto statement = query.compile(pg, resource_, db_parameter_mode::literal);
    std::pmr::string sql(statement.sql(), resource_);
    sql += strict ? " INTO STRICT " : " INTO ";
    bool first = true;
    for (const auto target : targets) {
        if (!first) {
            sql += ", ";
        }
        first = false;
        detail::append_db_qualified_identifier(sql, target, pg);
    }
    sql += ";\n";
    statements_ += sql;
}
void db_procedure::execute(const db_query& query) {
    if (query.returns_rows()) {
        throw std::invalid_argument("procedure execute requires a command; consume returned rows with select_into");
    }
    const auto statement = query.compile(pg, resource_, db_parameter_mode::literal);
    statements_ += statement.sql();
    statements_ += ";\n";
}
void db_procedure::perform(db_expression expression) {
    std::pmr::string sql("PERFORM ", resource_);
    append_expression(sql, expression);
    sql += ";\n";
    statements_ += sql;
}
void db_procedure::apply(const db_schema& schema) {
    if (schema.driver_ != pg) {
        throw std::invalid_argument("PL/pgSQL requires a PostgreSQL schema");
    }
    for (const auto& statement : schema.statements_) {
        if (statement.atomicity_ != db_migration_atomicity::transactional) {
            throw std::invalid_argument("nontransactional schema operations cannot run in a procedure");
        }
    }
    for (const auto& statement : schema.statements_) {
        statements_ += statement.sql_;
        statements_ += ";\n";
    }
}
void db_procedure::begin_if(db_expression condition) {
    std::pmr::string sql("IF ", resource_);
    append_expression(sql, condition);
    sql += " THEN\n";
    blocks_.push_back(block_type::if_value);
    statements_ += sql;
}
void db_procedure::else_if(db_expression condition) {
    if (blocks_.empty() || blocks_.back() != block_type::if_value) {
        throw std::invalid_argument("ELSEIF requires an open IF without ELSE");
    }
    std::pmr::string sql("ELSIF ", resource_);
    append_expression(sql, condition);
    sql += " THEN\n";
    statements_ += sql;
}
void db_procedure::otherwise() {
    if (blocks_.empty() || blocks_.back() != block_type::if_value) {
        throw std::invalid_argument("ELSE requires an open IF without ELSE");
    }
    statements_ += "ELSE\n";
    blocks_.back() = block_type::else_value;
}
void db_procedure::end_if() {
    if (blocks_.empty() || (blocks_.back() != block_type::if_value && blocks_.back() != block_type::else_value)) {
        throw std::invalid_argument("END IF requires an open IF");
    }
    statements_ += "END IF;\n";
    blocks_.pop_back();
}
void db_procedure::begin_block() {
    blocks_.push_back(block_type::begin);
    statements_ += "BEGIN\n";
}
void db_procedure::catch_sql_state(std::string_view state_value) {
    if (blocks_.empty() || (blocks_.back() != block_type::begin && blocks_.back() != block_type::exception)) {
        throw std::invalid_argument("exception handler requires an open block");
    }
    require_sql_state(state_value);
    const std::string_view prefix = blocks_.back() == block_type::begin ? "EXCEPTION\nWHEN SQLSTATE "
                                                                        : "WHEN SQLSTATE ";
    std::pmr::string sql(resource_);
    sql.reserve(prefix.size() + 8U + 12U);
    sql.append(prefix);
    detail::append_db_string_literal(sql, state_value, pg);
    sql += " THEN\nNULL;\n";
    statements_ += sql;
    blocks_.back() = block_type::exception;
}
void db_procedure::end_block() {
    if (blocks_.empty() || (blocks_.back() != block_type::begin && blocks_.back() != block_type::exception)) {
        throw std::invalid_argument("END requires an open block");
    }
    statements_ += "END;\n";
    blocks_.pop_back();
}
void db_procedure::begin_for_query(std::string_view target, const db_query& query) {
    if (!query.returns_rows()) {
        throw std::invalid_argument("FOR IN query loop requires a row-returning query");
    }
    detail::validate_db_identifier(target, pg);
    const auto statement = query.compile(pg, resource_, db_parameter_mode::literal);
    std::pmr::string sql(resource_);
    sql.reserve(4U + target.size() + 2U + 4U + statement.sql().size() + 6U);
    sql.append("FOR ");
    detail::append_db_identifier(sql, target, pg);
    sql.append(" IN ");
    sql.append(statement.sql());
    sql.append(" LOOP\n");
    blocks_.push_back(block_type::loop);
    statements_ += sql;
}
void db_procedure::begin_foreach(std::string_view target, db_expression array, unsigned slice) {
    std::pmr::string sql("FOREACH ", resource_);
    detail::append_db_identifier(sql, target, pg);
    if (slice > 0) {
        sql += " SLICE ";
        detail::append_db_number(sql, static_cast<std::uint64_t>(slice));
    }
    sql += " IN ARRAY ";
    append_expression(sql, array);
    sql += " LOOP\n";
    blocks_.push_back(block_type::loop);
    statements_ += sql;
}
void db_procedure::end_loop() {
    if (blocks_.empty() || blocks_.back() != block_type::loop) {
        throw std::invalid_argument("END LOOP requires an open FOR or FOREACH loop");
    }
    statements_ += "END LOOP;\n";
    blocks_.pop_back();
}
void db_procedure::execute_dynamic(db_expression command, std::span<const db_expression> using_args) {
    std::pmr::string sql("EXECUTE ", resource_);
    append_expression(sql, command);
    if (!using_args.empty()) {
        sql += " USING ";
        for (std::size_t i = 0; i < using_args.size(); ++i) {
            if (i != 0) {
                sql += ", ";
            }
            append_expression(sql, using_args[i]);
        }
    }
    sql += ";\n";
    statements_ += sql;
}
void db_procedure::lock_table(std::string_view table, db_table_lock_mode mode, bool nowait) {
    const auto mode_name = table_lock_mode_name(mode);
    std::pmr::string sql(resource_);
    sql.reserve(11U + table.size() + 4U + 4U + mode_name.size() + 5U + (nowait ? 7U : 0U) + 2U);
    sql.append("LOCK TABLE ");
    detail::append_db_qualified_identifier(sql, table, pg);
    sql.append(" IN ");
    sql.append(mode_name);
    sql.append(" MODE");
    if (nowait) {
        sql.append(" NOWAIT");
    }
    sql.append(";\n");
    statements_ += sql;
}
void db_procedure::return_value(db_expression expression) {
    std::pmr::string sql("RETURN", resource_);
    if (!expression.empty()) {
        sql += ' ';
        append_expression(sql, expression);
    }
    sql += ";\n";
    statements_ += sql;
}
void db_procedure::raise_exception(std::string_view message, std::string_view sql_state) {
    require_sql_state(sql_state);
    std::pmr::string sql("RAISE EXCEPTION USING MESSAGE = ", resource_);
    detail::append_db_string_literal(sql, message, pg);
    sql += ", ERRCODE = ";
    detail::append_db_string_literal(sql, sql_state, pg);
    sql += ";\n";
    statements_ += sql;
}
std::pmr::string db_procedure::body(std::pmr::memory_resource* resource) const {
    if (!blocks_.empty()) {
        throw std::invalid_argument("procedure has unclosed control-flow blocks");
    }
    const std::string_view statements =
        statements_.empty() ? std::string_view("NULL;\n") : std::string_view(statements_);
    std::pmr::string result(pmr_resource_or_default(resource));
    result.reserve((!declarations_.empty() ? 8U + declarations_.size() : 0U) + 6U + statements.size() + 3U);
    if (!declarations_.empty()) {
        result.append("DECLARE\n");
        result.append(declarations_);
    }
    result.append("BEGIN\n");
    result.append(statements);
    result.append("END");
    return result;
}

}  // namespace ruvia
