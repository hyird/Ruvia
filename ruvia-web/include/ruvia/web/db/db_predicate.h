#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_parameter_pack.h"

namespace ruvia {

// An owning condition. Literal arguments are copied during construction, so a
// condition may outlive the strings used to build it. Moving leaves the source empty.
class db_predicate final {
public:
    db_predicate() = default;
    db_predicate(const db_predicate& other) {
        if (!other.empty()) {
            query_.emplace(other.query_->resource());
            expression_ = other.expression(*query_);
        }
    }
    db_predicate& operator=(const db_predicate& other) {
        if (this != &other) {
            db_predicate copy(other);
            *this = std::move(copy);
        }
        return *this;
    }
    db_predicate(db_predicate&& other) noexcept
        : query_(std::move(other.query_)),
          expression_(std::exchange(other.expression_, db_expression{})) {}
    db_predicate& operator=(db_predicate&& other) noexcept {
        if (this != &other) {
            query_ = std::move(other.query_);
            expression_ = std::exchange(other.expression_, db_expression{});
        }
        return *this;
    }
    [[nodiscard]] static db_predicate from_expression(db_expression expression) {
        if (expression.empty()) {
            throw std::invalid_argument("cannot construct a database condition from an empty expression");
        }
        db_predicate result;
        result.query_.emplace();
        result.expression_ = result.query_->import_expression(expression);
        return result;
    }
    [[nodiscard]] static db_predicate exists(const db_query& subquery) {
        db_predicate result;
        result.query_.emplace();
        result.expression_ = result.query_->exists(subquery);
        return result;
    }
    [[nodiscard]] static db_predicate not_exists(const db_query& subquery) {
        return !exists(subquery);
    }
    [[nodiscard]] bool empty() const noexcept {
        return expression_.empty();
    }
    [[nodiscard]] db_expression expression(db_query& query, std::string_view table = {}, std::string_view alias = {}) const {
        return empty() ? db_expression{} : query.import_expression(expression_, alias.empty() ? std::string_view{} : table, alias);
    }
    db_predicate& and_where(const db_predicate& other) {
        if (other.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        if (empty()) {
            query_.emplace();
            expression_ = other.expression(*query_);
        } else {
            expression_ = query_->binary(expression_, db_binary_operator::and_value, other.expression(*query_));
        }
        return *this;
    }
    db_predicate& or_where(const db_predicate& other) {
        if (other.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        if (empty()) {
            query_.emplace();
            expression_ = other.expression(*query_);
        } else {
            expression_ = query_->binary(expression_, db_binary_operator::or_value, other.expression(*query_));
        }
        return *this;
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

class db_where_builder final {
public:
    db_where_builder() = default;
    db_where_builder(const db_where_builder&) = delete;
    db_where_builder& operator=(const db_where_builder&) = delete;
    db_where_builder(db_where_builder&&) noexcept = default;
    db_where_builder& operator=(db_where_builder&&) noexcept = default;

    db_where_builder& where(const db_predicate& condition) {
        if (condition.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        predicate_ = db_predicate{};
        predicate_.and_where(condition);
        return *this;
    }
    db_where_builder& and_where(const db_predicate& condition) {
        predicate_.and_where(condition);
        return *this;
    }
    db_where_builder& or_where(const db_predicate& condition) {
        predicate_.or_where(condition);
        return *this;
    }
    template <typename fn_type>
        requires std::invocable<fn_type&, db_where_builder&>
    db_where_builder& where(fn_type&& fn) {
        db_where_builder nested;
        fn(nested);
        return where(nested.predicate_);
    }
    template <typename fn_type>
        requires std::invocable<fn_type&, db_where_builder&>
    db_where_builder& and_where(fn_type&& fn) {
        db_where_builder nested;
        fn(nested);
        return and_where(nested.predicate_);
    }
    template <typename fn_type>
        requires std::invocable<fn_type&, db_where_builder&>
    db_where_builder& or_where(fn_type&& fn) {
        db_where_builder nested;
        fn(nested);
        return or_where(nested.predicate_);
    }
    [[nodiscard]] bool empty() const noexcept {
        return predicate_.empty();
    }
    [[nodiscard]] db_predicate predicate() && noexcept {
        return std::move(predicate_);
    }

private:
    db_predicate predicate_{};
};

template <typename fn_type>
    requires std::invocable<fn_type&, db_where_builder&>
[[nodiscard]] db_predicate db_brackets(fn_type&& fn) {
    db_where_builder builder;
    fn(builder);
    if (builder.empty()) {
        throw std::invalid_argument("database brackets require a nonempty condition");
    }
    return std::move(builder).predicate();
}

template <typename fn_type>
    requires std::invocable<fn_type&, db_where_builder&>
[[nodiscard]] db_predicate db_not_brackets(fn_type&& fn) {
    return !db_brackets(std::forward<fn_type>(fn));
}

class db_expr;
struct db_order_spec;
struct db_window_spec;
struct db_expr_case_branch;
class db_tuple;

class db_expr final {
public:
    db_expr() noexcept = default;
    db_expr(detail::db_owned_expression owned) noexcept
        : owned_(std::move(owned)) {}

    template <typename e_type, fixed_string field_name>
    db_expr(db_field_reference<e_type, field_name> field);

    [[nodiscard]] static db_expr from_expression(db_expression expression, std::string_view table = {}, bool aliased = false) {
        if (expression.empty()) {
            throw std::invalid_argument("cannot construct db_expr from an empty expression");
        }
        db_query query;
        const auto imported = query.import_expression(expression);
        return db_expr(detail::db_owned_expression(std::move(query), imported, table, aliased));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] static db_expr value(v_type&& val) {
        db_query query;
        const auto expr = query.value(detail::make_immediate_db_parameter(std::forward<v_type>(val)));
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr null() {
        db_query query;
        const auto expr = query.null_value();
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr default_value() {
        db_query query;
        const auto expr = query.default_value();
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr star(std::string_view table = {}) {
        db_query query;
        const auto expr = query.star(table);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr column(std::string_view name, std::string_view table = {}) {
        db_query query;
        const auto expr = query.column(name, table);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr excluded(std::string_view name) {
        db_query query;
        const auto expr = query.excluded(name);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr raw_sql(std::string_view expression) {
        db_query query;
        const auto expr = query.sql(expression);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr raw_sql(std::span<const std::string_view> parts, std::span<const db_expr> args) {
        db_query query;
        std::pmr::vector<db_expression> imported(query.resource());
        imported.reserve(args.size());
        for (const auto& arg : args) {
            imported.push_back(arg.expression(query));
        }
        const auto expr = query.sql(parts, imported);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr raw_sql(std::initializer_list<std::string_view> parts, std::initializer_list<db_expr> args) {
        return raw_sql(std::span<const std::string_view>(parts.begin(), parts.size()),
            std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr now() {
        return call("now");
    }
    [[nodiscard]] static db_expr clock_timestamp() {
        return call("clock_timestamp");
    }
    [[nodiscard]] static db_expr interval(std::string_view spec) {
        return value(spec).cast(db_data_type::interval);
    }
    [[nodiscard]] static db_expr call(std::string_view function, std::span<const db_expr> args = {}) {
        db_query query;
        std::pmr::vector<db_expression> imported(query.resource());
        imported.reserve(args.size());
        for (const auto& arg : args) {
            imported.push_back(arg.expression(query));
        }
        const auto expr = query.call(function, imported);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr call(std::string_view function, std::initializer_list<db_expr> args) {
        return call(function, std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr coalesce(std::span<const db_expr> args) {
        db_query query;
        std::pmr::vector<db_expression> imported(query.resource());
        imported.reserve(args.size());
        for (const auto& arg : args) {
            imported.push_back(arg.expression(query));
        }
        const auto expr = query.coalesce(imported);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr coalesce(std::initializer_list<db_expr> args) {
        return coalesce(std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr null_if(const db_expr& lhs, const db_expr& rhs) {
        db_query query;
        const auto expr = query.null_if(lhs.expression(query), rhs.expression(query));
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr greatest(std::span<const db_expr> args) {
        db_query query;
        std::pmr::vector<db_expression> imported(query.resource());
        imported.reserve(args.size());
        for (const auto& arg : args) {
            imported.push_back(arg.expression(query));
        }
        const auto expr = query.greatest(imported);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr greatest(std::initializer_list<db_expr> args) {
        return greatest(std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr least(std::span<const db_expr> args) {
        db_query query;
        std::pmr::vector<db_expression> imported(query.resource());
        imported.reserve(args.size());
        for (const auto& arg : args) {
            imported.push_back(arg.expression(query));
        }
        const auto expr = query.least(imported);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr least(std::initializer_list<db_expr> args) {
        return least(std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static inline db_expr case_when(std::span<const db_expr_case_branch> branches, const db_expr& otherwise = {});
    [[nodiscard]] static inline db_expr case_when(std::initializer_list<db_expr_case_branch> branches, const db_expr& otherwise = {});

    [[nodiscard]] static db_expr jsonb_build_object(std::span<const db_expr> args) {
        return call("jsonb_build_object", args);
    }
    [[nodiscard]] static db_expr jsonb_build_object(std::initializer_list<db_expr> args) {
        return call("jsonb_build_object", args);
    }
    [[nodiscard]] static db_expr jsonb_build_array(std::span<const db_expr> args) {
        return call("jsonb_build_array", args);
    }
    [[nodiscard]] static db_expr jsonb_build_array(std::initializer_list<db_expr> args) {
        return call("jsonb_build_array", args);
    }
    [[nodiscard]] static db_expr jsonb_set(const db_expr& target, std::span<const std::string_view> path,
        const db_expr& new_value, bool create_if_missing = true) {
        db_query query;
        std::pmr::vector<db_expression> segments(query.resource());
        segments.reserve(path.size());
        for (const auto segment : path) {
            segments.push_back(query.value(segment));
        }
        const auto path_array = query.cast(query.array(segments), {.data_type_ = db_data_type::text, .array_ = true});
        const std::array args{target.expression(query), path_array, new_value.expression(query), query.value(create_if_missing)};
        const auto expr = query.call("jsonb_set", args);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static db_expr jsonb_set(const db_expr& target, std::initializer_list<std::string_view> path,
        const db_expr& new_value, bool create_if_missing = true) {
        return jsonb_set(target, std::span<const std::string_view>(path.begin(), path.size()), new_value, create_if_missing);
    }
    [[nodiscard]] static db_expr jsonb_strip_nulls(const db_expr& target) {
        return call("jsonb_strip_nulls", {target});
    }

    [[nodiscard]] static inline db_expr aggregate(std::string_view function, std::span<const db_expr> args,
        bool distinct = false, std::span<const db_order_spec> order = {});
    [[nodiscard]] static inline db_expr aggregate(std::string_view function, std::initializer_list<db_expr> args,
        bool distinct = false, std::initializer_list<db_order_spec> order = {});
    [[nodiscard]] static db_expr count() {
        db_query query;
        const std::array args{query.star()};
        const auto expr = query.aggregate("count", args);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] static inline db_expr count(const db_expr& arg, bool distinct = false);
    [[nodiscard]] static inline db_expr sum(const db_expr& arg, bool distinct = false);
    [[nodiscard]] static inline db_expr min(const db_expr& arg);
    [[nodiscard]] static inline db_expr max(const db_expr& arg);
    [[nodiscard]] static inline db_expr avg(const db_expr& arg, bool distinct = false);
    [[nodiscard]] static inline db_expr bool_and(const db_expr& arg);
    [[nodiscard]] static inline db_expr bool_or(const db_expr& arg);
    [[nodiscard]] static inline db_expr jsonb_agg(const db_expr& arg, std::span<const db_order_spec> order = {});
    [[nodiscard]] static inline db_expr jsonb_agg(const db_expr& arg, std::initializer_list<db_order_spec> order);

    [[nodiscard]] bool empty() const noexcept {
        return owned_.empty();
    }
    [[nodiscard]] db_expression expression() const noexcept {
        return owned_.expression();
    }
    [[nodiscard]] db_expression expression(db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
        if (owned_.empty()) {
            throw std::invalid_argument("cannot evaluate an empty database expression");
        }
        return owned_.import_into(target, table, alias);
    }
    [[nodiscard]] db_selection as(std::string_view alias_name) const {
        db_query query;
        const auto expr = expression(query);
        return db_selection(std::string(alias_name), expr, detail::db_owned_expression(std::move(query), expr));
    }

    [[nodiscard]] db_expr cast(const db_type_definition& type) const {
        db_query query;
        const auto expr = query.cast(expression(query), type);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] db_expr cast(db_data_type type) const {
        return cast(db_type_definition{.data_type_ = type});
    }
    [[nodiscard]] db_expr at_time_zone(const db_expr& zone) const {
        db_query query;
        const auto expr = query.at_time_zone(expression(query), zone.expression(query));
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] db_expr at_time_zone(std::string_view zone) const {
        return at_time_zone(value(zone));
    }
    [[nodiscard]] db_expr extract(db_date_part part) const {
        db_query query;
        const auto expr = query.extract(part, expression(query));
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] db_expr subscript(const db_expr& index) const {
        db_query query;
        const auto expr = query.subscript(expression(query), index.expression(query));
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] db_expr subscript(std::int64_t index) const {
        return subscript(value(index));
    }
    [[nodiscard]] db_expr operator[](std::int64_t index) const {
        return subscript(index);
    }
    [[nodiscard]] db_expr collate(std::string_view collation) const {
        db_query query;
        const auto expr = query.collate(expression(query), collation);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] db_expr concat(const db_expr& rhs) const {
        return binary(db_binary_operator::concat, rhs);
    }
    [[nodiscard]] db_expr concat(std::string_view rhs) const {
        return binary(db_binary_operator::concat, value(rhs));
    }
    [[nodiscard]] db_expr json_get(std::string_view key) const {
        return binary(db_binary_operator::json_get, value(key));
    }
    [[nodiscard]] db_expr json_get(std::int64_t index) const {
        return binary(db_binary_operator::json_get, value(index));
    }
    [[nodiscard]] db_expr json_get_text(std::string_view key) const {
        return binary(db_binary_operator::json_get_text, value(key));
    }
    [[nodiscard]] db_expr json_get_text(std::int64_t index) const {
        return binary(db_binary_operator::json_get_text, value(index));
    }
    [[nodiscard]] db_expr json_path(std::span<const std::string_view> path) const {
        return apply_json_path(db_binary_operator::json_path, path);
    }
    [[nodiscard]] db_expr json_path(std::initializer_list<std::string_view> path) const {
        return json_path(std::span<const std::string_view>(path.begin(), path.size()));
    }
    [[nodiscard]] db_expr json_path_text(std::span<const std::string_view> path) const {
        return apply_json_path(db_binary_operator::json_path_text, path);
    }
    [[nodiscard]] db_expr json_path_text(std::initializer_list<std::string_view> path) const {
        return json_path_text(std::span<const std::string_view>(path.begin(), path.size()));
    }
    [[nodiscard]] db_expr json_delete(std::string_view key) const {
        return binary(db_binary_operator::json_delete, value(key));
    }
    [[nodiscard]] db_expr json_delete(std::int64_t index) const {
        return binary(db_binary_operator::json_delete, value(index));
    }
    [[nodiscard]] db_expr json_delete_path(std::span<const std::string_view> path) const {
        return apply_json_path(db_binary_operator::json_delete_path, path);
    }
    [[nodiscard]] db_expr json_delete_path(std::initializer_list<std::string_view> path) const {
        return json_delete_path(std::span<const std::string_view>(path.begin(), path.size()));
    }
    [[nodiscard]] db_expr json_concat(const db_expr& rhs) const {
        return binary(db_binary_operator::json_concat, rhs);
    }
    [[nodiscard]] db_expr filter(const db_predicate& predicate) const {
        if (predicate.empty()) {
            throw std::invalid_argument("aggregate FILTER requires a nonempty condition");
        }
        db_query query;
        const auto expr = query.filter(expression(query), predicate.expression(query));
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] inline db_expr over(const db_window_spec& window) const;
    [[nodiscard]] inline db_expr within_group(std::span<const db_order_spec> order) const;
    [[nodiscard]] inline db_expr within_group(std::initializer_list<db_order_spec> order) const;
    [[nodiscard]] inline db_order_spec asc(db_nulls_order nulls = db_nulls_order::default_value) const;
    [[nodiscard]] inline db_order_spec desc(db_nulls_order nulls = db_nulls_order::default_value) const;

    [[nodiscard]] db_expr operator-() const {
        return unary_expr(db_unary_operator::negate);
    }
    [[nodiscard]] db_expr operator~() const {
        return unary_expr(db_unary_operator::bit_not);
    }
    [[nodiscard]] db_expr operator+(const db_expr& rhs) const {
        return binary(db_binary_operator::add, rhs);
    }
    [[nodiscard]] db_expr operator-(const db_expr& rhs) const {
        return binary(db_binary_operator::subtract, rhs);
    }
    [[nodiscard]] db_expr operator*(const db_expr& rhs) const {
        return binary(db_binary_operator::multiply, rhs);
    }
    [[nodiscard]] db_expr operator/(const db_expr& rhs) const {
        return binary(db_binary_operator::divide, rhs);
    }
    [[nodiscard]] db_expr operator%(const db_expr& rhs) const {
        return binary(db_binary_operator::modulo, rhs);
    }
    [[nodiscard]] db_expr operator&(const db_expr& rhs) const {
        return binary(db_binary_operator::bit_and, rhs);
    }
    [[nodiscard]] db_expr operator|(const db_expr& rhs) const {
        return binary(db_binary_operator::bit_or, rhs);
    }
    [[nodiscard]] db_expr operator^(const db_expr& rhs) const {
        return binary(db_binary_operator::bit_xor, rhs);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_expr operator+(v_type&& rhs) const {
        return binary(db_binary_operator::add, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_expr operator-(v_type&& rhs) const {
        return binary(db_binary_operator::subtract, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_expr operator*(v_type&& rhs) const {
        return binary(db_binary_operator::multiply, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_expr operator/(v_type&& rhs) const {
        return binary(db_binary_operator::divide, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_expr operator%(v_type&& rhs) const {
        return binary(db_binary_operator::modulo, value(std::forward<v_type>(rhs)));
    }

    [[nodiscard]] db_predicate operator==(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::equal, rhs);
    }
    [[nodiscard]] db_predicate operator!=(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::not_equal, rhs);
    }
    [[nodiscard]] db_predicate operator<(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::less, rhs);
    }
    [[nodiscard]] db_predicate operator<=(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::less_equal, rhs);
    }
    [[nodiscard]] db_predicate operator>(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::greater, rhs);
    }
    [[nodiscard]] db_predicate operator>=(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::greater_equal, rhs);
    }
    [[nodiscard]] db_predicate is_distinct_from(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::is_distinct_from, rhs);
    }
    [[nodiscard]] db_predicate is_not_distinct_from(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::is_not_distinct_from, rhs);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator==(v_type&& rhs) const {
        return compare_expr(db_binary_operator::equal, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator!=(v_type&& rhs) const {
        return compare_expr(db_binary_operator::not_equal, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator<(v_type&& rhs) const {
        return compare_expr(db_binary_operator::less, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator<=(v_type&& rhs) const {
        return compare_expr(db_binary_operator::less_equal, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator>(v_type&& rhs) const {
        return compare_expr(db_binary_operator::greater, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator>=(v_type&& rhs) const {
        return compare_expr(db_binary_operator::greater_equal, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_distinct_from(v_type&& rhs) const {
        return compare_expr(db_binary_operator::is_distinct_from, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_not_distinct_from(v_type&& rhs) const {
        return compare_expr(db_binary_operator::is_not_distinct_from, value(std::forward<v_type>(rhs)));
    }
    [[nodiscard]] db_predicate is_null() const {
        return compare_expr(db_binary_operator::equal, null());
    }
    [[nodiscard]] db_predicate is_not_null() const {
        return compare_expr(db_binary_operator::not_equal, null());
    }
    [[nodiscard]] db_predicate is_true() const {
        return unary_predicate(db_unary_operator::is_true);
    }
    [[nodiscard]] db_predicate is_false() const {
        return unary_predicate(db_unary_operator::is_false);
    }
    [[nodiscard]] db_predicate is_not_true() const {
        return unary_predicate(db_unary_operator::is_not_true);
    }
    [[nodiscard]] db_predicate is_not_false() const {
        return unary_predicate(db_unary_operator::is_not_false);
    }
    [[nodiscard]] db_predicate like(std::string_view pattern) const {
        return compare_expr(db_binary_operator::like, value(pattern));
    }
    [[nodiscard]] db_predicate not_like(std::string_view pattern) const {
        return compare_expr(db_binary_operator::not_like, value(pattern));
    }
    [[nodiscard]] db_predicate ilike(std::string_view pattern) const {
        return compare_expr(db_binary_operator::i_like, value(pattern));
    }
    [[nodiscard]] db_predicate not_ilike(std::string_view pattern) const {
        return compare_expr(db_binary_operator::not_i_like, value(pattern));
    }
    [[nodiscard]] db_predicate regex(std::string_view pattern) const {
        return compare_expr(db_binary_operator::regex, value(pattern));
    }
    [[nodiscard]] db_predicate iregex(std::string_view pattern) const {
        return compare_expr(db_binary_operator::regex_insensitive, value(pattern));
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate between(lower_type&& lower, upper_type&& upper) const {
        db_query query;
        const auto expr = query.between(expression(query),
            query.value(detail::make_immediate_db_parameter(std::forward<lower_type>(lower))),
            query.value(detail::make_immediate_db_parameter(std::forward<upper_type>(upper))), false);
        return db_predicate::from_expression(expr);
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate not_between(lower_type&& lower, upper_type&& upper) const {
        db_query query;
        const auto expr = query.between(expression(query),
            query.value(detail::make_immediate_db_parameter(std::forward<lower_type>(lower))),
            query.value(detail::make_immediate_db_parameter(std::forward<upper_type>(upper))), true);
        return db_predicate::from_expression(expr);
    }

private:
    [[nodiscard]] db_expr binary(db_binary_operator op, const db_expr& rhs) const {
        db_query query;
        const auto expr = query.binary(expression(query), op, rhs.expression(query));
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] db_expr unary_expr(db_unary_operator op) const {
        db_query query;
        const auto expr = query.unary(op, expression(query));
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] db_expr apply_json_path(db_binary_operator op, std::span<const std::string_view> path) const {
        db_query query;
        std::pmr::vector<db_expression> segments(query.resource());
        segments.reserve(path.size());
        for (const auto segment : path) {
            segments.push_back(query.value(segment));
        }
        const auto path_array = query.cast(query.array(segments), {.data_type_ = db_data_type::text, .array_ = true});
        const auto expr = query.binary(expression(query), op, path_array);
        return db_expr(detail::db_owned_expression(std::move(query), expr));
    }
    [[nodiscard]] db_predicate compare_expr(db_binary_operator op, const db_expr& rhs) const {
        db_query query;
        const auto expr = query.binary(expression(query), op, rhs.expression(query));
        return db_predicate::from_expression(expr);
    }
    [[nodiscard]] db_predicate unary_predicate(db_unary_operator op) const {
        db_query query;
        const auto expr = query.unary(op, expression(query));
        return db_predicate::from_expression(expr);
    }

    detail::db_owned_expression owned_;
};

struct db_order_spec final {
    db_expr expression_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};

struct db_window_spec final {
    std::vector<db_expr> partition_by_{};
    std::vector<db_order_spec> order_by_{};
    std::optional<db_window_frame_options> frame_{};
};

struct db_expr_case_branch final {
    db_predicate when_{};
    db_expr then_{};
};

class db_tuple final {
public:
    db_tuple(std::span<const db_expr> elements) {
        if (elements.empty()) {
            throw std::invalid_argument("database tuple requires at least one element");
        }
        db_query query;
        std::pmr::vector<db_expression> imported(query.resource());
        imported.reserve(elements.size());
        for (const auto& element : elements) {
            imported.push_back(element.expression(query));
        }
        const auto expr = query.tuple(imported);
        owned_ = detail::db_owned_expression(std::move(query), expr);
    }
    db_tuple(std::initializer_list<db_expr> elements)
        : db_tuple(std::span<const db_expr>(elements.begin(), elements.size())) {}

    [[nodiscard]] db_expression expression(db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
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
        db_query query;
        const auto expr = query.binary(expression(query), op, rhs.expression(query));
        return db_predicate::from_expression(expr);
    }
    [[nodiscard]] db_predicate compare_list(db_binary_operator op, std::span<const db_tuple> rows) const {
        db_query query;
        std::pmr::vector<db_expression> list_items(query.resource());
        list_items.reserve(rows.size());
        for (const auto& row : rows) {
            list_items.push_back(row.expression(query));
        }
        const auto expr = query.binary(expression(query), op, query.list(list_items));
        return db_predicate::from_expression(expr);
    }

    detail::db_owned_expression owned_{};
};

inline db_expr db_expr::case_when(std::span<const db_expr_case_branch> branches, const db_expr& otherwise) {
    if (branches.empty()) {
        throw std::invalid_argument("CASE requires a branch");
    }
    db_query query;
    std::pmr::vector<db_case_branch> imported(query.resource());
    imported.reserve(branches.size());
    for (const auto& branch : branches) {
        if (branch.when_.empty()) {
            throw std::invalid_argument("CASE WHEN requires a nonempty condition");
        }
        imported.push_back({branch.when_.expression(query), branch.then_.expression(query)});
    }
    const auto else_expr = otherwise.empty() ? db_expression{} : otherwise.expression(query);
    const auto expr = query.case_when(imported, else_expr);
    return db_expr(detail::db_owned_expression(std::move(query), expr));
}

inline db_expr db_expr::case_when(std::initializer_list<db_expr_case_branch> branches, const db_expr& otherwise) {
    return case_when(std::span<const db_expr_case_branch>(branches.begin(), branches.size()), otherwise);
}

inline db_expr db_expr::aggregate(std::string_view function, std::span<const db_expr> args,
    bool distinct, std::span<const db_order_spec> order) {
    db_query query;
    std::pmr::vector<db_expression> imported_args(query.resource());
    imported_args.reserve(args.size());
    for (const auto& arg : args) {
        imported_args.push_back(arg.expression(query));
    }
    std::pmr::vector<db_order_term> imported_order(query.resource());
    imported_order.reserve(order.size());
    for (const auto& term : order) {
        imported_order.push_back({term.expression_.expression(query), term.direction_, term.nulls_});
    }
    const auto expr = query.aggregate(function, imported_args, distinct, imported_order);
    return db_expr(detail::db_owned_expression(std::move(query), expr));
}

inline db_expr db_expr::aggregate(std::string_view function, std::initializer_list<db_expr> args,
    bool distinct, std::initializer_list<db_order_spec> order) {
    return aggregate(function, std::span<const db_expr>(args.begin(), args.size()), distinct,
        std::span<const db_order_spec>(order.begin(), order.size()));
}

inline db_expr db_expr::count(const db_expr& arg, bool distinct) {
    const std::array args{arg};
    return aggregate("count", args, distinct);
}
inline db_expr db_expr::sum(const db_expr& arg, bool distinct) {
    const std::array args{arg};
    return aggregate("sum", args, distinct);
}
inline db_expr db_expr::min(const db_expr& arg) {
    const std::array args{arg};
    return aggregate("min", args);
}
inline db_expr db_expr::max(const db_expr& arg) {
    const std::array args{arg};
    return aggregate("max", args);
}
inline db_expr db_expr::avg(const db_expr& arg, bool distinct) {
    const std::array args{arg};
    return aggregate("avg", args, distinct);
}
inline db_expr db_expr::bool_and(const db_expr& arg) {
    const std::array args{arg};
    return aggregate("bool_and", args);
}
inline db_expr db_expr::bool_or(const db_expr& arg) {
    const std::array args{arg};
    return aggregate("bool_or", args);
}
inline db_expr db_expr::jsonb_agg(const db_expr& arg, std::span<const db_order_spec> order) {
    const std::array args{arg};
    return aggregate("jsonb_agg", args, false, order);
}
inline db_expr db_expr::jsonb_agg(const db_expr& arg, std::initializer_list<db_order_spec> order) {
    return jsonb_agg(arg, std::span<const db_order_spec>(order.begin(), order.size()));
}

inline db_expr db_expr::over(const db_window_spec& window) const {
    db_query query;
    db_window_options options;
    options.partition_by_.reserve(window.partition_by_.size());
    for (const auto& item : window.partition_by_) {
        options.partition_by_.push_back(item.expression(query));
    }
    options.order_by_.reserve(window.order_by_.size());
    for (const auto& term : window.order_by_) {
        options.order_by_.push_back({term.expression_.expression(query), term.direction_, term.nulls_});
    }
    options.frame_ = window.frame_;
    const auto expr = query.over(expression(query), options);
    return db_expr(detail::db_owned_expression(std::move(query), expr));
}

inline db_expr db_expr::within_group(std::span<const db_order_spec> order) const {
    db_query query;
    std::pmr::vector<db_order_term> terms(query.resource());
    terms.reserve(order.size());
    for (const auto& term : order) {
        terms.push_back({term.expression_.expression(query), term.direction_, term.nulls_});
    }
    const auto expr = query.within_group(expression(query), terms);
    return db_expr(detail::db_owned_expression(std::move(query), expr));
}

inline db_expr db_expr::within_group(std::initializer_list<db_order_spec> order) const {
    return within_group(std::span<const db_order_spec>(order.begin(), order.size()));
}

inline db_order_spec db_expr::asc(db_nulls_order nulls) const {
    return db_order_spec{*this, db_order_direction::asc, nulls};
}
inline db_order_spec db_expr::desc(db_nulls_order nulls) const {
    return db_order_spec{*this, db_order_direction::desc, nulls};
}

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
        db_query query;
        const auto expr = expression(query);
        return db_expr(detail::db_owned_expression(std::move(query), expr, e_type::table_name(), !alias_.empty()));
    }
    [[nodiscard]] db_expr excluded_expr() const {
        db_query query;
        const auto expr = excluded(query);
        return db_expr(detail::db_owned_expression(std::move(query), expr, e_type::table_name(), true));
    }
    [[nodiscard]] db_order_spec asc(db_nulls_order nulls = db_nulls_order::default_value) const {
        return to_expr().asc(nulls);
    }
    [[nodiscard]] db_order_spec desc(db_nulls_order nulls = db_nulls_order::default_value) const {
        return to_expr().desc(nulls);
    }
    [[nodiscard]] db_expr cast(const db_type_definition& type) const {
        return to_expr().cast(type);
    }
    [[nodiscard]] db_expr cast(db_data_type type) const {
        return to_expr().cast(type);
    }
    [[nodiscard]] db_expr at_time_zone(const db_expr& zone) const {
        return to_expr().at_time_zone(zone);
    }
    [[nodiscard]] db_expr at_time_zone(std::string_view zone) const {
        return to_expr().at_time_zone(zone);
    }
    [[nodiscard]] db_expr extract(db_date_part part) const {
        return to_expr().extract(part);
    }
    [[nodiscard]] db_expr subscript(const db_expr& index) const {
        return to_expr().subscript(index);
    }
    [[nodiscard]] db_expr subscript(std::int64_t index) const {
        return to_expr().subscript(index);
    }
    [[nodiscard]] db_expr operator[](std::int64_t index) const {
        return to_expr().subscript(index);
    }
    [[nodiscard]] db_expr concat(const db_expr& rhs) const {
        return to_expr().concat(rhs);
    }
    [[nodiscard]] db_expr concat(std::string_view rhs) const {
        return to_expr().concat(rhs);
    }
    [[nodiscard]] db_expr json_get(std::string_view key) const {
        return to_expr().json_get(key);
    }
    [[nodiscard]] db_expr json_get(std::int64_t index) const {
        return to_expr().json_get(index);
    }
    [[nodiscard]] db_expr json_get_text(std::string_view key) const {
        return to_expr().json_get_text(key);
    }
    [[nodiscard]] db_expr json_get_text(std::int64_t index) const {
        return to_expr().json_get_text(index);
    }
    [[nodiscard]] db_expr json_path(std::span<const std::string_view> path) const {
        return to_expr().json_path(path);
    }
    [[nodiscard]] db_expr json_path(std::initializer_list<std::string_view> path) const {
        return to_expr().json_path(path);
    }
    [[nodiscard]] db_expr json_path_text(std::span<const std::string_view> path) const {
        return to_expr().json_path_text(path);
    }
    [[nodiscard]] db_expr json_path_text(std::initializer_list<std::string_view> path) const {
        return to_expr().json_path_text(path);
    }
    [[nodiscard]] db_expr json_delete(std::string_view key) const {
        return to_expr().json_delete(key);
    }
    [[nodiscard]] db_expr json_delete(std::int64_t index) const {
        return to_expr().json_delete(index);
    }
    [[nodiscard]] db_expr json_delete_path(std::span<const std::string_view> path) const {
        return to_expr().json_delete_path(path);
    }
    [[nodiscard]] db_expr json_delete_path(std::initializer_list<std::string_view> path) const {
        return to_expr().json_delete_path(path);
    }
    [[nodiscard]] db_expr json_concat(const db_expr& rhs) const {
        return to_expr().json_concat(rhs);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_expr operator+(v_type&& rhs) const {
        return to_expr() + std::forward<v_type>(rhs);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_expr operator-(v_type&& rhs) const {
        return to_expr() - std::forward<v_type>(rhs);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_expr operator*(v_type&& rhs) const {
        return to_expr() * std::forward<v_type>(rhs);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_expr operator/(v_type&& rhs) const {
        return to_expr() / std::forward<v_type>(rhs);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_expr operator%(v_type&& rhs) const {
        return to_expr() % std::forward<v_type>(rhs);
    }
    [[nodiscard]] db_expr operator+(const db_expr& rhs) const {
        return to_expr() + rhs;
    }
    [[nodiscard]] db_expr operator-(const db_expr& rhs) const {
        return to_expr() - rhs;
    }
    [[nodiscard]] db_expr operator*(const db_expr& rhs) const {
        return to_expr() * rhs;
    }
    [[nodiscard]] db_expr operator/(const db_expr& rhs) const {
        return to_expr() / rhs;
    }
    [[nodiscard]] db_expr operator%(const db_expr& rhs) const {
        return to_expr() % rhs;
    }

    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator==(v_type&& value) const {
        return compare(db_binary_operator::equal, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator!=(v_type&& value) const {
        return compare(db_binary_operator::not_equal, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator<(v_type&& value) const {
        return compare(db_binary_operator::less, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator<=(v_type&& value) const {
        return compare(db_binary_operator::less_equal, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator>(v_type&& value) const {
        return compare(db_binary_operator::greater, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate operator>=(v_type&& value) const {
        return compare(db_binary_operator::greater_equal, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_distinct_from(v_type&& value) const {
        return compare(db_binary_operator::is_distinct_from, std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_not_distinct_from(v_type&& value) const {
        return compare(db_binary_operator::is_not_distinct_from, std::forward<v_type>(value));
    }

    template <typename other_entity, fixed_string other_field>
    [[nodiscard]] db_predicate operator==(db_field_reference<other_entity, other_field> other) const {
        return compare_column(db_binary_operator::equal, other);
    }
    template <typename other_entity, fixed_string other_field>
    [[nodiscard]] db_predicate operator!=(db_field_reference<other_entity, other_field> other) const {
        return compare_column(db_binary_operator::not_equal, other);
    }
    template <typename other_entity, fixed_string other_field>
    [[nodiscard]] db_predicate operator<(db_field_reference<other_entity, other_field> other) const {
        return compare_column(db_binary_operator::less, other);
    }
    template <typename other_entity, fixed_string other_field>
    [[nodiscard]] db_predicate operator<=(db_field_reference<other_entity, other_field> other) const {
        return compare_column(db_binary_operator::less_equal, other);
    }
    template <typename other_entity, fixed_string other_field>
    [[nodiscard]] db_predicate operator>(db_field_reference<other_entity, other_field> other) const {
        return compare_column(db_binary_operator::greater, other);
    }
    template <typename other_entity, fixed_string other_field>
    [[nodiscard]] db_predicate operator>=(db_field_reference<other_entity, other_field> other) const {
        return compare_column(db_binary_operator::greater_equal, other);
    }
    template <typename other_entity, fixed_string other_field>
    [[nodiscard]] db_predicate is_distinct_from(db_field_reference<other_entity, other_field> other) const {
        return compare_column(db_binary_operator::is_distinct_from, other);
    }
    template <typename other_entity, fixed_string other_field>
    [[nodiscard]] db_predicate is_not_distinct_from(db_field_reference<other_entity, other_field> other) const {
        return compare_column(db_binary_operator::is_not_distinct_from, other);
    }

    [[nodiscard]] db_predicate operator==(db_expression rhs) const {
        return compare_expression(db_binary_operator::equal, rhs);
    }
    [[nodiscard]] db_predicate operator!=(db_expression rhs) const {
        return compare_expression(db_binary_operator::not_equal, rhs);
    }
    [[nodiscard]] db_predicate operator<(db_expression rhs) const {
        return compare_expression(db_binary_operator::less, rhs);
    }
    [[nodiscard]] db_predicate operator<=(db_expression rhs) const {
        return compare_expression(db_binary_operator::less_equal, rhs);
    }
    [[nodiscard]] db_predicate operator>(db_expression rhs) const {
        return compare_expression(db_binary_operator::greater, rhs);
    }
    [[nodiscard]] db_predicate operator>=(db_expression rhs) const {
        return compare_expression(db_binary_operator::greater_equal, rhs);
    }
    [[nodiscard]] db_predicate is_distinct_from(db_expression rhs) const {
        return compare_expression(db_binary_operator::is_distinct_from, rhs);
    }
    [[nodiscard]] db_predicate is_not_distinct_from(db_expression rhs) const {
        return compare_expression(db_binary_operator::is_not_distinct_from, rhs);
    }
    [[nodiscard]] db_predicate operator==(const db_expr& rhs) const {
        return to_expr() == rhs;
    }
    [[nodiscard]] db_predicate operator!=(const db_expr& rhs) const {
        return to_expr() != rhs;
    }
    [[nodiscard]] db_predicate operator<(const db_expr& rhs) const {
        return to_expr() < rhs;
    }
    [[nodiscard]] db_predicate operator<=(const db_expr& rhs) const {
        return to_expr() <= rhs;
    }
    [[nodiscard]] db_predicate operator>(const db_expr& rhs) const {
        return to_expr() > rhs;
    }
    [[nodiscard]] db_predicate operator>=(const db_expr& rhs) const {
        return to_expr() >= rhs;
    }
    [[nodiscard]] db_predicate is_distinct_from(const db_expr& rhs) const {
        return to_expr().is_distinct_from(rhs);
    }
    [[nodiscard]] db_predicate is_not_distinct_from(const db_expr& rhs) const {
        return to_expr().is_not_distinct_from(rhs);
    }

    [[nodiscard]] db_predicate like(std::string_view pattern) const {
        return compare(db_binary_operator::like, pattern);
    }
    [[nodiscard]] db_predicate not_like(std::string_view pattern) const {
        return compare(db_binary_operator::not_like, pattern);
    }
    [[nodiscard]] db_predicate ilike(std::string_view pattern) const {
        return compare(db_binary_operator::i_like, pattern);
    }
    [[nodiscard]] db_predicate not_ilike(std::string_view pattern) const {
        return compare(db_binary_operator::not_i_like, pattern);
    }
    [[nodiscard]] db_predicate regex(std::string_view pattern) const {
        return compare(db_binary_operator::regex, pattern);
    }
    [[nodiscard]] db_predicate iregex(std::string_view pattern) const {
        return compare(db_binary_operator::regex_insensitive, pattern);
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate between(lower_type&& lower, upper_type&& upper) const {
        return compare_between(std::forward<lower_type>(lower), std::forward<upper_type>(upper), false);
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate not_between(lower_type&& lower, upper_type&& upper) const {
        return compare_between(std::forward<lower_type>(lower), std::forward<upper_type>(upper), true);
    }
    [[nodiscard]] db_predicate is_null() const {
        return compare(db_binary_operator::equal, nullptr);
    }
    [[nodiscard]] db_predicate is_not_null() const {
        return compare(db_binary_operator::not_equal, nullptr);
    }
    [[nodiscard]] db_predicate is_true() const {
        return apply_unary(db_unary_operator::is_true);
    }
    [[nodiscard]] db_predicate is_false() const {
        return apply_unary(db_unary_operator::is_false);
    }
    [[nodiscard]] db_predicate is_not_true() const {
        return apply_unary(db_unary_operator::is_not_true);
    }
    [[nodiscard]] db_predicate is_not_false() const {
        return apply_unary(db_unary_operator::is_not_false);
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
        requires requires(const subquery_type& s, db_query& q) {
            { s.subquery(q) } -> std::same_as<db_expression>;
        }
    [[nodiscard]] db_predicate in(const subquery_type& subquery) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.binary(expression(query), db_binary_operator::in, subquery.subquery(query));
        return result;
    }
    template <typename subquery_type>
        requires requires(const subquery_type& s, db_query& q) {
            { s.subquery(q) } -> std::same_as<db_expression>;
        }
    [[nodiscard]] db_predicate not_in(const subquery_type& subquery) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.binary(expression(query), db_binary_operator::not_in, subquery.subquery(query));
        return result;
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
        return compare(db_binary_operator::json_has_key, key);
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
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        const auto extracted = query.binary(expression(query), db_binary_operator::json_get_text, query.value(key));
        result.expression_ = query.binary(extracted, db_binary_operator::equal, query.value(value));
        return result;
    }
    [[nodiscard]] db_predicate json_path_text_equals(std::span<const std::string_view> path, std::string_view value) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        std::pmr::vector<db_expression> segments(query.resource());
        segments.reserve(path.size());
        for (const auto segment : path) {
            segments.push_back(query.value(segment));
        }
        const auto path_array = query.cast(query.array(segments), {.data_type_ = db_data_type::text, .array_ = true});
        const auto extracted = query.binary(expression(query), db_binary_operator::json_path_text, path_array);
        result.expression_ = query.binary(extracted, db_binary_operator::equal, query.value(value));
        return result;
    }
    [[nodiscard]] db_predicate json_path_text_equals(std::initializer_list<std::string_view> path, std::string_view value) const {
        return json_path_text_equals(std::span<const std::string_view>(path.begin(), path.size()), value);
    }

    [[nodiscard]] db_selection select() const {
        return as(field_name.view());
    }
    [[nodiscard]] db_selection as(std::string_view alias_name) const {
        db_query query;
        const auto expr = expression(query);
        return make_selection(alias_name, std::move(query), expr);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_selection coalesce(v_type&& fallback, std::string_view alias_name = field_name.view()) const {
        db_query query;
        const auto expr = query.coalesce({expression(query), query.value(detail::make_immediate_db_parameter(std::forward<v_type>(fallback)))});
        return make_selection(alias_name, std::move(query), expr);
    }
    [[nodiscard]] db_selection coalesce_text(std::string_view fallback = {}, std::string_view alias_name = field_name.view()) const {
        db_query query;
        auto col_expr = expression(query);
        if constexpr (column_type::data_type != db_data_type::text &&
                      column_type::data_type != db_data_type::varchar &&
                      column_type::data_type != db_data_type::char_value) {
            col_expr = query.cast(col_expr, db_data_type::text);
        }
        const auto expr = query.coalesce({col_expr, query.value(fallback)});
        return make_selection(alias_name, std::move(query), expr);
    }

    template <detail::db_parameter v_type>
        requires(is_writable && (!std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t> || column_type::options.nullable_))
    [[nodiscard]] db_assignment set(v_type&& value) const {
        db_query query;
        const auto expr = query.value(detail::make_immediate_db_parameter(std::forward<v_type>(value)));
        return make_assignment(std::move(query), expr);
    }
    template <detail::db_parameter v_type>
        requires(is_writable && (!std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t> || column_type::options.nullable_))
    [[nodiscard]] db_assignment operator=(v_type&& value) const {
        return set(std::forward<v_type>(value));
    }
    template <typename other_entity, fixed_string other_field>
        requires is_writable
    [[nodiscard]] db_assignment set(db_field_reference<other_entity, other_field> other) const {
        db_query query;
        const auto expr = other.expression(query);
        return db_assignment{
            .column_ = std::string(field_name.view()),
            .value_ = expr,
            .owned_ = detail::db_owned_expression(std::move(query), expr, other_entity::table_name(), !other.alias().empty()),
        };
    }
    [[nodiscard]] db_assignment set(db_expression value) const
        requires is_writable
    {
        if (value.empty()) {
            throw std::invalid_argument("cannot assign an empty database expression");
        }
        db_query query;
        const auto expr = query.import_expression(value);
        return make_assignment(std::move(query), expr);
    }
    [[nodiscard]] db_assignment set(const db_expr& value) const
        requires is_writable
    {
        if (value.empty()) {
            throw std::invalid_argument("cannot assign an empty database expression");
        }
        db_query query;
        const auto expr = value.expression(query);
        return make_assignment(std::move(query), expr);
    }
    [[nodiscard]] db_assignment operator=(const db_expr& value) const
        requires is_writable
    {
        return set(value);
    }
    [[nodiscard]] db_assignment set_null() const
        requires(is_writable && column_type::options.nullable_)
    {
        db_query query;
        const auto expr = query.null_value();
        return make_assignment(std::move(query), expr);
    }
    [[nodiscard]] db_assignment set_null_if_empty(std::string_view value) const
        requires(is_writable && column_type::options.nullable_)
    {
        db_query query;
        auto expr = query.null_if(query.value(value), query.value(std::string_view{}));
        if constexpr (!column_type::options.enum_name_.view().empty()) {
            expr = query.cast(expr, {.custom_name_ = std::string(column_type::options.enum_name_.view())});
        } else if constexpr (column_type::data_type != db_data_type::inferred &&
                             column_type::data_type != db_data_type::text &&
                             column_type::data_type != db_data_type::varchar &&
                             column_type::data_type != db_data_type::char_value) {
            expr = query.cast(expr, {.data_type_ = column_type::data_type,
                                        .length_ = column_type::options.length_,
                                        .precision_ = column_type::options.precision_,
                                        .scale_ = column_type::options.scale_});
        }
        return make_assignment(std::move(query), expr);
    }
    [[nodiscard]] db_assignment set_default() const
        requires is_writable
    {
        db_query query;
        const auto expr = query.default_value();
        return make_assignment(std::move(query), expr);
    }
    [[nodiscard]] db_assignment set_now() const
        requires is_writable
    {
        db_query query;
        const auto expr = query.call("now");
        return make_assignment(std::move(query), expr);
    }
    [[nodiscard]] db_assignment set_excluded() const
        requires is_writable
    {
        db_query query;
        const auto expr = query.excluded(field_name.view());
        return make_assignment(std::move(query), expr);
    }
    template <typename number_type = std::int64_t>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment increment(number_type value = number_type{1}) const {
        return adjust(db_binary_operator::add, value);
    }
    template <typename number_type>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment operator+=(number_type value) const {
        return increment(value);
    }
    template <typename number_type = std::int64_t>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment decrement(number_type value = number_type{1}) const {
        return adjust(db_binary_operator::subtract, value);
    }
    template <typename number_type>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment operator-=(number_type value) const {
        return decrement(value);
    }

private:
    [[nodiscard]] db_selection make_selection(std::string_view alias_name, db_query&& query, db_expression expr) const {
        return db_selection(
            std::string(alias_name), expr, detail::db_owned_expression(std::move(query), expr, e_type::table_name(), !alias_.empty()));
    }
    [[nodiscard]] db_assignment make_assignment(db_query&& query, db_expression expr) const {
        return db_assignment{
            .column_ = std::string(field_name.view()),
            .value_ = expr,
            .owned_ = detail::db_owned_expression(std::move(query), expr, e_type::table_name(), !alias_.empty()),
        };
    }
    template <typename number_type>
    [[nodiscard]] db_assignment adjust(db_binary_operator op, number_type value) const {
        db_query query;
        const auto expr = query.binary(expression(query), op, query.value(value));
        return make_assignment(std::move(query), expr);
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate compare_between(lower_type&& lower, upper_type&& upper, bool negate) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.between(expression(query),
            query.value(detail::make_immediate_db_parameter(std::forward<lower_type>(lower))),
            query.value(detail::make_immediate_db_parameter(std::forward<upper_type>(upper))),
            negate);
        return result;
    }
    [[nodiscard]] db_predicate apply_unary(db_unary_operator op) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.unary(op, expression(query));
        return result;
    }
    template <typename t_type>
    [[nodiscard]] db_predicate compare_list(db_binary_operator op, std::span<const t_type> values) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        std::pmr::vector<db_expression> args(query.resource());
        args.reserve(values.size());
        for (const auto& value : values) {
            args.push_back(query.value(detail::make_immediate_db_parameter(value)));
        }
        result.expression_ = query.binary(expression(query), op, query.list(args));
        return result;
    }
    [[nodiscard]] db_predicate compare_subquery(db_binary_operator op, const db_query& subquery) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.binary(expression(query), op, query.subquery(subquery));
        return result;
    }
    [[nodiscard]] db_predicate compare_json(db_binary_operator op, std::string_view json_text) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.binary(expression(query), op, query.cast(query.value(json_text), db_data_type::jsonb));
        return result;
    }
    [[nodiscard]] db_predicate compare_json_keys(db_binary_operator op, std::span<const std::string_view> keys) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        std::pmr::vector<db_expression> args(query.resource());
        args.reserve(keys.size());
        for (const auto key : keys) {
            args.push_back(query.value(key));
        }
        const auto key_array = query.cast(query.array(args), {.data_type_ = db_data_type::text, .array_ = true});
        result.expression_ = query.binary(expression(query), op, key_array);
        return result;
    }
    template <typename t_type>
    db_predicate compare_array(db_binary_operator op, std::span<const t_type> values) const {
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
    template <typename other_entity, fixed_string other_field>
    db_predicate compare_column(db_binary_operator op, db_field_reference<other_entity, other_field> other) const {
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.binary(expression(query), op, other.expression(query));
        return result;
    }
    db_predicate compare_expression(db_binary_operator op, db_expression rhs) const {
        if (rhs.empty()) {
            throw std::invalid_argument("cannot compare a column against an empty database expression");
        }
        db_predicate result;
        result.query_.emplace();
        auto& query = *result.query_;
        result.expression_ = query.binary(expression(query), op, query.import_expression(rhs));
        return result;
    }

    std::string_view alias_{};
};

template <typename e_type, fixed_string field_name>
db_expr::db_expr(db_field_reference<e_type, field_name> field)
    : db_expr(field.to_expr()) {}

template <fixed_string table, typename... members_type>
template <fixed_string name>
constexpr db_field_reference<db_entity<table, members_type...>, name> db_entity<table, members_type...>::column(std::string_view alias) noexcept {
    return db_field_reference<db_entity<table, members_type...>, name>(alias);
}

}  // namespace ruvia
