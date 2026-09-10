#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/DbQuery.h"

namespace ruvia {

class DbSchema;

struct DbVariableDefinition final {
    std::string name{};
    DbTypeDefinition type{};
    DbExpression defaultValue{};
};

// Structured PL/pgSQL for migration blocks and trigger functions. All inputs
// are consumed synchronously. The procedure owns its generated body in PMR.
class DbProcedure final {
public:
    explicit DbProcedure(std::pmr::memory_resource* resource = nullptr);
    DbProcedure(const DbProcedure&) = delete;
    DbProcedure& operator=(const DbProcedure&) = delete;
    DbProcedure(DbProcedure&&) noexcept = default;
    DbProcedure& operator=(DbProcedure&&) = delete;

    void declareVariable(const DbVariableDefinition& variable);
    void declareRow(std::string_view name, std::string_view table);
    void declareRecord(std::string_view name);
    void assign(std::string_view target, DbExpression value);
    void selectInto(const DbQuery& query, std::span<const std::string_view> targets, bool strict = false);
    void execute(const DbQuery& query);
    void perform(DbExpression expression);
    void apply(const DbSchema& schema);
    void beginIf(DbExpression condition);
    void elseIf(DbExpression condition);
    void otherwise();
    void endIf();
    void beginBlock();
    void catchSqlState(std::string_view state);
    void endBlock();
    void returnValue(DbExpression expression = {});
    void raiseException(std::string_view message, std::string_view sqlState = "P0001");
    [[nodiscard]] std::pmr::string body(std::pmr::memory_resource* resource = nullptr) const;

private:
    enum class Block : std::uint8_t { kIf,
        kElse,
        kBegin,
        kException };
    void registerVariable(std::string_view name);
    void appendExpression(std::pmr::string& output, DbExpression value) const;
    std::pmr::memory_resource* resource_;
    std::pmr::string declarations_;
    std::pmr::string statements_;
    std::pmr::vector<std::pmr::string> variables_;
    std::pmr::vector<Block> blocks_;
};

}  // namespace ruvia
