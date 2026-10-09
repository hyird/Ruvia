#pragma once
#include "ruvia/web/db/db_query.h"
namespace ruvia {
// Owns expression nodes; returned views remain valid until this owner is destroyed.
// Repository and builder entry points copy those nodes before returning.
class db_expressions final {
public:
    using expr_type = db_expression;
    explicit db_expressions(std::pmr::memory_resource* resource = nullptr)
        : query_(resource) {}
    // Trusted SQL syntax interleaved with expression arguments. Values belong in
    // value() arguments, never in the syntax strings. parts.size() == args.size()+1.
    expr_type sql(std::span<const std::string_view> parts, std::span<const expr_type> args) {
        return query_.sql(parts, args);
    }
    expr_type sql(std::initializer_list<std::string_view> parts, std::initializer_list<expr_type> args) {
        return sql(std::span<const std::string_view>(parts.begin(), parts.size()), std::span<const expr_type>(args.begin(), args.size()));
    }
    expr_type sql(std::string_view expression) {
        return sql(std::span<const std::string_view>(&expression, 1), {});
    }
    expr_type column(std::string_view name, std::string_view table = {}) {
        return query_.column(name, table);
    }
    expr_type star(std::string_view table = {}) {
        return query_.star(table);
    }
    expr_type value(const db_value& value) {
        return query_.value(value);
    }
    template <detail::db_parameter value_type>
        requires(!std::same_as<std::remove_cvref_t<value_type>, db_value>)
    expr_type value(value_type&& value) {
        return this->value(detail::make_immediate_db_parameter(std::forward<value_type>(value)));
    }
    expr_type excluded(std::string_view column) {
        return query_.excluded(column);
    }
    expr_type null_value() {
        return query_.null_value();
    }
    expr_type default_value() {
        return query_.default_value();
    }
    expr_type call(std::string_view function, std::span<const expr_type> args = {}, std::span<const db_named_argument> named = {}) {
        return query_.call(function, args, named);
    }
    expr_type call(std::string_view function, std::initializer_list<expr_type> args) {
        return call(function, std::span<const expr_type>(args.begin(), args.size()));
    }
    expr_type coalesce(std::span<const expr_type> args) {
        return query_.coalesce(args);
    }
    expr_type coalesce(std::initializer_list<expr_type> args) {
        return coalesce(std::span<const expr_type>(args.begin(), args.size()));
    }
    expr_type null_if(expr_type value, expr_type other) {
        return query_.null_if(value, other);
    }
    expr_type greatest(std::span<const expr_type> args) {
        return query_.greatest(args);
    }
    expr_type greatest(std::initializer_list<expr_type> args) {
        return greatest(std::span<const expr_type>(args.begin(), args.size()));
    }
    expr_type least(std::span<const expr_type> args) {
        return query_.least(args);
    }
    expr_type least(std::initializer_list<expr_type> args) {
        return least(std::span<const expr_type>(args.begin(), args.size()));
    }
    expr_type binary(expr_type lhs, db_binary_operator op, expr_type rhs) {
        return query_.binary(lhs, op, rhs);
    }
    expr_type unary(db_unary_operator op, expr_type value) {
        return query_.unary(op, value);
    }
    expr_type between(expr_type value, expr_type lower, expr_type upper, bool negate = false) {
        return query_.between(value, lower, upper, negate);
    }
    expr_type tuple(std::span<const expr_type> values) {
        return query_.tuple(values);
    }
    expr_type tuple(std::initializer_list<expr_type> values) {
        return tuple(std::span<const expr_type>(values.begin(), values.size()));
    }
    expr_type list(std::span<const expr_type> values) {
        return query_.list(values);
    }
    expr_type list(std::initializer_list<expr_type> values) {
        return list(std::span<const expr_type>(values.begin(), values.size()));
    }
    expr_type array(std::span<const expr_type> values) {
        return query_.array(values);
    }
    expr_type array(std::initializer_list<expr_type> values) {
        return array(std::span<const expr_type>(values.begin(), values.size()));
    }
    expr_type any(expr_type array_value) {
        return query_.any(array_value);
    }
    expr_type all(expr_type array_value) {
        return query_.all(array_value);
    }
    expr_type cast(expr_type value, const db_type_definition& type) {
        return query_.cast(value, type);
    }
    expr_type cast(expr_type value, db_data_type type) {
        return cast(value, db_type_definition{.data_type_ = type});
    }
    expr_type case_when(std::span<const db_case_branch> branches, expr_type otherwise = {}) {
        return query_.case_when(branches, otherwise);
    }
    expr_type case_when(std::initializer_list<db_case_branch> branches, expr_type otherwise = {}) {
        return case_when(std::span<const db_case_branch>(branches.begin(), branches.size()), otherwise);
    }
    expr_type aggregate(std::string_view function, std::span<const expr_type> args, bool distinct = false, std::span<const db_order_term> order = {}) {
        return query_.aggregate(function, args, distinct, order);
    }
    expr_type aggregate(std::string_view function, std::initializer_list<expr_type> args, bool distinct = false, std::span<const db_order_term> order = {}) {
        return aggregate(function, std::span<const expr_type>(args.begin(), args.size()), distinct, order);
    }
    expr_type filter(expr_type aggregate, expr_type predicate) {
        return query_.filter(aggregate, predicate);
    }
    expr_type over(expr_type function, const db_window_options& window) {
        return query_.over(function, window);
    }
    expr_type within_group(expr_type function, std::span<const db_order_term> order) {
        return query_.within_group(function, order);
    }
    expr_type extract(db_date_part part, expr_type value) {
        return query_.extract(part, value);
    }
    expr_type subscript(expr_type array_value, expr_type index) {
        return query_.subscript(array_value, index);
    }
    expr_type collate(expr_type value, std::string_view collation) {
        return query_.collate(value, collation);
    }
    expr_type import_expression(expr_type expression, std::string_view source_qualifier = {}, std::string_view target_qualifier = {}) {
        return query_.import_expression(expression, source_qualifier, target_qualifier);
    }

private:
    template <typename, typename>
    friend class db_query_builder;
    db_query query_;
};
}  // namespace ruvia
