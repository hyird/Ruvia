#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_parameter_pack.h"

namespace ruvia {

class db_tuple final {
public:
    db_tuple(std::span<const db_expr> elements)
        : owned_(detail::db_owned_expression::make_tuple(elements)) {}
    db_tuple(std::initializer_list<db_expr> elements)
        : db_tuple(std::span<const db_expr>(elements.begin(), elements.size())) {}

    [[nodiscard]] db_expression expression(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
        return owned_.import_into(target, table, alias);
    }

    [[nodiscard]] db_predicate operator==(const db_tuple& rhs) const {
        return compare(db_binary_operator::equal, rhs);
    }
    [[nodiscard]] db_predicate operator!=(const db_tuple& rhs) const {
        return compare(db_binary_operator::not_equal, rhs);
    }
    [[nodiscard]] db_predicate operator<(const db_tuple& rhs) const {
        return compare(db_binary_operator::less, rhs);
    }
    [[nodiscard]] db_predicate operator<=(const db_tuple& rhs) const {
        return compare(db_binary_operator::less_equal, rhs);
    }
    [[nodiscard]] db_predicate operator>(const db_tuple& rhs) const {
        return compare(db_binary_operator::greater, rhs);
    }
    [[nodiscard]] db_predicate operator>=(const db_tuple& rhs) const {
        return compare(db_binary_operator::greater_equal, rhs);
    }
    [[nodiscard]] db_predicate is_distinct_from(const db_tuple& rhs) const {
        return compare(db_binary_operator::is_distinct_from, rhs);
    }
    [[nodiscard]] db_predicate is_not_distinct_from(const db_tuple& rhs) const {
        return compare(db_binary_operator::is_not_distinct_from, rhs);
    }
    [[nodiscard]] db_predicate in(std::span<const db_tuple> rows) const {
        return compare_list(db_binary_operator::in, rows);
    }
    [[nodiscard]] db_predicate in(std::initializer_list<db_tuple> rows) const {
        return in(std::span<const db_tuple>(rows.begin(), rows.size()));
    }
    [[nodiscard]] db_predicate not_in(std::span<const db_tuple> rows) const {
        return compare_list(db_binary_operator::not_in, rows);
    }
    [[nodiscard]] db_predicate not_in(std::initializer_list<db_tuple> rows) const {
        return not_in(std::span<const db_tuple>(rows.begin(), rows.size()));
    }

private:
    [[nodiscard]] db_predicate compare(db_binary_operator op, const db_tuple& rhs) const {
        return db_predicate(detail::db_owned_expression::make_binary(owned_, op, rhs.owned_));
    }
    [[nodiscard]] db_predicate compare_list(db_binary_operator op, std::span<const db_tuple> rows) const {
        std::vector<detail::db_owned_expression> items;
        items.reserve(rows.size());
        for (const auto& row : rows) {
            items.push_back(row.owned_);
        }
        return db_predicate(detail::db_owned_expression::make_in_expr_list(owned_, op, items));
    }

    detail::db_owned_expression owned_{};
};

template <typename e_type, fixed_string field_name>
class db_field_reference final {
    static_assert(sql_entity<e_type>, "SQL field references require a SQL entity");
    static_assert(e_type::template column_index<field_name>() < std::tuple_size_v<typename e_type::columns_type>);
    using column_type = std::tuple_element_t<e_type::template column_index<field_name>(), typename e_type::columns_type>;
    static constexpr bool is_writable = column_type::options.generated_type_ == db_generated_type::none && !column_type::options.read_only_;
    static constexpr bool is_numeric = !detail::is_pmr_vector<typename column_type::value_type>::value &&
                                       (column_type::data_type == db_data_type::small_int ||
                                           column_type::data_type == db_data_type::integer ||
                                           column_type::data_type == db_data_type::big_int ||
                                           column_type::data_type == db_data_type::numeric ||
                                           column_type::data_type == db_data_type::real ||
                                           column_type::data_type == db_data_type::double_value);

public:
    constexpr db_field_reference(std::string_view alias = {}) noexcept
        : alias_(alias) {}
    constexpr db_field_reference(const db_field_reference&) noexcept = default;
    constexpr db_field_reference(db_field_reference&&) noexcept = default;
    constexpr db_field_reference& operator=(const db_field_reference&) noexcept = default;
    constexpr db_field_reference& operator=(db_field_reference&&) noexcept = default;

