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
class procedure_append_guard final {
public:
    explicit procedure_append_guard(std::pmr::string& target, std::size_t extra_reserve = 48U)
        : target_(target),
          initial_size_(target.size()) {
        if (target_.capacity() - initial_size_ < extra_reserve) {
            target_.reserve(initial_size_ + (std::max)(extra_reserve, std::size_t{64}));
        }
    }

    procedure_append_guard(const procedure_append_guard&) = delete;
    procedure_append_guard& operator=(const procedure_append_guard&) = delete;

    void commit() noexcept {
        committed_ = true;
    }

    ~procedure_append_guard() {
        if (!committed_) {
            target_.resize(initial_size_);
        }
    }

private:
    std::pmr::string& target_;
    std::size_t initial_size_;
    bool committed_{false};
};
}  // namespace

db_procedure::db_procedure(std::pmr::memory_resource* resource)
    : resource_(pmr_resource_or_default(resource)),
      declarations_(resource_),
      statements_(resource_),
      variables_(resource_),
      blocks_(resource_) {}
void db_procedure::append_expression(std::pmr::string& output, db_expression value) const {
    detail::append_db_expression(output, value, pg);
}
void db_procedure::register_variable(std::string_view name) {
    if (std::ranges::any_of(variables_, [&](const auto& existing) { return existing == name; })) {
        throw std::invalid_argument("duplicate procedure variable");
    }
    variables_.emplace_back(name);
}
void db_procedure::declare_variable(const db_variable_definition& variable) {
    if (std::ranges::any_of(variables_, [&](const auto& existing) { return existing == variable.name_; })) {
        throw std::invalid_argument("duplicate procedure variable");
    }
    procedure_append_guard guard(declarations_, 32U + variable.name_.size());
    detail::append_db_identifier(declarations_, variable.name_, pg);
    declarations_ += ' ';
    const auto& t = variable.type_;
    detail::append_db_type_name(
        declarations_, t.data_type_, t.custom_name_, t.length_, t.precision_, t.scale_, t.array_, pg);
    if (!variable.default_value_.empty()) {
        declarations_ += " := ";
        append_expression(declarations_, variable.default_value_);
    }
    declarations_ += ";\n";
    variables_.emplace_back(variable.name_);
    guard.commit();
}
void db_procedure::declare_row(std::string_view name, std::string_view table_value) {
    if (std::ranges::any_of(variables_, [&](const auto& existing) { return existing == name; })) {
        throw std::invalid_argument("duplicate procedure variable");
    }
    procedure_append_guard guard(declarations_, 16U + name.size() + table_value.size());
    detail::append_db_identifier(declarations_, name, pg);
    declarations_ += ' ';
    detail::append_db_qualified_identifier(declarations_, table_value, pg);
    declarations_ += "%ROWTYPE;\n";
    variables_.emplace_back(name);
    guard.commit();
}
void db_procedure::declare_record(std::string_view name) {
    detail::validate_db_identifier(name, pg);
    if (std::ranges::any_of(variables_, [&](const auto& existing) { return existing == name; })) {
        throw std::invalid_argument("duplicate procedure variable");
    }
    procedure_append_guard guard(declarations_, name.size() + 11U);
    detail::append_db_identifier(declarations_, name, pg);
    declarations_ += " RECORD;\n";
    variables_.emplace_back(name);
    guard.commit();
}
void db_procedure::assign(std::string_view target, db_expression value) {
    procedure_append_guard guard(statements_, 32U + target.size());
    detail::append_db_qualified_identifier(statements_, target, pg);
    statements_ += " := ";
    append_expression(statements_, value);
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::select_into(const db_query& query, std::span<const std::string_view> targets, bool strict) {
    if (targets.empty() || !query.returns_rows()) {
        throw std::invalid_argument("SELECT INTO requires returned rows and target variables");
    }
    const auto statement = query.compile(pg, resource_, db_parameter_mode::literal);
    procedure_append_guard guard(statements_, statement.sql().size() + 16U + targets.size() * 16U);
    statements_ += statement.sql();
    statements_ += strict ? " INTO STRICT " : " INTO ";
    bool first = true;
    for (const auto target : targets) {
        if (!first) {
            statements_ += ", ";
        }
        first = false;
        detail::append_db_qualified_identifier(statements_, target, pg);
    }
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::execute(const db_query& query) {
    if (query.returns_rows()) {
        throw std::invalid_argument("procedure execute requires a command; consume returned rows with select_into");
    }
    const auto statement = query.compile(pg, resource_, db_parameter_mode::literal);
    procedure_append_guard guard(statements_, statement.sql().size() + 2U);
    statements_ += statement.sql();
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::perform(db_expression expression) {
    procedure_append_guard guard(statements_, 48U);
    statements_ += "PERFORM ";
    append_expression(statements_, expression);
    statements_ += ";\n";
    guard.commit();
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
    procedure_append_guard guard(statements_, 48U);
    statements_ += "IF ";
    append_expression(statements_, condition);
    statements_ += " THEN\n";
    blocks_.push_back(block_type::if_value);
    guard.commit();
}
void db_procedure::else_if(db_expression condition) {
    if (blocks_.empty() || blocks_.back() != block_type::if_value) {
        throw std::invalid_argument("ELSEIF requires an open IF without ELSE");
    }
    procedure_append_guard guard(statements_, 48U);
    statements_ += "ELSIF ";
    append_expression(statements_, condition);
    statements_ += " THEN\n";
    guard.commit();
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
    procedure_append_guard guard(statements_, prefix.size() + 20U);
    statements_.append(prefix);
    detail::append_db_string_literal(statements_, state_value, pg);
    statements_ += " THEN\nNULL;\n";
    blocks_.back() = block_type::exception;
    guard.commit();
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
    procedure_append_guard guard(statements_, 16U + target.size() + statement.sql().size());
    statements_.append("FOR ");
    detail::append_db_identifier(statements_, target, pg);
    statements_.append(" IN ");
    statements_.append(statement.sql());
    statements_.append(" LOOP\n");
    blocks_.push_back(block_type::loop);
    guard.commit();
}
void db_procedure::begin_foreach(std::string_view target, db_expression array, unsigned slice) {
    procedure_append_guard guard(statements_, 48U + target.size());
    statements_ += "FOREACH ";
    detail::append_db_identifier(statements_, target, pg);
    if (slice > 0) {
        statements_ += " SLICE ";
        detail::append_db_number(statements_, static_cast<std::uint64_t>(slice));
    }
    statements_ += " IN ARRAY ";
    append_expression(statements_, array);
    statements_ += " LOOP\n";
    blocks_.push_back(block_type::loop);
    guard.commit();
}
void db_procedure::end_loop() {
    if (blocks_.empty() || blocks_.back() != block_type::loop) {
        throw std::invalid_argument("END LOOP requires an open FOR or FOREACH loop");
    }
    statements_ += "END LOOP;\n";
    blocks_.pop_back();
}
void db_procedure::execute_dynamic(db_expression command, std::span<const db_expression> using_args) {
    procedure_append_guard guard(statements_, 48U + using_args.size() * 24U);
    statements_ += "EXECUTE ";
    append_expression(statements_, command);
    if (!using_args.empty()) {
        statements_ += " USING ";
        for (std::size_t i = 0; i < using_args.size(); ++i) {
            if (i != 0) {
                statements_ += ", ";
            }
            append_expression(statements_, using_args[i]);
        }
    }
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::lock_table(std::string_view table, db_table_lock_mode mode, bool nowait) {
    const auto mode_name = table_lock_mode_name(mode);
    procedure_append_guard guard(
        statements_, 11U + table.size() + 8U + mode_name.size() + 5U + (nowait ? 7U : 0U) + 2U);
    statements_.append("LOCK TABLE ");
    detail::append_db_qualified_identifier(statements_, table, pg);
    statements_.append(" IN ");
    statements_.append(mode_name);
    statements_.append(" MODE");
    if (nowait) {
        statements_.append(" NOWAIT");
    }
    statements_.append(";\n");
    guard.commit();
}
void db_procedure::return_value(db_expression expression) {
    procedure_append_guard guard(statements_, 32U);
    statements_ += "RETURN";
    if (!expression.empty()) {
        statements_ += ' ';
        append_expression(statements_, expression);
    }
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::raise_exception(std::string_view message, std::string_view sql_state) {
    require_sql_state(sql_state);
    procedure_append_guard guard(statements_, 56U + message.size() + sql_state.size());
    statements_ += "RAISE EXCEPTION USING MESSAGE = ";
    detail::append_db_string_literal(statements_, message, pg);
    statements_ += ", ERRCODE = ";
    detail::append_db_string_literal(statements_, sql_state, pg);
    statements_ += ";\n";
    guard.commit();
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
