#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/web/db/DbPredicate.h"

namespace ruvia::detail {

// Read-only view of the small expression subset shared by the SQL and Redis
// repositories. All returned strings, values and child expressions borrow the
// query or predicate that owns the input expression.
struct DbExpressionInspection final {
    enum class Kind : std::uint8_t {
        kUnsupported,
        kColumn,
        kValue,
        kBinary,
        kUnary,
        kBetween,
        kList,
    };

    Kind kind{Kind::kUnsupported};
    DbBinaryOperator binary{DbBinaryOperator::kEqual};
    DbUnaryOperator unary{DbUnaryOperator::kNot};
    std::string_view column{};
    std::string_view table{};
    const DbValue* value{nullptr};
    bool negated{false};
};

struct DbExpressionAccess final {
    [[nodiscard]] static DbExpressionInspection inspect(DbExpression expression);
    [[nodiscard]] static DbExpression operand(DbExpression expression, std::size_t index);
    [[nodiscard]] static std::size_t operandCount(DbExpression expression);
};

// DbPredicate owns a DbQuery and its expression tree. This accessor returns
// the expression handle only; it never copies the AST.
struct DbPredicateAccess final {
    [[nodiscard]] static DbExpression root(const DbPredicate& predicate) noexcept {
        return predicate.expression_;
    }
};

}  // namespace ruvia::detail