    [[nodiscard]] static constexpr std::string_view name() noexcept {
        return field_name.view();
    }
    [[nodiscard]] static constexpr std::string_view table_name() noexcept {
        return e_type::table_name();
    }
    [[nodiscard]] constexpr std::string_view alias() const noexcept {
        return alias_;
    }
    [[nodiscard]] constexpr db_field_reference with_alias(std::string_view alias) const noexcept {
        return db_field_reference(alias);
    }
    [[nodiscard]] db_expression expression(db_query& query, std::string_view fallback_alias = {}) const {
        const auto qualifier = !alias_.empty() ? alias_ : (fallback_alias.empty() ? e_type::table_name() : fallback_alias);
        return query.column(field_name.view(), qualifier);
    }
    [[nodiscard]] db_expression excluded(db_query& query) const {
        return query.excluded(field_name.view());
    }
    [[nodiscard]] db_expr to_expr() const {
        return db_expr(detail::db_owned_expression::make_static_column(
            field_name.view(), e_type::table_name(), alias_));
    }
    [[nodiscard]] operator db_expr() const {
        return to_expr();
    }
    [[nodiscard]] db_expr excluded_expr() const noexcept {
        return db_expr(detail::db_owned_expression::make_static_excluded(
            field_name.view(), e_type::table_name()));
    }

    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_distinct_from(v_type&& value) const {
        return to_expr().is_distinct_from(std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_not_distinct_from(v_type&& value) const {
        return to_expr().is_not_distinct_from(std::forward<v_type>(value));
    }
    [[nodiscard]] db_predicate is_distinct_from(const db_expr& rhs) const {
        return to_expr().is_distinct_from(rhs);
    }
    [[nodiscard]] db_predicate is_not_distinct_from(const db_expr& rhs) const {
        return to_expr().is_not_distinct_from(rhs);
    }

    [[nodiscard]] db_predicate like(std::string_view pattern) const {
        return to_expr().like(pattern);
    }
    [[nodiscard]] db_predicate not_like(std::string_view pattern) const {
        return to_expr().not_like(pattern);
    }
    [[nodiscard]] db_predicate ilike(std::string_view pattern) const {
        return to_expr().ilike(pattern);
    }
    [[nodiscard]] db_predicate not_ilike(std::string_view pattern) const {
        return to_expr().not_ilike(pattern);
    }
    [[nodiscard]] db_predicate regex(std::string_view pattern) const {
        return to_expr().regex(pattern);
    }
    [[nodiscard]] db_predicate iregex(std::string_view pattern) const {
        return to_expr().iregex(pattern);
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate between(lower_type&& lower, upper_type&& upper) const {
        return to_expr().between(std::forward<lower_type>(lower), std::forward<upper_type>(upper));
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate not_between(lower_type&& lower, upper_type&& upper) const {
        return to_expr().not_between(std::forward<lower_type>(lower), std::forward<upper_type>(upper));
    }
    [[nodiscard]] db_predicate is_null() const {
        return to_expr().is_null();
    }
    [[nodiscard]] db_predicate is_not_null() const {
        return to_expr().is_not_null();
    }
    [[nodiscard]] db_predicate is_true() const {
        return to_expr().is_true();
    }
    [[nodiscard]] db_predicate is_false() const {
        return to_expr().is_false();
    }
    [[nodiscard]] db_predicate is_not_true() const {
        return to_expr().is_not_true();
    }
    [[nodiscard]] db_predicate is_not_false() const {
        return to_expr().is_not_false();
    }
    template <typename t_type>
    [[nodiscard]] db_predicate in(std::span<const t_type> values) const {
        return compare_list(db_binary_operator::in, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate in(std::initializer_list<t_type> values) const {
        return in(std::span<const t_type>(values.begin(), values.size()));
    }
    template <typename t_type>
    [[nodiscard]] db_predicate not_in(std::span<const t_type> values) const {
        return compare_list(db_binary_operator::not_in, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate not_in(std::initializer_list<t_type> values) const {
        return not_in(std::span<const t_type>(values.begin(), values.size()));
    }
    [[nodiscard]] db_predicate in(const db_query& subquery) const {
        return compare_subquery(db_binary_operator::in, subquery);
    }
    [[nodiscard]] db_predicate not_in(const db_query& subquery) const {
        return compare_subquery(db_binary_operator::not_in, subquery);
    }
    template <typename subquery_type>
        requires requires(const subquery_type& s) {
            { s.subquery_query() } -> std::same_as<const db_query&>;
        }
    [[nodiscard]] db_predicate in(const subquery_type& subquery) const {
        return compare_subquery(db_binary_operator::in, subquery.subquery_query());
    }
    template <typename subquery_type>
        requires requires(const subquery_type& s) {
            { s.subquery_query() } -> std::same_as<const db_query&>;
        }
    [[nodiscard]] db_predicate not_in(const subquery_type& subquery) const {
        return compare_subquery(db_binary_operator::not_in, subquery.subquery_query());
    }

    template <typename t_type>
    [[nodiscard]] db_predicate array_contains(std::span<const t_type> values) const {
        return compare_array(db_binary_operator::array_contains, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_contains(std::initializer_list<t_type> values) const {
        return array_contains(std::span<const t_type>(values.begin(), values.size()));
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_contained_by(std::span<const t_type> values) const {
        return compare_array(db_binary_operator::array_contained_by, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_contained_by(std::initializer_list<t_type> values) const {
        return array_contained_by(std::span<const t_type>(values.begin(), values.size()));
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_overlap(std::span<const t_type> values) const {
        return compare_array(db_binary_operator::array_overlap, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_overlap(std::initializer_list<t_type> values) const {
        return array_overlap(std::span<const t_type>(values.begin(), values.size()));
    }

    [[nodiscard]] db_predicate json_contains(std::string_view json_text) const {
        return compare_json(db_binary_operator::json_contains, json_text);
    }
    [[nodiscard]] db_predicate json_contained_by(std::string_view json_text) const {
        return compare_json(db_binary_operator::json_contained_by, json_text);
    }
    [[nodiscard]] db_predicate json_has_key(std::string_view key) const {
        return to_expr().compare_expr(db_binary_operator::json_has_key, db_expr::value(key));
    }
    [[nodiscard]] db_predicate json_has_any_key(std::span<const std::string_view> keys) const {
        return compare_json_keys(db_binary_operator::json_has_any_key, keys);
    }
    [[nodiscard]] db_predicate json_has_any_key(std::initializer_list<std::string_view> keys) const {
        return json_has_any_key(std::span<const std::string_view>(keys.begin(), keys.size()));
    }
    [[nodiscard]] db_predicate json_has_all_keys(std::span<const std::string_view> keys) const {
        return compare_json_keys(db_binary_operator::json_has_all_keys, keys);
    }
    [[nodiscard]] db_predicate json_has_all_keys(std::initializer_list<std::string_view> keys) const {
        return json_has_all_keys(std::span<const std::string_view>(keys.begin(), keys.size()));
    }
    [[nodiscard]] db_predicate json_text_equals(std::string_view key, std::string_view value) const {
        return to_expr().json_get_text(key) == value;
    }
    [[nodiscard]] db_predicate json_path_text_equals(std::span<const std::string_view> path, std::string_view value) const {
        return to_expr().json_path_text(path) == value;
    }
    [[nodiscard]] db_predicate json_path_text_equals(std::initializer_list<std::string_view> path, std::string_view value) const {
        return json_path_text_equals(std::span<const std::string_view>(path.begin(), path.size()), value);
    }

    [[nodiscard]] db_selection select() const {
        return as(field_name.view());
    }
    [[nodiscard]] db_selection as(std::string_view alias_name) const {
        return to_expr().as(alias_name);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_selection coalesce(v_type&& fallback, std::string_view alias_name = field_name.view()) const {
        return db_expr::coalesce({to_expr(), db_expr::value(std::forward<v_type>(fallback))}).as(alias_name);
    }
    [[nodiscard]] db_selection coalesce_text(std::string_view fallback = {}, std::string_view alias_name = field_name.view()) const {
        auto col_expr = to_expr();
        if constexpr (column_type::data_type != db_data_type::text &&
                      column_type::data_type != db_data_type::varchar &&
                      column_type::data_type != db_data_type::char_value) {
            col_expr = col_expr.cast(db_data_type::text);
        }
        return db_expr::coalesce({col_expr, db_expr::value(fallback)}).as(alias_name);
    }

    template <detail::db_parameter v_type>
        requires(is_writable && (!std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t> || column_type::options.nullable_))
    [[nodiscard]] db_assignment set(v_type&& value) const {
        return db_assignment{std::string(field_name.view()), db_expr::value(std::forward<v_type>(value))};
    }
    template <detail::db_parameter v_type>
        requires(is_writable && (!std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t> || column_type::options.nullable_))
    [[nodiscard]] db_assignment operator=(v_type&& value) const {
        return set(std::forward<v_type>(value));
    }
    [[nodiscard]] db_assignment set(const db_expr& value) const
        requires is_writable
    {
        if (value.empty()) {
            throw std::invalid_argument("cannot assign an empty database expression");
        }
        return db_assignment{std::string(field_name.view()), value};
    }
    [[nodiscard]] db_assignment operator=(const db_expr& value) const
        requires is_writable
    {
        return set(value);
    }
    [[nodiscard]] db_assignment set_null() const
        requires(is_writable && column_type::options.nullable_)
    {
        return db_assignment{std::string(field_name.view()), db_expr::null()};
    }
    [[nodiscard]] db_assignment set_null_if_empty(std::string_view value) const
        requires(is_writable && column_type::options.nullable_)
    {
        auto expr = db_expr::null_if(db_expr::value(value), db_expr::value(std::string_view{}));
        if constexpr (!column_type::options.enum_name_.view().empty()) {
            expr = expr.cast({.custom_name_ = std::string(column_type::options.enum_name_.view())});
        } else if constexpr (column_type::data_type != db_data_type::inferred &&
                             column_type::data_type != db_data_type::text &&
                             column_type::data_type != db_data_type::varchar &&
                             column_type::data_type != db_data_type::char_value) {
            expr = expr.cast({.data_type_ = column_type::data_type,
                .length_ = column_type::options.length_,
                .precision_ = column_type::options.precision_,
                .scale_ = column_type::options.scale_});
        }
        return db_assignment{std::string(field_name.view()), std::move(expr)};
    }
    [[nodiscard]] db_assignment set_default() const
        requires is_writable
    {
        return db_assignment{std::string(field_name.view()), db_expr::default_value()};
    }
    [[nodiscard]] db_assignment set_now() const
        requires is_writable
    {
        return db_assignment{std::string(field_name.view()), db_expr::now()};
    }
    [[nodiscard]] db_assignment set_excluded() const
        requires is_writable
    {
        return db_assignment{std::string(field_name.view()), excluded_expr()};
    }
    template <typename number_type = std::int64_t>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment increment(number_type value = number_type{1}) const {
        return db_assignment{std::string(field_name.view()), to_expr() + value};
    }
    template <typename number_type>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment operator+=(number_type value) const {
        return increment(value);
    }
    template <typename number_type = std::int64_t>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment decrement(number_type value = number_type{1}) const {
        return db_assignment{std::string(field_name.view()), to_expr() - value};
    }
    template <typename number_type>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment operator-=(number_type value) const {
        return decrement(value);
    }

private:
    template <typename t_type>
    [[nodiscard]] db_predicate compare_list(db_binary_operator op, std::span<const t_type> values) const {
        std::vector<db_value> items;
        items.reserve(values.size());
        for (const auto& value : values) {
            items.push_back(detail::make_immediate_db_parameter(value));
        }
        return db_predicate(detail::db_owned_expression::make_in_list(to_expr().owned(), op, items));
    }
    [[nodiscard]] db_predicate compare_subquery(db_binary_operator op, const db_query& subquery) const {
        return db_predicate(detail::db_owned_expression::make_in_subquery(to_expr().owned(), op, subquery));
    }
    [[nodiscard]] db_predicate compare_json(db_binary_operator op, std::string_view json_text) const {
        return to_expr().compare_expr(op, db_expr::value(json_text).cast(db_data_type::jsonb));
    }
    [[nodiscard]] db_predicate compare_json_keys(db_binary_operator op, std::span<const std::string_view> keys) const {
        return db_predicate(detail::db_owned_expression::make_json_keys_op(to_expr().owned(), op, keys));
    }
    template <typename t_type>
    db_predicate compare_array(db_binary_operator op, std::span<const t_type> values) const {
        static_assert(detail::is_pmr_vector<typename column_type::value_type>::value, "array predicates require an array column");
        using item_type = typename detail::is_pmr_vector<typename column_type::value_type>::value_type;
        constexpr auto type = column_type::options.data_type_ == db_data_type::inferred || column_type::options.data_type_ == db_data_type::array
                                  ? detail::db_entity_type_traits<item_type>::data_type
                                  : column_type::options.data_type_;
        std::vector<db_value> items;
        items.reserve(values.size());
        for (const auto& value : values) {
            items.push_back(detail::make_immediate_db_parameter(value));
        }
        const db_type_definition array_type{
            .data_type_ = type,
            .length_ = column_type::options.length_,
            .precision_ = column_type::options.precision_,
            .scale_ = column_type::options.scale_,
            .array_ = true,
        };
        return db_predicate(detail::db_owned_expression::make_array_op(to_expr().owned(), op, items, array_type));
    }

    std::string_view alias_{};
};

template <fixed_string table, typename... members_type>
template <fixed_string name>
constexpr db_field_reference<db_entity<table, members_type...>, name> db_entity<table, members_type...>::column(std::string_view alias) noexcept {
    return db_field_reference<db_entity<table, members_type...>, name>(alias);
}

}  // namespace ruvia
