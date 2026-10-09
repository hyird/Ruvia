#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_parameter_pack.h"

namespace ruvia {

namespace detail {
struct db_predicate_access;
}  // namespace detail

// An owning condition. Literal arguments are copied during construction, so a
// condition may outlive the strings used to build it.
class db_predicate final {
public:
    db_predicate() = default;
    db_predicate(const db_predicate&) = delete;
    db_predicate& operator=(const db_predicate&) = delete;
    db_predicate(db_predicate&&) noexcept = default;
    db_predicate& operator=(db_predicate&&) noexcept = default;
    [[nodiscard]] bool empty() const noexcept {
        return expression_.empty();
    }
    [[nodiscard]] db_expression expression(db_query& query, std::string_view table = {}, std::string_view alias = {}) const {
        return empty() ? db_expression{} : query.import_expression(expression_, alias.empty() ? std::string_view{} : table, alias);
    }
    friend db_predicate operator&&(db_predicate left, const db_predicate& right) {
        return combine(std::move(left), db_binary_operator::and_value, right);
    }
    friend db_predicate operator||(db_predicate left, const db_predicate& right) {
        return combine(std::move(left), db_binary_operator::or_value, right);
    }
    friend db_predicate operator!(db_predicate value) {
        if (value.empty()) {
            throw std::invalid_argument("cannot negate an empty database condition");
        }
        value.expression_ = value.query_->unary(db_unary_operator::not_value, value.expression_);
        return value;
    }

private:
    template <typename, fixed_string>
    friend class db_field_reference;
    friend struct detail::db_predicate_access;
    static db_predicate combine(db_predicate left, db_binary_operator op, const db_predicate& right) {
        if (left.empty() || right.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        left.expression_ = left.query_->binary(left.expression_, op, right.expression(*left.query_));
        return left;
    }
    std::optional<db_query> query_{};
    db_expression expression_{};
};

template <typename e_type, fixed_string field_name>
class db_field_reference final {
    static_assert(sql_entity<e_type>, "SQL field references require a SQL entity");
    static_assert(e_type::template column_index<field_name>() < std::tuple_size_v<typename e_type::columns_type>);

public:
    [[nodiscard]] static constexpr std::string_view name() noexcept {
        return field_name.view();
    }
    [[nodiscard]] db_expression expression(db_query& query, std::string_view alias = {}) const {
        return query.column(field_name.view(), alias.empty() ? e_type::table_name() : alias);
    }
    template <detail::db_parameter v_type>
    db_predicate operator==(v_type&& value) const {
        return compare(db_binary_operator::equal, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    db_predicate operator!=(v_type&& value) const {
        return compare(db_binary_operator::not_equal, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    db_predicate operator<(v_type&& value) const {
        return compare(db_binary_operator::less, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    db_predicate operator<=(v_type&& value) const {
        return compare(db_binary_operator::less_equal, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    db_predicate operator>(v_type&& value) const {
        return compare(db_binary_operator::greater, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    db_predicate operator>=(v_type&& value) const {
        return compare(db_binary_operator::greater_equal, std::forward<v_type>(value));
    }
    [[nodiscard]] db_predicate like(std::string_view pattern) const {
        return compare(db_binary_operator::like, pattern);
    }
    [[nodiscard]] db_predicate ilike(std::string_view pattern) const {
        return compare(db_binary_operator::i_like, pattern);
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate between(lower_type&& lower, upper_type&& upper) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.between(expression(query),
            query.value(detail::make_immediate_db_parameter(std::forward<lower_type>(lower))),
            query.value(detail::make_immediate_db_parameter(std::forward<upper_type>(upper))));
        return result;
    }
    [[nodiscard]] db_predicate is_null() const {
        return compare(db_binary_operator::equal, nullptr);
    }
    [[nodiscard]] db_predicate is_not_null() const {
        return compare(db_binary_operator::not_equal, nullptr);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate in(std::span<const t_type> values) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        std::pmr::vector<db_expression> args(query.resource());
        for (const auto& value : values) {
            args.push_back(query.value(detail::make_immediate_db_parameter(value)));
        }
        result.expression_ = query.binary(expression(query), db_binary_operator::in, query.list(args));
        return result;
    }
    template <typename t_type>
    [[nodiscard]] db_predicate in(std::initializer_list<t_type> values) const {
        return in(std::span<const t_type>(values.begin(), values.size()));
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

private:
    template <typename t_type>
    db_predicate compare_array(db_binary_operator op, std::span<const t_type> values) const {
        using column_type = std::tuple_element_t<e_type::template column_index<field_name>(), typename e_type::columns_type>;
        static_assert(detail::is_pmr_vector<typename column_type::value_type>::value, "array predicates require an array column");
        using item_type = typename detail::is_pmr_vector<typename column_type::value_type>::value_type;
        constexpr auto type = column_type::options.data_type_ == db_data_type::inferred || column_type::options.data_type_ == db_data_type::array
                                  ? detail::db_entity_type_traits<item_type>::data_type
                                  : column_type::options.data_type_;
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        std::pmr::vector<db_expression> args(query.resource());
        args.reserve(values.size());
        for (const auto& value : values) {
            args.push_back(query.value(detail::make_immediate_db_parameter(value)));
        }
        const auto array_value = query.cast(query.array(args), {.data_type_ = type,
                                                                   .length_ = column_type::options.length_,
                                                                   .precision_ = column_type::options.precision_,
                                                                   .scale_ = column_type::options.scale_,
                                                                   .array_ = true});
        result.expression_ = query.binary(expression(query), op, array_value);
        return result;
    }
    template <detail::db_parameter v_type>
    db_predicate compare(db_binary_operator op, v_type&& value) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.binary(expression(query), op, query.value(detail::make_immediate_db_parameter(std::forward<v_type>(value))));
        return result;
    }
};

template <fixed_string table, typename... columns_type>
template <fixed_string name>
db_field_reference<db_entity<table, columns_type...>, name> db_entity<table, columns_type...>::column() {
    return {};
}

}  // namespace ruvia
