#pragma once
#include "ruvia/web/db/DbQuery.h"
namespace ruvia {
// Owns expression nodes; returned views remain valid until this owner is destroyed.
// Repository and builder entry points copy those nodes before returning.
class DbExpressions final {
public:
    using Expr = DbExpression;
    explicit DbExpressions(std::pmr::memory_resource* resource = nullptr)
        : query_(resource) {}
    // Trusted SQL syntax interleaved with expression arguments. Values belong in
    // value() arguments, never in the syntax strings. parts.size() == args.size()+1.
    Expr sql(std::span<const std::string_view> parts, std::span<const Expr> args) {
        return query_.sql(parts, args);
    }
    Expr sql(std::initializer_list<std::string_view> parts, std::initializer_list<Expr> args) {
        return sql(std::span<const std::string_view>(parts.begin(), parts.size()), std::span<const Expr>(args.begin(), args.size()));
    }
    Expr sql(std::string_view expression) {
        return sql(std::span<const std::string_view>(&expression, 1), {});
    }
    Expr column(std::string_view name, std::string_view table = {}) {
        return query_.column(name, table);
    }
    Expr star(std::string_view table = {}) {
        return query_.star(table);
    }
    Expr value(DbValue value) {
        return query_.value(value);
    }
    template <detail::DbParameter Value>
        requires(!std::same_as<std::remove_cvref_t<Value>, DbValue>)
    Expr value(Value&& value) {
        return this->value(detail::makeImmediateDbParameter(std::forward<Value>(value)));
    }
    Expr excluded(std::string_view column) {
        return query_.excluded(column);
    }
    Expr nullValue() {
        return query_.nullValue();
    }
    Expr defaultValue() {
        return query_.defaultValue();
    }
    Expr call(std::string_view function, std::span<const Expr> args = {}, std::span<const DbNamedArgument> named = {}) {
        return query_.call(function, args, named);
    }
    Expr call(std::string_view function, std::initializer_list<Expr> args) {
        return call(function, std::span<const Expr>(args.begin(), args.size()));
    }
    Expr coalesce(std::span<const Expr> args) {
        return query_.coalesce(args);
    }
    Expr coalesce(std::initializer_list<Expr> args) {
        return coalesce(std::span<const Expr>(args.begin(), args.size()));
    }
    Expr nullIf(Expr value, Expr other) {
        return query_.nullIf(value, other);
    }
    Expr greatest(std::span<const Expr> args) {
        return query_.greatest(args);
    }
    Expr greatest(std::initializer_list<Expr> args) {
        return greatest(std::span<const Expr>(args.begin(), args.size()));
    }
    Expr least(std::span<const Expr> args) {
        return query_.least(args);
    }
    Expr least(std::initializer_list<Expr> args) {
        return least(std::span<const Expr>(args.begin(), args.size()));
    }
    Expr binary(Expr lhs, DbBinaryOperator op, Expr rhs) {
        return query_.binary(lhs, op, rhs);
    }
    Expr unary(DbUnaryOperator op, Expr value) {
        return query_.unary(op, value);
    }
    Expr between(Expr value, Expr lower, Expr upper, bool negate = false) {
        return query_.between(value, lower, upper, negate);
    }
    Expr tuple(std::span<const Expr> values) {
        return query_.tuple(values);
    }
    Expr tuple(std::initializer_list<Expr> values) {
        return tuple(std::span<const Expr>(values.begin(), values.size()));
    }
    Expr list(std::span<const Expr> values) {
        return query_.list(values);
    }
    Expr list(std::initializer_list<Expr> values) {
        return list(std::span<const Expr>(values.begin(), values.size()));
    }
    Expr array(std::span<const Expr> values) {
        return query_.array(values);
    }
    Expr array(std::initializer_list<Expr> values) {
        return array(std::span<const Expr>(values.begin(), values.size()));
    }
    Expr any(Expr array) {
        return query_.any(array);
    }
    Expr all(Expr array) {
        return query_.all(array);
    }
    Expr cast(Expr value, const DbTypeDefinition& type) {
        return query_.cast(value, type);
    }
    Expr cast(Expr value, DbDataType type) {
        return cast(value, DbTypeDefinition{.dataType = type});
    }
    Expr caseWhen(std::span<const DbCaseBranch> branches, Expr otherwise = {}) {
        return query_.caseWhen(branches, otherwise);
    }
    Expr caseWhen(std::initializer_list<DbCaseBranch> branches, Expr otherwise = {}) {
        return caseWhen(std::span<const DbCaseBranch>(branches.begin(), branches.size()), otherwise);
    }
    Expr aggregate(std::string_view function, std::span<const Expr> args, bool distinct = false, std::span<const DbOrderTerm> order = {}) {
        return query_.aggregate(function, args, distinct, order);
    }
    Expr aggregate(std::string_view function, std::initializer_list<Expr> args, bool distinct = false, std::span<const DbOrderTerm> order = {}) {
        return aggregate(function, std::span<const Expr>(args.begin(), args.size()), distinct, order);
    }
    Expr filter(Expr aggregate, Expr predicate) {
        return query_.filter(aggregate, predicate);
    }
    Expr over(Expr function, const DbWindowOptions& window) {
        return query_.over(function, window);
    }
    Expr withinGroup(Expr function, std::span<const DbOrderTerm> order) {
        return query_.withinGroup(function, order);
    }
    Expr extract(DbDatePart part, Expr value) {
        return query_.extract(part, value);
    }
    Expr subscript(Expr array, Expr index) {
        return query_.subscript(array, index);
    }
    Expr collate(Expr value, std::string_view collation) {
        return query_.collate(value, collation);
    }
    Expr importExpression(Expr expression, std::string_view sourceQualifier = {}, std::string_view targetQualifier = {}) {
        return query_.importExpression(expression, sourceQualifier, targetQualifier);
    }

private:
    template <typename, typename>
    friend class DbQueryBuilder;
    DbQuery query_;
};
}  // namespace ruvia
