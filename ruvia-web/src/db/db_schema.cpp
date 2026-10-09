#include "ruvia/web/db/db_schema.h"

#include <algorithm>

#include "db/db_sql_format.h"

namespace ruvia {
namespace {
using detail::append_db_identifier;
using detail::append_db_qualified_identifier;
using detail::append_db_string_literal;
std::string_view action(db_referential_action a) {
    switch (a) {
        case db_referential_action::restrict:
            return "RESTRICT";
        case db_referential_action::cascade:
            return "CASCADE";
        case db_referential_action::set_null:
            return "SET NULL";
        case db_referential_action::set_default:
            return "SET DEFAULT";
        default:
            return "NO ACTION";
    }
}
std::string_view method(db_index_method m) {
    switch (m) {
        case db_index_method::hash:
            return "hash";
        case db_index_method::gin:
            return "gin";
        case db_index_method::gist:
            return "gist";
        case db_index_method::sp_gist:
            return "spgist";
        case db_index_method::brin:
            return "brin";
        default:
            return "btree";
    }
}
}  // namespace

db_schema::db_schema(db_schema_options options)
    : driver_(options.driver_),
      resource_(detail::pmr_resource_or_default(options.resource_)),
      statements_(resource_) {
    detail::require_db_dialect(driver_);
}
void db_schema::append(std::pmr::string sql, db_migration_atomicity a, bool procedural) {
    statements_.push_back(statement_type{std::move(sql), a, procedural});
}
void db_schema::require_postgresql() const {
    if (driver_ != db_driver::postgresql) {
        throw std::invalid_argument("schema operation requires PostgreSQL");
    }
}
std::pmr::string db_schema::table_prefix(std::string_view table_value) const {
    std::pmr::string s(resource_);
    append_db_qualified_identifier(s, table_value, driver_);
    return s;
}
void db_schema::append_expression(std::pmr::string& s, db_query::expr_type e) const {
    if (e.empty()) {
        throw std::invalid_argument("schema expression is required");
    }
    auto x = db_query::render_expression(e, driver_, resource_, db_parameter_mode::literal);
    s += x;
}
void db_schema::append_type(std::pmr::string& s, const db_type_definition& t) const {
    detail::append_db_type_name(s, t.data_type_, t.custom_name_, t.length_, t.precision_, t.scale_,
        t.array_, driver_);
}
void db_schema::append_column(std::pmr::string& s, const db_schema_column& c) const {
    if (c.generated_type_ != db_generated_type::none && c.generated_type_ != db_generated_type::stored &&
        c.generated_type_ != db_generated_type::virtual_value) {
        throw std::invalid_argument("unsupported generated column storage mode");
    }
    if (c.name_.empty()) {
        throw std::invalid_argument("column name is required");
    }
    if (c.generated_type_ == db_generated_type::none && !c.as_expression_.empty()) {
        throw std::invalid_argument("generated column expression requires generated metadata");
    }
    if (c.generated_type_ != db_generated_type::none && (c.identity_ != db_identity::none || !c.default_value_.empty())) {
        throw std::invalid_argument("generated column conflicts with identity or default");
    }
    append_db_identifier(s, c.name_, driver_);
    s += ' ';
    append_type(s, c.type_);
    if (c.generated_type_ != db_generated_type::none) {
        if (c.as_expression_.empty()) {
            throw std::invalid_argument("generated column expression is required");
        }
        if (driver_ == db_driver::postgresql && c.generated_type_ == db_generated_type::virtual_value) {
            throw std::invalid_argument("PostgreSQL schema compilation supports stored generated columns only");
        }
        if (driver_ == db_driver::mariadb && (!c.nullable_ || c.primary_key_)) {
            throw std::invalid_argument("MariaDB generated columns cannot declare NOT NULL or PRIMARY KEY");
        }
        s += " GENERATED ALWAYS AS (";
        append_expression(s, c.as_expression_);
        s += ")";
        if (driver_ == db_driver::postgresql || c.generated_type_ == db_generated_type::stored) {
            s += " STORED";
        } else {
            s += " VIRTUAL";
        }
    }
    if (c.identity_ != db_identity::none) {
        if (!c.default_value_.empty() || c.nullable_ || c.type_.array_ ||
            (c.type_.data_type_ != db_data_type::small_int && c.type_.data_type_ != db_data_type::integer && c.type_.data_type_ != db_data_type::big_int)) {
            throw std::invalid_argument("identity requires a non-null integer column without a default");
        }
        if (driver_ == db_driver::postgresql) {
            s += " GENERATED ";
            s += (c.identity_ == db_identity::always ? "ALWAYS" : "BY DEFAULT");
            s += " AS IDENTITY";
        } else {
            if (c.identity_ == db_identity::always) {
                throw std::invalid_argument("MariaDB cannot enforce GENERATED ALWAYS identity semantics");
            }
            s += " AUTO_INCREMENT";
        }
    }
    if (!c.nullable_) {
        s += " NOT NULL";
    }
    if (c.unique_) {
        s += " UNIQUE";
    }
    if (c.primary_key_) {
        s += " PRIMARY KEY";
    }
    if (!c.default_value_.empty()) {
        s += " DEFAULT ";
        append_expression(s, c.default_value_);
    }
}
void db_schema::append_constraint(std::pmr::string& s, const db_schema_constraint& c) const {
    if ((c.deferrable_ || c.initially_deferred_ || c.not_valid_) && driver_ != db_driver::postgresql) {
        throw std::invalid_argument("deferred and NOT VALID constraints require PostgreSQL");
    }
    if (c.initially_deferred_ && !c.deferrable_) {
        throw std::invalid_argument("INITIALLY DEFERRED requires DEFERRABLE");
    }
    if (c.kind_ == db_constraint_kind::check && c.deferrable_) {
        throw std::invalid_argument("CHECK constraints cannot be deferred");
    }
    if (c.not_valid_ && c.kind_ != db_constraint_kind::check && c.kind_ != db_constraint_kind::foreign_key) {
        throw std::invalid_argument("NOT VALID requires a CHECK or foreign key constraint");
    }
    if (c.kind_ != db_constraint_kind::check && c.columns_.empty()) {
        throw std::invalid_argument("constraint requires columns");
    }
    if (c.kind_ == db_constraint_kind::foreign_key && c.columns_.size() != c.referenced_columns_.size()) {
        throw std::invalid_argument("foreign key column counts differ");
    }
    if (c.name_.empty()) {
        throw std::invalid_argument("constraint name is required");
    }
    append_db_identifier(s, c.name_, driver_);
    s += ' ';
    if (c.kind_ == db_constraint_kind::check) {
        s += "CHECK (";
        append_expression(s, c.check_);
        s += ')';
    } else if (c.kind_ == db_constraint_kind::primary_key || c.kind_ == db_constraint_kind::unique) {
        s += (c.kind_ == db_constraint_kind::primary_key ? "PRIMARY KEY (" : "UNIQUE (");
        for (size_t i = 0; i < c.columns_.size(); ++i) {
            if (i) {
                s += ',';
            }
            append_db_identifier(s, c.columns_[i], driver_);
        }
        s += ')';
    } else {
        if (c.columns_.empty() || c.referenced_table_.empty() || c.referenced_columns_.empty()) {
            throw std::invalid_argument("foreign key fields are required");
        }
        s += "FOREIGN KEY (";
        for (size_t i = 0; i < c.columns_.size(); ++i) {
            if (i) {
                s += ',';
            }
            append_db_identifier(s, c.columns_[i], driver_);
        }
        s += ") REFERENCES ";
        append_db_qualified_identifier(s, c.referenced_table_, driver_);
        s += " (";
        for (size_t i = 0; i < c.referenced_columns_.size(); ++i) {
            if (i) {
                s += ',';
            }
            append_db_identifier(s, c.referenced_columns_[i], driver_);
        }
        s += ") ON DELETE ";
        s += action(c.on_delete_);
        s += " ON UPDATE ";
        s += action(c.on_update_);
    }
    if (c.deferrable_) {
        s += " DEFERRABLE";
    }
    if (c.initially_deferred_) {
        s += " INITIALLY DEFERRED";
    }
    if (c.not_valid_) {
        s += " NOT VALID";
    }
}

void db_schema::create_schema(std::string_view n, bool ine) {
    std::pmr::string s("CREATE SCHEMA ", resource_);
    if (ine) {
        s += "IF NOT EXISTS ";
    }
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::drop_schema(std::string_view n, db_drop_behavior b, bool ie) {
    if (driver_ == db_driver::mariadb && b == db_drop_behavior::cascade) {
        throw std::invalid_argument("MariaDB schema drops do not support CASCADE");
    }
    std::pmr::string s("DROP SCHEMA ", resource_);
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_identifier(s, n, driver_);
    if (b == db_drop_behavior::cascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void db_schema::create_table(const db_table_definition& t) {
    if (t.name_.empty() || t.columns_.empty()) {
        throw std::invalid_argument("table and columns are required");
    }
    std::pmr::string s("CREATE ", resource_);
    if (t.temporary_ && t.unlogged_) {
        throw std::invalid_argument("table cannot be both temporary and unlogged");
    }
    if (t.unlogged_) {
        require_postgresql();
    }
    std::size_t primary_columns = 0;
    for (std::size_t i = 0; i < t.columns_.size(); ++i) {
        primary_columns += t.columns_[i].primary_key_ ? 1 : 0;
        for (std::size_t j = 0; j < i; ++j) {
            if (t.columns_[i].name_ == t.columns_[j].name_) {
                throw std::invalid_argument("duplicate table column");
            }
        }
    }
    if (primary_columns > 1) {
        throw std::invalid_argument("composite primary keys require a table constraint");
    }
    for (const auto& constraint : t.constraints_) {
        if (constraint.not_valid_) {
            throw std::invalid_argument("NOT VALID is only supported when adding a constraint");
        }
        if (constraint.kind_ == db_constraint_kind::primary_key && primary_columns++ != 0) {
            throw std::invalid_argument("duplicate table primary key");
        }
        if (driver_ == db_driver::mariadb && constraint.kind_ == db_constraint_kind::primary_key) {
            for (const auto& column : t.columns_) {
                if (column.generated_type_ != db_generated_type::none &&
                    std::ranges::find(constraint.columns_, column.name_) != constraint.columns_.end()) {
                    throw std::invalid_argument("MariaDB generated columns cannot be primary keys");
                }
            }
        }
    }
    if (t.temporary_) {
        s += "TEMPORARY ";
    }
    if (t.unlogged_) {
        s += "UNLOGGED ";
    }
    s += "TABLE ";
    if (t.if_not_exists_) {
        s += "IF NOT EXISTS ";
    }
    append_db_qualified_identifier(s, t.name_, driver_);
    s += " (";
    for (size_t i = 0; i < t.columns_.size(); ++i) {
        if (i) {
            s += ", ";
        }
        append_column(s, t.columns_[i]);
    }
    for (auto& c : t.constraints_) {
        s += ", CONSTRAINT ";
        append_constraint(s, c);
    }
    s += ')';
    append(std::move(s));
}
void db_schema::drop_table(std::string_view t, db_drop_behavior b, bool ie) {
    std::pmr::string s("DROP TABLE ", resource_);
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_qualified_identifier(s, t, driver_);
    if (b == db_drop_behavior::cascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void db_schema::rename_table(std::string_view t, std::string_view n) {
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " RENAME TO ";
    if (driver_ == db_driver::mariadb) {
        if (const auto dot = t.rfind('.'); dot != std::string_view::npos) {
            append_db_qualified_identifier(s, t.substr(0, dot), driver_);
            s += '.';
        }
    }
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::add_column(std::string_view t, const db_schema_column& c, bool ie) {
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " ADD COLUMN ";
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    append_column(s, c);
    append(std::move(s));
}
void db_schema::drop_column(std::string_view t, std::string_view c, db_drop_behavior b, bool ie) {
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " DROP COLUMN ";
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_identifier(s, c, driver_);
    if (b == db_drop_behavior::cascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void db_schema::rename_column(std::string_view t, std::string_view c, std::string_view n) {
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " RENAME COLUMN ";
    append_db_identifier(s, c, driver_);
    s += " TO ";
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::alter_column_type(std::string_view t, std::string_view c, const db_type_definition& ty, db_query::expr_type u) {
    require_postgresql();
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " ALTER COLUMN ";
    append_db_identifier(s, c, driver_);
    s += " TYPE ";
    append_type(s, ty);
    if (!u.empty()) {
        s += " USING ";
        append_expression(s, u);
    }
    append(std::move(s));
}
void db_schema::set_column_nullable(std::string_view t, std::string_view c, bool n) {
    require_postgresql();
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " ALTER COLUMN ";
    append_db_identifier(s, c, driver_);
    s += n ? " DROP NOT NULL" : " SET NOT NULL";
    append(std::move(s));
}
void db_schema::set_column_default(std::string_view t, std::string_view c, db_query::expr_type e) {
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " ALTER COLUMN ";
    append_db_identifier(s, c, driver_);
    s += " SET DEFAULT ";
    append_expression(s, e);
    append(std::move(s));
}
void db_schema::drop_column_default(std::string_view t, std::string_view c) {
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " ALTER COLUMN ";
    append_db_identifier(s, c, driver_);
    s += " DROP DEFAULT";
    append(std::move(s));
}
void db_schema::add_constraint(std::string_view t, const db_schema_constraint& c) {
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " ADD CONSTRAINT ";
    append_constraint(s, c);
    append(std::move(s));
}
void db_schema::drop_constraint(std::string_view t, std::string_view n, db_drop_behavior b, bool ie) {
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " DROP CONSTRAINT ";
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_identifier(s, n, driver_);
    if (b == db_drop_behavior::cascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void db_schema::validate_constraint(std::string_view t, std::string_view n) {
    require_postgresql();
    std::pmr::string s("ALTER TABLE ", resource_);
    append_db_qualified_identifier(s, t, driver_);
    s += " VALIDATE CONSTRAINT ";
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::create_index(const db_index_definition& i) {
    if (driver_ != db_driver::postgresql && (i.concurrently_ || i.method_ != db_index_method::btree || !i.include_.empty() || !i.where_.empty())) {
        throw std::invalid_argument("selected index options require PostgreSQL");
    }
    if (i.name_.empty() || i.table_.empty() || i.keys_.empty()) {
        throw std::invalid_argument("index name, table and keys are required");
    }
    std::pmr::string s("CREATE ", resource_);
    if (i.unique_) {
        s += "UNIQUE ";
    }
    s += "INDEX ";
    if (i.concurrently_) {
        s += "CONCURRENTLY ";
    }
    if (i.if_not_exists_) {
        s += "IF NOT EXISTS ";
    }
    append_db_identifier(s, i.name_, driver_);
    if (driver_ == db_driver::mariadb) {
        s += " USING ";
        s += method(i.method_);
    }
    s += " ON ";
    append_db_qualified_identifier(s, i.table_, driver_);
    if (driver_ == db_driver::postgresql) {
        s += " USING ";
        s += method(i.method_);
    }
    s += " (";
    for (size_t n = 0; n < i.keys_.size(); ++n) {
        if (n) {
            s += ", ";
        }
        const auto& key = i.keys_[n];
        if (key.column_.empty() == key.expression_.empty()) {
            throw std::invalid_argument("an index key requires exactly one column or expression");
        }
        if (driver_ != db_driver::postgresql && (!key.expression_.empty() || !key.operator_class_.empty() || key.nulls_ != db_nulls_order::default_value)) {
            throw std::invalid_argument("selected index key options require PostgreSQL");
        }
        if (!i.keys_[n].column_.empty()) {
            append_db_identifier(s, i.keys_[n].column_, driver_);
        } else {
            s += '(';
            append_expression(s, i.keys_[n].expression_);
            s += ')';
        }
        if (!i.keys_[n].operator_class_.empty()) {
            s += ' ';
            append_db_qualified_identifier(s, i.keys_[n].operator_class_, driver_);
        }
        if (i.keys_[n].order_ == db_order_direction::desc) {
            s += " DESC";
        }
        if (i.keys_[n].nulls_ == db_nulls_order::first) {
            s += " NULLS FIRST";
        }
        if (i.keys_[n].nulls_ == db_nulls_order::last) {
            s += " NULLS LAST";
        }
    }
    s += ')';
    if (!i.include_.empty()) {
        s += " INCLUDE (";
        for (size_t n = 0; n < i.include_.size(); ++n) {
            if (n) {
                s += ',';
            }
            append_db_identifier(s, i.include_[n], driver_);
        }
        s += ')';
    }
    if (!i.where_.empty()) {
        s += " WHERE ";
        append_expression(s, i.where_);
    }
    append(std::move(s), i.concurrently_ ? db_migration_atomicity::unwrapped : db_migration_atomicity::transactional);
}
void db_schema::drop_index(std::string_view n, bool c, bool ie) {
    require_postgresql();
    std::pmr::string s("DROP INDEX ", resource_);
    if (c) {
        s += "CONCURRENTLY ";
    }
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_qualified_identifier(s, n, driver_);
    append(std::move(s), c ? db_migration_atomicity::unwrapped : db_migration_atomicity::transactional);
}
void db_schema::create_enum(std::string_view n, std::span<const std::string_view> v) {
    require_postgresql();
    if (v.empty()) {
        throw std::invalid_argument("enum values are required");
    }
    std::pmr::string s("CREATE TYPE ", resource_);
    append_db_qualified_identifier(s, n, driver_);
    s += " AS ENUM (";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) {
            s += ',';
        }
        append_db_string_literal(s, v[i], driver_);
    }
    s += ')';
    append(std::move(s));
}
void db_schema::add_enum_value(std::string_view n, std::string_view v, bool ie) {
    require_postgresql();
    std::pmr::string s("ALTER TYPE ", resource_);
    append_db_qualified_identifier(s, n, driver_);
    s += " ADD VALUE ";
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    append_db_string_literal(s, v, driver_);
    append(std::move(s), db_migration_atomicity::unwrapped);
}
void db_schema::drop_enum(std::string_view n, db_drop_behavior b, bool ie) {
    require_postgresql();
    std::pmr::string s("DROP TYPE ", resource_);
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_qualified_identifier(s, n, driver_);
    if (b == db_drop_behavior::cascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void db_schema::create_extension(std::string_view n, bool ie) {
    require_postgresql();
    std::pmr::string s("CREATE EXTENSION ", resource_);
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::create_view(std::string_view n, const db_query& q, bool replace, bool mat) {
    q.require_select_query();
    if (mat) {
        require_postgresql();
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
    append_db_qualified_identifier(s, n, driver_);
    s += " AS ";
    auto st = q.compile(driver_, resource_, db_parameter_mode::literal);
    s += st.sql();
    append(std::move(s));
}
void db_schema::drop_view(std::string_view n, bool mat, db_drop_behavior b, bool ie) {
    if (mat) {
        require_postgresql();
    }
    std::pmr::string s("DROP ", resource_);
    if (mat) {
        s += "MATERIALIZED ";
    }
    s += "VIEW ";
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_qualified_identifier(s, n, driver_);
    if (b == db_drop_behavior::cascade) {
        s += " CASCADE";
    }
    append(std::move(s));
}
void db_schema::execute(const db_query& q) {
    if (q.returns_rows()) {
        throw std::invalid_argument("schema execute requires a command without returned rows");
    }
    auto st = q.compile(driver_, resource_, db_parameter_mode::literal);
    append(std::pmr::string(st.sql(), resource_));
}
void db_schema::perform(db_query::expr_type e) {
    require_postgresql();
    std::pmr::string s("PERFORM ", resource_);
    append_expression(s, e);
    append(std::move(s), db_migration_atomicity::transactional, true);
}

void db_schema::run(const db_procedure& procedure) {
    require_postgresql();
    auto body = procedure.body(resource_);
    std::pmr::string sql("DO ", resource_);
    detail::append_db_dollar_body(sql, body);
    append(std::move(sql));
}
void db_schema::create_trigger_function(std::string_view name, const db_procedure& procedure, bool replace) {
    require_postgresql();
    auto body = procedure.body(resource_);
    std::pmr::string sql(replace ? "CREATE OR REPLACE FUNCTION " : "CREATE FUNCTION ", resource_);
    append_db_qualified_identifier(sql, name, driver_);
    sql += "() RETURNS trigger LANGUAGE plpgsql AS ";
    detail::append_db_dollar_body(sql, body);
    append(std::move(sql));
}
void db_schema::drop_trigger_function(std::string_view name, db_drop_behavior behavior, bool if_exists) {
    require_postgresql();
    std::pmr::string sql("DROP FUNCTION ", resource_);
    if (if_exists) {
        sql += "IF EXISTS ";
    }
    append_db_qualified_identifier(sql, name, driver_);
    sql += "()";
    if (behavior == db_drop_behavior::cascade) {
        sql += " CASCADE";
    }
    append(std::move(sql));
}
void db_schema::create_trigger(const db_trigger_definition& trigger) {
    require_postgresql();
    if (trigger.events_.empty()) {
        throw std::invalid_argument("trigger requires an event");
    }
    bool update = false;
    bool truncate = false;
    for (std::size_t i = 0; i < trigger.events_.size(); ++i) {
        update |= trigger.events_[i] == db_trigger_event::update;
        truncate |= trigger.events_[i] == db_trigger_event::truncate;
        for (std::size_t j = 0; j < i; ++j) {
            if (trigger.events_[i] == trigger.events_[j]) {
                throw std::invalid_argument("duplicate trigger event");
            }
        }
    }
    if (!update && !trigger.update_columns_.empty()) {
        throw std::invalid_argument("trigger UPDATE OF requires an update event");
    }
    if (truncate && trigger.for_each_row_) {
        throw std::invalid_argument("TRUNCATE triggers run for each statement");
    }
    if (trigger.timing_ == db_trigger_timing::instead_of && (!trigger.for_each_row_ || !trigger.when_.empty() || !trigger.update_columns_.empty())) {
        throw std::invalid_argument("INSTEAD OF requires a row trigger without WHEN or UPDATE OF");
    }
    std::pmr::string sql("CREATE TRIGGER ", resource_);
    append_db_identifier(sql, trigger.name_, driver_);
    switch (trigger.timing_) {
        case db_trigger_timing::before:
            sql += " BEFORE ";
            break;
        case db_trigger_timing::after:
            sql += " AFTER ";
            break;
        case db_trigger_timing::instead_of:
            sql += " INSTEAD OF ";
            break;
    }
    bool first = true;
    for (const auto event : trigger.events_) {
        if (!first) {
            sql += " OR ";
        }
        first = false;
        switch (event) {
            case db_trigger_event::insert:
                sql += "INSERT";
                break;
            case db_trigger_event::update:
                sql += "UPDATE";
                if (!trigger.update_columns_.empty()) {
                    sql += " OF ";
                    for (std::size_t i = 0; i < trigger.update_columns_.size(); ++i) {
                        if (i != 0) {
                            sql += ", ";
                        }
                        append_db_identifier(sql, trigger.update_columns_[i], driver_);
                    }
                }
                break;
            case db_trigger_event::delete_value:
                sql += "DELETE";
                break;
            case db_trigger_event::truncate:
                sql += "TRUNCATE";
                break;
        }
    }
    sql += " ON ";
    append_db_qualified_identifier(sql, trigger.table_, driver_);
    sql += trigger.for_each_row_ ? " FOR EACH ROW" : " FOR EACH STATEMENT";
    if (!trigger.when_.empty()) {
        sql += " WHEN (";
        append_expression(sql, trigger.when_);
        sql += ')';
    }
    sql += " EXECUTE FUNCTION ";
    append_db_qualified_identifier(sql, trigger.function_, driver_);
    sql += '(';
    for (std::size_t i = 0; i < trigger.arguments_.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        append_db_string_literal(sql, trigger.arguments_[i], driver_);
    }
    sql += ')';
    append(std::move(sql));
}
void db_schema::drop_trigger(std::string_view table_value, std::string_view name, bool if_exists) {
    require_postgresql();
    std::pmr::string sql("DROP TRIGGER ", resource_);
    if (if_exists) {
        sql += "IF EXISTS ";
    }
    append_db_identifier(sql, name, driver_);
    sql += " ON ";
    append_db_qualified_identifier(sql, table_value, driver_);
    append(std::move(sql));
}
std::pmr::vector<db_migration> db_schema::compile(std::string_view id) const {
    std::pmr::vector<db_migration> out(resource_);
    if (id.empty()) {
        throw std::invalid_argument("migration id is required");
    }
    if (driver_ == db_driver::postgresql) {
        bool unwrapped = false;
        for (auto& s : statements_) {
            if (s.atomicity_ == db_migration_atomicity::unwrapped) {
                unwrapped = true;
            }
        }
        if (unwrapped && statements_.size() != 1) {
            throw std::invalid_argument("unwrapped schema operation must be alone");
        }
        if (statements_.size() == 1 && !statements_[0].procedural_) {
            out.emplace_back(db_migration_options{std::string(id), std::string(statements_[0].sql_), statements_[0].atomicity_});
        } else if (!statements_.empty()) {
            std::pmr::string body(" BEGIN ", resource_);
            for (auto& s : statements_) {
                body.append(s.sql_);
                body.push_back(';');
            }
            body += " END ";
            std::pmr::string sql("DO ", resource_);
            detail::append_db_dollar_body(sql, body);
            out.emplace_back(db_migration_options{std::string(id), std::string(sql), db_migration_atomicity::transactional});
        }
    } else {
        for (size_t i = 0; i < statements_.size(); ++i) {
            std::string numbered = std::string(id) + "_" + std::to_string(i + 1);
            out.emplace_back(db_migration_options{std::move(numbered), std::string(statements_[i].sql_), statements_[i].atomicity_});
        }
    }
    return out;
}

}  // namespace ruvia
