#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db_query.h"

namespace ruvia {

class db_schema;

struct db_variable_definition final {
    std::string name_{};
    db_type_definition type_{};
    db_expression default_value_{};
};

// Structured PL/pgSQL for migration blocks and trigger functions. All inputs
// are consumed synchronously. The procedure owns its generated body in PMR.
class db_procedure final {
public:
    explicit db_procedure(std::pmr::memory_resource* resource = nullptr);
    db_procedure(const db_procedure&) = delete;
    db_procedure& operator=(const db_procedure&) = delete;
    db_procedure(db_procedure&&) noexcept = default;
    db_procedure& operator=(db_procedure&&) = delete;

    void declare_variable(const db_variable_definition& variable);
    void declare_row(std::string_view name, std::string_view table);
    void declare_record(std::string_view name);
    void assign(std::string_view target, db_expression value);
    void select_into(const db_query& query, std::span<const std::string_view> targets, bool strict = false);
    void execute(const db_query& query);
    void perform(db_expression expression);
    void apply(const db_schema& schema);
    void begin_if(db_expression condition);
    void else_if(db_expression condition);
    void otherwise();
    void end_if();
    void begin_block();
    void catch_sql_state(std::string_view state);
    void end_block();
    void return_value(db_expression expression = {});
    void raise_exception(std::string_view message, std::string_view sql_state = "P0001");
    [[nodiscard]] std::pmr::string body(std::pmr::memory_resource* resource = nullptr) const;

private:
    enum class block_type : std::uint8_t { if_value,
        else_value,
        begin,
        exception };
    void register_variable(std::string_view name);
    void append_expression(std::pmr::string& output, db_expression value) const;
    std::pmr::memory_resource* resource_;
    std::pmr::string declarations_;
    std::pmr::string statements_;
    std::pmr::vector<std::pmr::string> variables_;
    std::pmr::vector<block_type> blocks_;
};

}  // namespace ruvia
