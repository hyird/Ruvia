#include "ruvia/web/db/DbSchema.h"

#include <algorithm>

#include "ruvia/web/detail/db/DbSqlFormat.h"

namespace ruvia {
namespace {
using detail::appendDbIdentifier;
using detail::appendDbQualifiedIdentifier;
using detail::appendDbStringLiteral;
std::string_view action(DbReferentialAction a) {
    switch (a) {
        case DbReferentialAction::kRestrict:
            return "RESTRICT";
        case DbReferentialAction::kCascade:
            return "CASCADE";
        case DbReferentialAction::kSetNull:
            return "SET NULL";
        case DbReferentialAction::kSetDefault:
            return "SET DEFAULT";
        default:
            return "NO ACTION";
    }
}
std::string_view method(DbIndexMethod m) {
    switch (m) {
        case DbIndexMethod::kHash:
            return "hash";
        case DbIndexMethod::kGin:
            return "gin";
        case DbIndexMethod::kGist:
            return "gist";
        case DbIndexMethod::kSpGist:
            return "spgist";
        case DbIndexMethod::kBrin:
            return "brin";
        default:
            return "btree";
    }
}
}  // namespace

DbSchema::DbSchema(DbSchemaOptions options)
    : driver_(options.driver),
      resource_(detail::pmrResourceOrDefault(options.resource)),
      statements_(resource_) {
    detail::requireDbDialect(driver_);
}
void DbSchema::append(std::pmr::string sql, DbMigrationAtomicity a, bool procedural) {
    statements_.push_back(Statement{std::move(sql), a, procedural});
}
void DbSchema::requirePostgreSql() const {
    if (driver_ != DbDriver::kPostgreSql) {
        throw std::invalid_argument("schema operation requires PostgreSQL");
    }
}
std::pmr::string DbSchema::tablePrefix(std::string_view table) const {
    std::pmr::string s(resource_);
    appendDbQualifiedIdentifier(s, table, driver_);
    return s;
}
void DbSchema::appendExpression(std::pmr::string& s, DbQuery::Expr e) const {
    if (e.empty()) {
        throw std::invalid_argument("schema expression is required");
    }
    auto x = DbQuery::renderExpression(e, driver_, resource_, DbParameterMode::kLiteral);
    s += x;
}
void DbSchema::appendType(std::pmr::string& s, const DbTypeDefinition& t) const {
    detail::appendDbTypeName(s, t.dataType, t.customName, t.length, t.precision, t.scale,
        t.array, driver_);
}
void DbSchema::appendColumn(std::pmr::string& s, const DbSchemaColumn& c) const {
    if (c.generatedType != DbGeneratedType::kNone && c.generatedType != DbGeneratedType::kStored &&
        c.generatedType != DbGeneratedType::kVirtual) {
        throw std::invalid_argument("unsupported generated column storage mode");
    }
    if (c.name.empty()) {
        throw std::invalid_argument("column name is required");
    }
    if (c.generatedType == DbGeneratedType::kNone && !c.asExpression.empty()) {
        throw std::invalid_argument("generated column expression requires generated metadata");
    }
    if (c.generatedType != DbGeneratedType::kNone && (c.identity != DbIdentity::kNone || !c.defaultValue.empty())) {
        throw std::invalid_argument("generated column conflicts with identity or default");
    }
    appendDbIdentifier(s, c.name, driver_);
    s += ' ';
    appendType(s, c.type);
    if (c.generatedType != DbGeneratedType::kNone) {
        if (c.asExpression.empty()) {
            throw std::invalid_argument("generated column expression is required");
        }
        if (driver_ == DbDriver::kPostgreSql && c.generatedType == DbGeneratedType::kVirtual) {
            throw std::invalid_argument("PostgreSQL schema compilation supports stored generated columns only");
        }
        if (driver_ == DbDriver::kMariaDb && (!c.nullable || c.primaryKey)) {
            throw std::invalid_argument("MariaDB generated columns cannot declare NOT NULL or PRIMARY KEY");
        }
        s += " GENERATED ALWAYS AS (";
        appendExpression(s, c.asExpression);
        s += ")";
        if (driver_ == DbDriver::kPostgreSql || c.generatedType == DbGeneratedType::kStored) {
            s += " STORED";
        } else {
            s += " VIRTUAL";
        }
    }
    if (c.identity != DbIdentity::kNone) {
        if (!c.defaultValue.empty() || c.nullable || c.type.array ||
            (c.type.dataType != DbDataType::kSmallInt && c.type.dataType != DbDataType::kInteger && c.type.dataType != DbDataType::kBigInt)) {
            throw std::invalid_argument("identity requires a non-null integer column without a default");
        }
        if (driver_ == DbDriver::kPostgreSql) {
            s += " GENERATED ";
            s += (c.identity == DbIdentity::kAlways ? "ALWAYS" : "BY DEFAULT");
            s += " AS IDENTITY";
        } else {
            if (c.identity == DbIdentity::kAlways) {
                throw std::invalid_argument("MariaDB cannot enforce GENERATED ALWAYS identity semantics");
            }
            s += " AUTO_INCREMENT";
        }
    }
    if (!c.nullable) {
        s += " NOT NULL";
    }
    if (c.unique) {
        s += " UNIQUE";
    }
    if (c.primaryKey) {
        s += " PRIMARY KEY";
    }
    if (!c.defaultValue.empty()) {
        s += " DEFAULT ";
        appendExpression(s, c.defaultValue);
    }
}
void DbSchema::appendConstraint(std::pmr::string& s, const DbSchemaConstraint& c) const {
    if ((c.deferrable || c.initiallyDeferred || c.notValid) && driver_ != DbDriver::kPostgreSql) {
        throw std::invalid_argument("deferred and NOT VALID constraints require PostgreSQL");
    }
    if (c.initiallyDeferred && !c.deferrable) {
        throw std::invalid_argument("INITIALLY DEFERRED requires DEFERRABLE");
    }
    if (c.kind == DbConstraintKind::kCheck && c.deferrable) {
        throw std::invalid_argument("CHECK constraints cannot be deferred");
    }
    if (c.notValid && c.kind != DbConstraintKind::kCheck && c.kind != DbConstraintKind::kForeignKey) {
        throw std::invalid_argument("NOT VALID requires a CHECK or foreign key constraint");
    }
    if (c.kind != DbConstraintKind::kCheck && c.columns.empty()) {
        throw std::invalid_argument("constraint requires columns");
    }
    if (c.kind == DbConstraintKind::kForeignKey && c.columns.size() != c.referencedColumns.size()) {
        throw std::invalid_argument("foreign key column counts differ");
    }
    if (c.name.empty()) {
        throw std::invalid_argument("constraint name is required");
    }
    appendDbIdentifier(s, c.name, driver_);
    s += ' ';
    if (c.kind == DbConstraintKind::kCheck) {
        s += "CHECK (";
        appendExpression(s, c.check);
        s += ')';
    } else if (c.kind == DbConstraintKind::kPrimaryKey || c.kind == DbConstraintKind::kUnique) {
        s += (c.kind == DbConstraintKind::kPrimaryKey ? "PRIMARY KEY (" : "UNIQUE (");
        for (size_t i = 0; i < c.columns.size(); ++i) {
            if (i) {
                s += ',';
            }
            appendDbIdentifier(s, c.columns[i], driver_);
        }
        s += ')';
    } else {
        if (c.columns.empty() || c.referencedTable.empty() || c.referencedColumns.empty()) {
            throw std::invalid_argument("foreign key fields are required");
        }
        s += "FOREIGN KEY (";
        for (size_t i = 0; i < c.columns.size(); ++i) {
            if (i) {
                s += ',';
            }
            appendDbIdentifier(s, c.columns[i], driver_);
        }
        s += ") REFERENCES ";
        appendDbQualifiedIdentifier(s, c.referencedTable, driver_);
        s += " (";
        for (size_t i = 0; i < c.referencedColumns.size(); ++i) {
            if (i) {
                s += ',';
            }
            appendDbIdentifier(s, c.referencedColumns[i], driver_);
        }
        s += ") ON DELETE ";
        s += action(c.onDelete);
        s += " ON UPDATE ";
        s += action(c.onUpdate);
    }
    if (c.deferrable) {
        s += " DEFERRABLE";
    }
    if (c.initiallyDeferred) {
        s += " INITIALLY DEFERRED";
    }
    if (c.notValid) {
        s += " NOT VALID";
    }
}

void DbSchema::createSchema(std::string_view n, bool ine) {
    std::pmr::string s("CREATE SCHEMA ", resource_);
    if (ine) {
        s += "IF NOT EXISTS ";
    }
    appendDbIdentifier(s, n, driver_);
    append(std::move(s));
}
void DbSchema::dropSchema(std::string_view n, DbDropBehavior b, bool ie) {
    std::pmr::string s("DROP SCHEMA ", resource_);
    if (ie) {
        s += "IF EXISTS ";
    }
    appendDbIdentifier(s, n, driver_);
    if (b == DbDropBehavior::kCascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void DbSchema::createTable(const DbTableDefinition& t) {
    if (t.name.empty() || t.columns.empty()) {
        throw std::invalid_argument("table and columns are required");
    }
    std::pmr::string s("CREATE ", resource_);
    if (t.temporary && t.unlogged) {
        throw std::invalid_argument("table cannot be both temporary and unlogged");
    }
    if (t.unlogged) {
        requirePostgreSql();
    }
    std::size_t primaryColumns = 0;
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        primaryColumns += t.columns[i].primaryKey ? 1 : 0;
        for (std::size_t j = 0; j < i; ++j) {
            if (t.columns[i].name == t.columns[j].name) {
                throw std::invalid_argument("duplicate table column");
            }
        }
    }
    if (primaryColumns > 1) {
        throw std::invalid_argument("composite primary keys require a table constraint");
    }
    for (const auto& constraint : t.constraints) {
        if (constraint.notValid) {
            throw std::invalid_argument("NOT VALID is only supported when adding a constraint");
        }
        if (constraint.kind == DbConstraintKind::kPrimaryKey && primaryColumns++ != 0) {
            throw std::invalid_argument("duplicate table primary key");
        }
        if (driver_ == DbDriver::kMariaDb && constraint.kind == DbConstraintKind::kPrimaryKey) {
            for (const auto& column : t.columns) {
                if (column.generatedType != DbGeneratedType::kNone &&
                    std::ranges::find(constraint.columns, column.name) != constraint.columns.end()) {
                    throw std::invalid_argument("MariaDB generated columns cannot be primary keys");
                }
            }
        }
    }
    if (t.temporary) {
        s += "TEMPORARY ";
    }
    if (t.unlogged) {
        s += "UNLOGGED ";
    }
    s += "TABLE ";
    if (t.ifNotExists) {
        s += "IF NOT EXISTS ";
    }
    appendDbQualifiedIdentifier(s, t.name, driver_);
    s += " (";
    for (size_t i = 0; i < t.columns.size(); ++i) {
        if (i) {
            s += ", ";
        }
        appendColumn(s, t.columns[i]);
    }
    for (auto& c : t.constraints) {
        s += ", CONSTRAINT ";
        appendConstraint(s, c);
    }
    s += ')';
    append(std::move(s));
}
void DbSchema::dropTable(std::string_view t, DbDropBehavior b, bool ie) {
    std::pmr::string s("DROP TABLE ", resource_);
    if (ie) {
        s += "IF EXISTS ";
    }
    appendDbQualifiedIdentifier(s, t, driver_);
    if (b == DbDropBehavior::kCascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void DbSchema::renameTable(std::string_view t, std::string_view n) {
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " RENAME TO ";
    appendDbIdentifier(s, n, driver_);
    append(std::move(s));
}
void DbSchema::addColumn(std::string_view t, const DbSchemaColumn& c, bool ie) {
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " ADD COLUMN ";
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    appendColumn(s, c);
    append(std::move(s));
}
void DbSchema::dropColumn(std::string_view t, std::string_view c, DbDropBehavior b, bool ie) {
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " DROP COLUMN ";
    if (ie) {
        s += "IF EXISTS ";
    }
    appendDbIdentifier(s, c, driver_);
    if (b == DbDropBehavior::kCascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void DbSchema::renameColumn(std::string_view t, std::string_view c, std::string_view n) {
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " RENAME COLUMN ";
    appendDbIdentifier(s, c, driver_);
    s += " TO ";
    appendDbIdentifier(s, n, driver_);
    append(std::move(s));
}
void DbSchema::alterColumnType(std::string_view t, std::string_view c, const DbTypeDefinition& ty, DbQuery::Expr u) {
    requirePostgreSql();
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " ALTER COLUMN ";
    appendDbIdentifier(s, c, driver_);
    s += " TYPE ";
    appendType(s, ty);
    if (!u.empty()) {
        s += " USING ";
        appendExpression(s, u);
    }
    append(std::move(s));
}
void DbSchema::setColumnNullable(std::string_view t, std::string_view c, bool n) {
    requirePostgreSql();
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " ALTER COLUMN ";
    appendDbIdentifier(s, c, driver_);
    s += n ? " DROP NOT NULL" : " SET NOT NULL";
    append(std::move(s));
}
void DbSchema::setColumnDefault(std::string_view t, std::string_view c, DbQuery::Expr e) {
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " ALTER COLUMN ";
    appendDbIdentifier(s, c, driver_);
    s += " SET DEFAULT ";
    appendExpression(s, e);
    append(std::move(s));
}
void DbSchema::dropColumnDefault(std::string_view t, std::string_view c) {
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " ALTER COLUMN ";
    appendDbIdentifier(s, c, driver_);
    s += " DROP DEFAULT";
    append(std::move(s));
}
void DbSchema::addConstraint(std::string_view t, const DbSchemaConstraint& c) {
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " ADD CONSTRAINT ";
    appendConstraint(s, c);
    append(std::move(s));
}
void DbSchema::dropConstraint(std::string_view t, std::string_view n, DbDropBehavior b, bool ie) {
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " DROP CONSTRAINT ";
    if (ie) {
        s += "IF EXISTS ";
    }
    appendDbIdentifier(s, n, driver_);
    if (b == DbDropBehavior::kCascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void DbSchema::validateConstraint(std::string_view t, std::string_view n) {
    requirePostgreSql();
    std::pmr::string s("ALTER TABLE ", resource_);
    appendDbQualifiedIdentifier(s, t, driver_);
    s += " VALIDATE CONSTRAINT ";
    appendDbIdentifier(s, n, driver_);
    append(std::move(s));
}
void DbSchema::createIndex(const DbIndexDefinition& i) {
    if (driver_ != DbDriver::kPostgreSql && (i.concurrently || i.method != DbIndexMethod::kBtree || !i.include.empty() || !i.where.empty())) {
        throw std::invalid_argument("selected index options require PostgreSQL");
    }
    if (i.name.empty() || i.table.empty() || i.keys.empty()) {
        throw std::invalid_argument("index name, table and keys are required");
    }
    std::pmr::string s("CREATE ", resource_);
    if (i.unique) {
        s += "UNIQUE ";
    }
    s += "INDEX ";
    if (i.concurrently) {
        s += "CONCURRENTLY ";
    }
    if (i.ifNotExists) {
        s += "IF NOT EXISTS ";
    }
    appendDbIdentifier(s, i.name, driver_);
    s += " ON ";
    appendDbQualifiedIdentifier(s, i.table, driver_);
    s += " USING ";
    s += method(i.method);
    s += " (";
    for (size_t n = 0; n < i.keys.size(); ++n) {
        if (n) {
            s += ", ";
        }
        const auto& key = i.keys[n];
        if (key.column.empty() == key.expression.empty()) {
            throw std::invalid_argument("an index key requires exactly one column or expression");
        }
        if (driver_ != DbDriver::kPostgreSql && (!key.expression.empty() || !key.operatorClass.empty() || key.nulls != DbNullsOrder::kDefault)) {
            throw std::invalid_argument("selected index key options require PostgreSQL");
        }
        if (!i.keys[n].column.empty()) {
            appendDbIdentifier(s, i.keys[n].column, driver_);
        } else {
            s += '(';
            appendExpression(s, i.keys[n].expression);
            s += ')';
        }
        if (!i.keys[n].operatorClass.empty()) {
            s += ' ';
            appendDbQualifiedIdentifier(s, i.keys[n].operatorClass, driver_);
        }
        if (i.keys[n].order == DbOrderDirection::kDesc) {
            s += " DESC";
        }
        if (i.keys[n].nulls == DbNullsOrder::kFirst) {
            s += " NULLS FIRST";
        }
        if (i.keys[n].nulls == DbNullsOrder::kLast) {
            s += " NULLS LAST";
        }
    }
    s += ')';
    if (!i.include.empty()) {
        s += " INCLUDE (";
        for (size_t n = 0; n < i.include.size(); ++n) {
            if (n) {
                s += ',';
            }
            appendDbIdentifier(s, i.include[n], driver_);
        }
        s += ')';
    }
    if (!i.where.empty()) {
        s += " WHERE ";
        appendExpression(s, i.where);
    }
    append(std::move(s), i.concurrently ? DbMigrationAtomicity::kUnwrapped : DbMigrationAtomicity::kTransactional);
}
void DbSchema::dropIndex(std::string_view n, bool c, bool ie) {
    requirePostgreSql();
    std::pmr::string s("DROP INDEX ", resource_);
    if (c) {
        s += "CONCURRENTLY ";
    }
    if (ie) {
        s += "IF EXISTS ";
    }
    appendDbQualifiedIdentifier(s, n, driver_);
    append(std::move(s), c ? DbMigrationAtomicity::kUnwrapped : DbMigrationAtomicity::kTransactional);
}
void DbSchema::createEnum(std::string_view n, std::span<const std::string_view> v) {
    requirePostgreSql();
    if (v.empty()) {
        throw std::invalid_argument("enum values are required");
    }
    std::pmr::string s("CREATE TYPE ", resource_);
    appendDbQualifiedIdentifier(s, n, driver_);
    s += " AS ENUM (";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) {
            s += ',';
        }
        appendDbStringLiteral(s, v[i], driver_);
    }
    s += ')';
    append(std::move(s));
}
void DbSchema::addEnumValue(std::string_view n, std::string_view v, bool ie) {
    requirePostgreSql();
    std::pmr::string s("ALTER TYPE ", resource_);
    appendDbQualifiedIdentifier(s, n, driver_);
    s += " ADD VALUE ";
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    appendDbStringLiteral(s, v, driver_);
    append(std::move(s), DbMigrationAtomicity::kUnwrapped);
}
void DbSchema::dropEnum(std::string_view n, DbDropBehavior b, bool ie) {
    requirePostgreSql();
    std::pmr::string s("DROP TYPE ", resource_);
    if (ie) {
        s += "IF EXISTS ";
    }
    appendDbQualifiedIdentifier(s, n, driver_);
    if (b == DbDropBehavior::kCascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void DbSchema::createExtension(std::string_view n, bool ie) {
    requirePostgreSql();
    std::pmr::string s("CREATE EXTENSION ", resource_);
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    appendDbIdentifier(s, n, driver_);
    append(std::move(s));
}
void DbSchema::createView(std::string_view n, const DbQuery& q, bool replace, bool mat) {
    q.requireSelectQuery();
    if (mat) {
        requirePostgreSql();
    }
    if (replace && mat) {
        throw std::invalid_argument("materialized views cannot use OR REPLACE");
    }
    std::pmr::string s("CREATE ", resource_);
    if (replace) {
        s += "OR REPLACE ";
    }
    if (mat) {
        s += "MATERIALIZED ";
    }
    s += "VIEW ";
    appendDbQualifiedIdentifier(s, n, driver_);
    s += " AS ";
    auto st = q.compile(driver_, resource_, DbParameterMode::kLiteral);
    s += st.sql();
    append(std::move(s));
}
void DbSchema::dropView(std::string_view n, bool mat, DbDropBehavior b, bool ie) {
    if (mat) {
        requirePostgreSql();
    }
    std::pmr::string s("DROP ", resource_);
    if (mat) {
        s += "MATERIALIZED ";
    }
    s += "VIEW ";
    if (ie) {
        s += "IF EXISTS ";
    }
    appendDbQualifiedIdentifier(s, n, driver_);
    if (b == DbDropBehavior::kCascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void DbSchema::execute(const DbQuery& q) {
    if (q.returnsRows()) {
        throw std::invalid_argument("schema execute requires a command without returned rows");
    }
    auto st = q.compile(driver_, resource_, DbParameterMode::kLiteral);
    append(std::pmr::string(st.sql(), resource_));
}
void DbSchema::perform(DbQuery::Expr e) {
    requirePostgreSql();
    std::pmr::string s("PERFORM ", resource_);
    appendExpression(s, e);
    append(std::move(s), DbMigrationAtomicity::kTransactional, true);
}

void DbSchema::run(const DbProcedure& procedure) {
    requirePostgreSql();
    auto body = procedure.body(resource_);
    std::pmr::string sql("DO ", resource_);
    detail::appendDbDollarBody(sql, body);
    append(std::move(sql));
}
void DbSchema::createTriggerFunction(std::string_view name, const DbProcedure& procedure, bool replace) {
    requirePostgreSql();
    auto body = procedure.body(resource_);
    std::pmr::string sql(replace ? "CREATE OR REPLACE FUNCTION " : "CREATE FUNCTION ", resource_);
    appendDbQualifiedIdentifier(sql, name, driver_);
    sql += "() RETURNS trigger LANGUAGE plpgsql AS ";
    detail::appendDbDollarBody(sql, body);
    append(std::move(sql));
}
void DbSchema::dropTriggerFunction(std::string_view name, DbDropBehavior behavior, bool ifExists) {
    requirePostgreSql();
    std::pmr::string sql("DROP FUNCTION ", resource_);
    if (ifExists) {
        sql += "IF EXISTS ";
    }
    appendDbQualifiedIdentifier(sql, name, driver_);
    sql += "()";
    if (behavior == DbDropBehavior::kCascade) {
        sql += " CASCADE";
    }
    append(std::move(sql));
}
void DbSchema::createTrigger(const DbTriggerDefinition& trigger) {
    requirePostgreSql();
    if (trigger.events.empty()) {
        throw std::invalid_argument("trigger requires an event");
    }
    bool update = false;
    bool truncate = false;
    for (std::size_t i = 0; i < trigger.events.size(); ++i) {
        update |= trigger.events[i] == DbTriggerEvent::kUpdate;
        truncate |= trigger.events[i] == DbTriggerEvent::kTruncate;
        for (std::size_t j = 0; j < i; ++j) {
            if (trigger.events[i] == trigger.events[j]) {
                throw std::invalid_argument("duplicate trigger event");
            }
        }
    }
    if (!update && !trigger.updateColumns.empty()) {
        throw std::invalid_argument("trigger UPDATE OF requires an update event");
    }
    if (truncate && trigger.forEachRow) {
        throw std::invalid_argument("TRUNCATE triggers run for each statement");
    }
    if (trigger.timing == DbTriggerTiming::kInsteadOf && (!trigger.forEachRow || !trigger.when.empty() || !trigger.updateColumns.empty())) {
        throw std::invalid_argument("INSTEAD OF requires a row trigger without WHEN or UPDATE OF");
    }
    std::pmr::string sql("CREATE TRIGGER ", resource_);
    appendDbIdentifier(sql, trigger.name, driver_);
    switch (trigger.timing) {
        case DbTriggerTiming::kBefore:
            sql += " BEFORE ";
            break;
        case DbTriggerTiming::kAfter:
            sql += " AFTER ";
            break;
        case DbTriggerTiming::kInsteadOf:
            sql += " INSTEAD OF ";
            break;
    }
    bool first = true;
    for (const auto event : trigger.events) {
        if (!first) {
            sql += " OR ";
        }
        first = false;
        switch (event) {
            case DbTriggerEvent::kInsert:
                sql += "INSERT";
                break;
            case DbTriggerEvent::kUpdate:
                sql += "UPDATE";
                if (!trigger.updateColumns.empty()) {
                    sql += " OF ";
                    for (std::size_t i = 0; i < trigger.updateColumns.size(); ++i) {
                        if (i != 0) {
                            sql += ", ";
                        }
                        appendDbIdentifier(sql, trigger.updateColumns[i], driver_);
                    }
                }
                break;
            case DbTriggerEvent::kDelete:
                sql += "DELETE";
                break;
            case DbTriggerEvent::kTruncate:
                sql += "TRUNCATE";
                break;
        }
    }
    sql += " ON ";
    appendDbQualifiedIdentifier(sql, trigger.table, driver_);
    sql += trigger.forEachRow ? " FOR EACH ROW" : " FOR EACH STATEMENT";
    if (!trigger.when.empty()) {
        sql += " WHEN (";
        appendExpression(sql, trigger.when);
        sql += ')';
    }
    sql += " EXECUTE FUNCTION ";
    appendDbQualifiedIdentifier(sql, trigger.function, driver_);
    sql += '(';
    for (std::size_t i = 0; i < trigger.arguments.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        appendDbStringLiteral(sql, trigger.arguments[i], driver_);
    }
    sql += ')';
    append(std::move(sql));
}
void DbSchema::dropTrigger(std::string_view table, std::string_view name, bool ifExists) {
    requirePostgreSql();
    std::pmr::string sql("DROP TRIGGER ", resource_);
    if (ifExists) {
        sql += "IF EXISTS ";
    }
    appendDbIdentifier(sql, name, driver_);
    sql += " ON ";
    appendDbQualifiedIdentifier(sql, table, driver_);
    append(std::move(sql));
}
std::pmr::vector<DbMigration> DbSchema::compile(std::string_view id) const {
    std::pmr::vector<DbMigration> out(resource_);
    if (id.empty()) {
        throw std::invalid_argument("migration id is required");
    }
    if (driver_ == DbDriver::kPostgreSql) {
        bool unwrapped = false;
        for (auto& s : statements_) {
            if (s.atomicity == DbMigrationAtomicity::kUnwrapped) {
                unwrapped = true;
            }
        }
        if (unwrapped && statements_.size() != 1) {
            throw std::invalid_argument("unwrapped schema operation must be alone");
        }
        if (statements_.size() == 1 && !statements_[0].procedural) {
            out.emplace_back(DbMigrationOptions{std::string(id), std::string(statements_[0].sql), statements_[0].atomicity});
        } else if (!statements_.empty()) {
            std::pmr::string body(" BEGIN ", resource_);
            for (auto& s : statements_) {
                body.append(s.sql);
                body.push_back(';');
            }
            body += " END ";
            std::pmr::string sql("DO ", resource_);
            detail::appendDbDollarBody(sql, body);
            out.emplace_back(DbMigrationOptions{std::string(id), std::string(sql), DbMigrationAtomicity::kTransactional});
        }
    } else {
        for (size_t i = 0; i < statements_.size(); ++i) {
            std::string numbered = std::string(id) + "_" + std::to_string(i + 1);
            out.emplace_back(DbMigrationOptions{std::move(numbered), std::string(statements_[i].sql), statements_[i].atomicity});
        }
    }
    return out;
}

}  // namespace ruvia
