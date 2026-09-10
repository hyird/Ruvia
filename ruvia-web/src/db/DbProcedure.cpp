#include "ruvia/web/db/DbProcedure.h"

#include <algorithm>

#include "ruvia/web/db/DbSchema.h"
#include "ruvia/web/detail/db/DbSqlFormat.h"

namespace ruvia {
namespace {
constexpr auto pg = DbDriver::kPostgreSql;
void requireSqlState(std::string_view state) {
    if (state.size() != 5 || state == "00000" || !std::ranges::all_of(state, [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z'); })) {
        throw std::invalid_argument("SQLSTATE must have five uppercase alphanumeric characters and indicate failure");
    }
}
}  // namespace

DbProcedure::DbProcedure(std::pmr::memory_resource* resource)
    : resource_(detail::pmrResourceOrDefault(resource)),
      declarations_(resource_),
      statements_(resource_),
      variables_(resource_),
      blocks_(resource_) {}
void DbProcedure::appendExpression(std::pmr::string& output, DbExpression value) const {
    output += DbQuery::renderExpression(value, pg, resource_);
}
void DbProcedure::registerVariable(std::string_view name) {
    if (std::ranges::any_of(variables_, [&](const auto& existing) { return existing == name; })) {
        throw std::invalid_argument("duplicate procedure variable");
    }
    variables_.emplace_back(name);
}
void DbProcedure::declareVariable(const DbVariableDefinition& variable) {
    std::pmr::string sql(resource_);
    detail::appendDbIdentifier(sql, variable.name, pg);
    sql += ' ';
    const auto& t = variable.type;
    detail::appendDbTypeName(sql, t.dataType, t.customName, t.length, t.precision, t.scale, t.array, pg);
    if (!variable.defaultValue.empty()) {
        sql += " := ";
        appendExpression(sql, variable.defaultValue);
    }
    sql += ";\n";
    registerVariable(variable.name);
    declarations_ += sql;
}
void DbProcedure::declareRow(std::string_view name, std::string_view table) {
    std::pmr::string sql(resource_);
    detail::appendDbIdentifier(sql, name, pg);
    sql += ' ';
    detail::appendDbQualifiedIdentifier(sql, table, pg);
    sql += "%ROWTYPE;\n";
    registerVariable(name);
    declarations_ += sql;
}
void DbProcedure::declareRecord(std::string_view name) {
    std::pmr::string sql(resource_);
    detail::appendDbIdentifier(sql, name, pg);
    sql += " RECORD;\n";
    registerVariable(name);
    declarations_ += sql;
}
void DbProcedure::assign(std::string_view target, DbExpression value) {
    std::pmr::string sql(resource_);
    detail::appendDbQualifiedIdentifier(sql, target, pg);
    sql += " := ";
    appendExpression(sql, value);
    sql += ";\n";
    statements_ += sql;
}
void DbProcedure::selectInto(const DbQuery& query, std::span<const std::string_view> targets, bool strict) {
    if (targets.empty() || !query.returnsRows()) {
        throw std::invalid_argument("SELECT INTO requires returned rows and target variables");
    }
    const auto statement = query.compile(pg, resource_, DbParameterMode::kLiteral);
    std::pmr::string sql(statement.sql(), resource_);
    sql += strict ? " INTO STRICT " : " INTO ";
    bool first = true;
    for (const auto target : targets) {
        if (!first) {
            sql += ", ";
        }
        first = false;
        detail::appendDbQualifiedIdentifier(sql, target, pg);
    }
    sql += ";\n";
    statements_ += sql;
}
void DbProcedure::execute(const DbQuery& query) {
    if (query.returnsRows()) {
        throw std::invalid_argument("procedure execute requires a command; consume returned rows with selectInto");
    }
    const auto statement = query.compile(pg, resource_, DbParameterMode::kLiteral);
    statements_ += statement.sql();
    statements_ += ";\n";
}
void DbProcedure::perform(DbExpression expression) {
    std::pmr::string sql("PERFORM ", resource_);
    appendExpression(sql, expression);
    sql += ";\n";
    statements_ += sql;
}
void DbProcedure::apply(const DbSchema& schema) {
    if (schema.driver_ != pg) {
        throw std::invalid_argument("PL/pgSQL requires a PostgreSQL schema");
    }
    for (const auto& statement : schema.statements_) {
        if (statement.atomicity != DbMigrationAtomicity::kTransactional) {
            throw std::invalid_argument("nontransactional schema operations cannot run in a procedure");
        }
    }
    for (const auto& statement : schema.statements_) {
        statements_ += statement.sql;
        statements_ += ";\n";
    }
}
void DbProcedure::beginIf(DbExpression condition) {
    std::pmr::string sql("IF ", resource_);
    appendExpression(sql, condition);
    sql += " THEN\n";
    blocks_.push_back(Block::kIf);
    statements_ += sql;
}
void DbProcedure::elseIf(DbExpression condition) {
    if (blocks_.empty() || blocks_.back() != Block::kIf) {
        throw std::invalid_argument("ELSEIF requires an open IF without ELSE");
    }
    std::pmr::string sql("ELSIF ", resource_);
    appendExpression(sql, condition);
    sql += " THEN\n";
    statements_ += sql;
}
void DbProcedure::otherwise() {
    if (blocks_.empty() || blocks_.back() != Block::kIf) {
        throw std::invalid_argument("ELSE requires an open IF without ELSE");
    }
    statements_ += "ELSE\n";
    blocks_.back() = Block::kElse;
}
void DbProcedure::endIf() {
    if (blocks_.empty() || (blocks_.back() != Block::kIf && blocks_.back() != Block::kElse)) {
        throw std::invalid_argument("END IF requires an open IF");
    }
    statements_ += "END IF;\n";
    blocks_.pop_back();
}
void DbProcedure::beginBlock() {
    blocks_.push_back(Block::kBegin);
    statements_ += "BEGIN\n";
}
void DbProcedure::catchSqlState(std::string_view state) {
    if (blocks_.empty() || (blocks_.back() != Block::kBegin && blocks_.back() != Block::kException)) {
        throw std::invalid_argument("exception handler requires an open block");
    }
    requireSqlState(state);
    std::pmr::string sql(resource_);
    if (blocks_.back() == Block::kBegin) {
        sql += "EXCEPTION\n";
    }
    sql += "WHEN SQLSTATE ";
    detail::appendDbStringLiteral(sql, state, pg);
    sql += " THEN\nNULL;\n";
    statements_ += sql;
    blocks_.back() = Block::kException;
}
void DbProcedure::endBlock() {
    if (blocks_.empty() || (blocks_.back() != Block::kBegin && blocks_.back() != Block::kException)) {
        throw std::invalid_argument("END requires an open block");
    }
    statements_ += "END;\n";
    blocks_.pop_back();
}
void DbProcedure::returnValue(DbExpression expression) {
    std::pmr::string sql("RETURN", resource_);
    if (!expression.empty()) {
        sql += ' ';
        appendExpression(sql, expression);
    }
    sql += ";\n";
    statements_ += sql;
}
void DbProcedure::raiseException(std::string_view message, std::string_view sqlState) {
    requireSqlState(sqlState);
    std::pmr::string sql("RAISE EXCEPTION USING MESSAGE = ", resource_);
    detail::appendDbStringLiteral(sql, message, pg);
    sql += ", ERRCODE = ";
    detail::appendDbStringLiteral(sql, sqlState, pg);
    sql += ";\n";
    statements_ += sql;
}
std::pmr::string DbProcedure::body(std::pmr::memory_resource* resource) const {
    if (!blocks_.empty()) {
        throw std::invalid_argument("procedure has unclosed control-flow blocks");
    }
    std::pmr::string result(detail::pmrResourceOrDefault(resource));
    if (!declarations_.empty()) {
        result += "DECLARE\n";
        result += declarations_;
    }
    result += "BEGIN\n";
    result += statements_.empty() ? std::string_view("NULL;\n") : std::string_view(statements_);
    result += "END";
    return result;
}

}  // namespace ruvia
