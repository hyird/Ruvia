#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/web/db/db_session.h"
#include "ruvia/web/model.h"

namespace ruvia {

namespace detail {
// Expression and nested-query depth accepted by import, compilation and
// relation paths. Same-operator AND/OR chains do not nest.
inline constexpr std::size_t max_db_query_depth = 256;
class db_query_storage;
struct db_query_node;
enum class db_node_kind : std::uint8_t;
class db_query_compiler;
class db_relation_plan;
struct db_query_plan;
struct qualified_expression_importer;
struct db_expr_tree;
class db_owned_expression;
#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state;
#endif
}  // namespace detail

enum class db_parameter_mode : std::uint8_t { bound,
    literal };
enum class db_binary_operator : std::uint8_t {
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    and_value,
    or_value,
    add,
    subtract,
    multiply,
    divide,
    modulo,
    concat,
    like,
    not_like,
    i_like,
    not_i_like,
    in,
    not_in,
    is_distinct_from,
    is_not_distinct_from,
    bit_and,
    bit_or,
    bit_xor,
    json_get,
    json_get_text,
    json_path,
    json_path_text,
    json_contains,
    json_contained_by,
    json_has_key,
    json_has_any_key,
    json_has_all_keys,
    json_concat,
    json_delete,
    json_delete_path,
    array_contains,
    array_contained_by,
    array_overlap,
    regex,
    regex_insensitive,
    inet_contains,
    inet_contains_or_equal,
    inet_contained_by,
    inet_contained_by_or_equal,
    inet_overlap,
};
enum class db_unary_operator : std::uint8_t { not_value,
    negate,
    bit_not,
    is_null,
    is_not_null,
    is_true,
    is_false,
    is_not_true,
    is_not_false };
enum class db_join_type : std::uint8_t { inner,
    left,
    right,
    full,
    cross };
enum class db_order_direction : std::uint8_t { asc,
    desc };
enum class db_nulls_order : std::uint8_t { default_value,
    first,
    last };
enum class db_row_lock : std::uint8_t { update,
    no_key_update,
    share,
    key_share };
enum class db_materialization : std::uint8_t { default_value,
    materialized,
    not_materialized };
enum class db_set_operation : std::uint8_t { union_value,
    union_all,
    intersect,
    intersect_all,
    except,
    except_all };
enum class db_window_frame : std::uint8_t { rows,
    range,
    groups };
enum class db_frame_boundary : std::uint8_t { unbounded_preceding,
    preceding,
    current_row,
    following,
    unbounded_following };
enum class db_date_part : std::uint8_t { epoch,
    year,
    month,
    day,
    hour,
    minute,
    second,
    dow,
    doy,
    week,
    quarter };

struct db_type_definition final {
    db_data_type data_type_{db_data_type::inferred};
    borrowed_text custom_name_{};
    std::size_t length_{0};
    unsigned precision_{0};
    unsigned scale_{0};
    bool array_{false};
};

// Borrows the address-stable AST owned by its query. Import an expression or
// pass it to an ORM operation while that query is alive; no operation retains
// this view after returning its cold asynchronous operation.
class db_expression final {
public:
    db_expression() noexcept = default;
    [[nodiscard]] bool empty() const noexcept {
        return owner_ == nullptr;
    }

private:
    friend class db_query;
    friend class detail::db_owned_expression;
    friend class detail::db_query_compiler;
    friend class detail::db_relation_plan;
    db_expression(const detail::db_query_storage* owner_value, std::size_t node_value) noexcept
        : owner_(owner_value),
          node_(node_value) {}
    const detail::db_query_storage* owner_{nullptr};
    std::size_t node_{0};
};

struct db_window_boundary final {
    db_frame_boundary kind_{db_frame_boundary::current_row};
    query_row_count offset_{};
};
struct db_window_frame_options final {
    db_window_frame kind_{db_window_frame::rows};
    db_window_boundary start_{db_frame_boundary::unbounded_preceding};
    db_window_boundary end_{db_frame_boundary::current_row};
};
struct db_named_argument final {
    borrowed_text name_{};
    db_expression value_{};
};
struct db_source_column final {
    borrowed_text name_{};
    db_type_definition type_{};
};
// span 及元素中的文本均为借用；调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_source_options final {
    bool lateral_{false};
    bool with_ordinality_{false};
    borrowed_span<db_source_column> columns_{};
};
// span 及元素中的文本均为借用；with 入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_cte_options final {
    bool recursive_{false};
    db_materialization materialization_{db_materialization::default_value};
    borrowed_span<borrowed_text> columns_{};
};
// span 及元素中的文本均为借用；lock/set_lock 入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_lock_options final {
    db_row_lock mode_{db_row_lock::update};
    bool nowait_{false};
    bool skip_locked_{false};
    borrowed_span<borrowed_text> tables_{};
};

class db_query;
class db_predicate;
class db_expr;
class db_tuple;
struct db_order_term;
struct db_window_options;
struct db_case_branch;
struct db_selection;
struct db_assignment;

namespace detail {

class db_owned_expression final {
public:
    enum class leaf_kind : std::uint8_t {
        empty,
        borrowed,
        null_value,
        default_value,
        star,
        column,
        excluded,
        signed_value,
        unsigned_value,
        double_value,
        bool_value,
        tree,
    };

    db_owned_expression() noexcept;
    db_owned_expression(const db_owned_expression& other);
    db_owned_expression& operator=(const db_owned_expression& other);
    db_owned_expression(db_owned_expression&& other) noexcept;
    db_owned_expression& operator=(db_owned_expression&& other) noexcept;
    ~db_owned_expression();

    [[nodiscard]] bool empty() const noexcept {
        return kind_ == leaf_kind::empty;
    }
    [[nodiscard]] db_expression import_into(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const;

    [[nodiscard]] static db_owned_expression from_borrowed(
        db_expression expression, borrowed_text table = {}, bool aliased = false) noexcept;
    [[nodiscard]] static db_owned_expression from_expression(
        db_expression expression, std::string_view table = {}, bool aliased = false);
    [[nodiscard]] static db_owned_expression make_null() noexcept;
    [[nodiscard]] static db_owned_expression make_default() noexcept;
    [[nodiscard]] static db_owned_expression make_star(std::string_view table = {});
    [[nodiscard]] static db_owned_expression make_column(
        std::string_view name, std::string_view table = {}, bool explicit_alias = false);
    [[nodiscard]] static db_owned_expression make_static_column(
        std::string_view name, std::string_view table, std::string_view alias);
    [[nodiscard]] static db_owned_expression make_excluded(std::string_view name);
    [[nodiscard]] static db_owned_expression make_static_excluded(std::string_view name) noexcept;
    [[nodiscard]] static db_owned_expression make_value(const db_value& value);
    [[nodiscard]] static db_owned_expression make_raw_sql(
        std::span<const std::string_view> parts, std::span<const db_expr> args);
    [[nodiscard]] static db_owned_expression make_call(
        std::string_view function, std::span<const db_expr> args);
    [[nodiscard]] static db_owned_expression make_coalesce(std::span<const db_expr> args);
    [[nodiscard]] static db_owned_expression make_null_if(
        const db_owned_expression& lhs, const db_owned_expression& rhs);
    [[nodiscard]] static db_owned_expression make_greatest(std::span<const db_expr> args);
    [[nodiscard]] static db_owned_expression make_least(std::span<const db_expr> args);
    [[nodiscard]] static db_owned_expression make_case_when(
        std::span<const db_case_branch> branches, const db_owned_expression& otherwise);
    [[nodiscard]] static db_owned_expression make_jsonb_set(const db_owned_expression& target,
        std::span<const std::string_view> path, const db_owned_expression& new_value, bool create_if_missing);
    [[nodiscard]] static db_owned_expression make_json_path_op(
        const db_owned_expression& target, db_binary_operator op, std::span<const std::string_view> path);
    [[nodiscard]] static db_owned_expression make_aggregate(std::string_view function,
        std::span<const db_expr> args, bool distinct, std::span<const db_order_term> order);
    [[nodiscard]] static db_owned_expression make_count_star();
    [[nodiscard]] static db_owned_expression make_filter(
        const db_owned_expression& aggregate, const db_owned_expression& predicate);
    [[nodiscard]] static db_owned_expression make_over(
        const db_owned_expression& function, const db_window_options& window);
    [[nodiscard]] static db_owned_expression make_within_group(
        const db_owned_expression& function, std::span<const db_order_term> order);
    [[nodiscard]] static db_owned_expression make_cast(
        const db_owned_expression& operand, const db_type_definition& type);
    [[nodiscard]] static db_owned_expression make_at_time_zone(
        const db_owned_expression& operand, const db_owned_expression& zone);
    [[nodiscard]] static db_owned_expression make_extract(
        db_date_part part, const db_owned_expression& operand);
    [[nodiscard]] static db_owned_expression make_subscript(
        const db_owned_expression& array, const db_owned_expression& index);
    [[nodiscard]] static db_owned_expression make_collate(
        const db_owned_expression& operand, std::string_view collation);
    [[nodiscard]] static db_owned_expression make_binary(
        const db_owned_expression& lhs, db_binary_operator op, const db_owned_expression& rhs);
    [[nodiscard]] static db_owned_expression make_binary(
        db_owned_expression&& lhs, db_binary_operator op, const db_owned_expression& rhs);
    [[nodiscard]] static db_owned_expression make_unary(
        db_unary_operator op, const db_owned_expression& operand);
    [[nodiscard]] static db_owned_expression make_unary(
        db_unary_operator op, db_owned_expression&& operand);
    [[nodiscard]] static db_owned_expression make_between(const db_owned_expression& operand,
        const db_value& lower, const db_value& upper, bool negate);
    [[nodiscard]] static db_owned_expression make_tuple(std::span<const db_expr> elements);
    [[nodiscard]] static db_owned_expression make_in_list(const db_owned_expression& lhs,
        db_binary_operator op, std::size_t count, const void* context,
        db_value (*element_at)(const void* context, std::size_t index));
    template <typename t_type>
    [[nodiscard]] static db_owned_expression make_in_list(
        const db_owned_expression& lhs, db_binary_operator op, std::span<const t_type> values) {
        return make_in_list(lhs, op, values.size(), values.data(),
            [](const void* context, std::size_t index) -> db_value {
                return make_immediate_db_parameter(static_cast<const t_type*>(context)[index]);
            });
    }
    [[nodiscard]] static db_owned_expression make_in_expr_list(
        const db_owned_expression& lhs, db_binary_operator op, std::span<const db_tuple> values);
    [[nodiscard]] static db_owned_expression make_in_subquery(
        const db_owned_expression& lhs, db_binary_operator op, const db_query& subquery);
    [[nodiscard]] static db_owned_expression make_exists(const db_query& subquery);
    [[nodiscard]] static db_owned_expression make_array_op(const db_owned_expression& lhs,
        db_binary_operator op, std::size_t count, const void* context,
        db_value (*element_at)(const void* context, std::size_t index),
        const db_type_definition& array_type);
    template <typename t_type>
    [[nodiscard]] static db_owned_expression make_array_op(const db_owned_expression& lhs,
        db_binary_operator op, std::span<const t_type> values, const db_type_definition& array_type) {
        return make_array_op(lhs, op, values.size(), values.data(), [](const void* context, std::size_t index) -> db_value { return make_immediate_db_parameter(static_cast<const t_type*>(context)[index]); }, array_type);
    }

private:
    using tree_owner_type = std::unique_ptr<db_expr_tree>;

    [[nodiscard]] static tree_owner_type allocate_tree();
    [[nodiscard]] static db_owned_expression from_tree(tree_owner_type tree) noexcept;
    [[nodiscard]] static db_owned_expression make_variadic(
        db_node_kind kind, std::span<const db_expr> args, std::string_view function);
    [[nodiscard]] static db_owned_expression make_two_operand_node(
        db_node_kind kind, const db_owned_expression& lhs, const db_owned_expression& rhs);
    [[nodiscard]] std::size_t append_to_tree(db_expr_tree& target) const;
    leaf_kind kind_{leaf_kind::empty};
    bool aliased_{false};
    db_expression borrowed_{};
    std::string_view text_{};
    std::string_view table_{};
    union scalar_union {
        constexpr scalar_union() noexcept
            : signed_value_(0) {}
        std::int64_t signed_value_;
        std::uint64_t unsigned_value_;
        double double_value_;
        bool bool_value_;
    } scalar_{};
    tree_owner_type tree_;
};
}  // namespace detail

// An owning condition. Literal arguments are copied during construction, so a
// condition may outlive the strings used to build it. Moving leaves the source empty.
class db_predicate final {
public:
    db_predicate() noexcept = default;
    db_predicate(detail::db_owned_expression owned) noexcept
        : owned_(std::move(owned)) {}
    db_predicate(db_expression expression) noexcept
        : owned_(detail::db_owned_expression::from_borrowed(expression, {}, true)) {}
    db_predicate(const db_predicate&) = default;
    db_predicate& operator=(const db_predicate&) = default;
    db_predicate(db_predicate&&) noexcept = default;
    db_predicate& operator=(db_predicate&&) noexcept = default;

    [[nodiscard]] static db_predicate from_expression(db_expression expression) {
        return db_predicate(detail::db_owned_expression::from_expression(expression));
    }
    [[nodiscard]] static db_predicate exists(const db_query& subquery) {
        return db_predicate(detail::db_owned_expression::make_exists(subquery));
    }
    [[nodiscard]] static db_predicate not_exists(const db_query& subquery) {
        return !exists(subquery);
    }
    [[nodiscard]] bool empty() const noexcept {
        return owned_.empty();
    }
    [[nodiscard]] db_expression expression(
        db_query& query, std::string_view table = {}, std::string_view alias = {}) const {
        return owned_.empty() ? db_expression{} : owned_.import_into(query, table, alias);
    }
    db_predicate& and_where(const db_predicate& other) {
        if (other.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        if (empty()) {
            owned_ = other.owned_;
        } else {
            owned_ = detail::db_owned_expression::make_binary(
                std::move(owned_), db_binary_operator::and_value, other.owned_);
        }
        return *this;
    }
    db_predicate& or_where(const db_predicate& other) {
        if (other.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        if (empty()) {
            owned_ = other.owned_;
        } else {
            owned_ = detail::db_owned_expression::make_binary(
                std::move(owned_), db_binary_operator::or_value, other.owned_);
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
        return db_predicate(
            detail::db_owned_expression::make_unary(db_unary_operator::not_value, std::move(value.owned_)));
    }

private:
    friend class db_expr;
    template <typename, fixed_string>
    friend class db_field_reference;
    static db_predicate combine(db_predicate left, db_binary_operator op, const db_predicate& right) {
        if (left.empty() || right.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        return db_predicate(
            detail::db_owned_expression::make_binary(std::move(left.owned_), op, right.owned_));
    }
    detail::db_owned_expression owned_{};
};

class db_expr final {
public:
    db_expr() noexcept = default;
    db_expr(detail::db_owned_expression owned) noexcept
        : owned_(std::move(owned)) {}
    db_expr(db_expression expression, borrowed_text table = {}, bool aliased = false) noexcept
        : owned_(detail::db_owned_expression::from_borrowed(expression, table, aliased)) {}
    db_expr(const db_predicate& predicate)
        : owned_(predicate.owned_) {}
    db_expr(db_predicate&& predicate) noexcept
        : owned_(std::move(predicate.owned_)) {}

    [[nodiscard]] static db_expr from_expression(
        db_expression expression, std::string_view table = {}, bool aliased = false) {
        return db_expr(detail::db_owned_expression::from_expression(expression, table, aliased));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] static db_expr value(v_type&& val) {
        return db_expr(detail::db_owned_expression::make_value(
            detail::make_immediate_db_parameter(std::forward<v_type>(val))));
    }
    [[nodiscard]] static db_expr null() noexcept {
        return db_expr(detail::db_owned_expression::make_null());
    }
    [[nodiscard]] static db_expr default_value() noexcept {
        return db_expr(detail::db_owned_expression::make_default());
    }
    [[nodiscard]] static db_expr star(std::string_view table = {}) {
        return db_expr(detail::db_owned_expression::make_star(table));
    }
    [[nodiscard]] static db_expr column(std::string_view name, std::string_view table = {}) {
        return db_expr(detail::db_owned_expression::make_column(name, table));
    }
    [[nodiscard]] static db_expr excluded(std::string_view name) {
        return db_expr(detail::db_owned_expression::make_excluded(name));
    }
    [[nodiscard]] static db_expr raw_sql(std::string_view expression) {
        return raw_sql(std::span<const std::string_view>(&expression, 1), {});
    }
    [[nodiscard]] static db_expr raw_sql(std::span<const std::string_view> parts, std::span<const db_expr> args) {
        return db_expr(detail::db_owned_expression::make_raw_sql(parts, args));
    }
    [[nodiscard]] static db_expr raw_sql(
        std::initializer_list<std::string_view> parts, std::initializer_list<db_expr> args) {
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
        return db_expr(detail::db_owned_expression::make_call(function, args));
    }
    [[nodiscard]] static db_expr call(std::string_view function, std::initializer_list<db_expr> args) {
        return call(function, std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr coalesce(std::span<const db_expr> args) {
        return db_expr(detail::db_owned_expression::make_coalesce(args));
    }
    [[nodiscard]] static db_expr coalesce(std::initializer_list<db_expr> args) {
        return coalesce(std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr null_if(const db_expr& lhs, const db_expr& rhs) {
        return db_expr(detail::db_owned_expression::make_null_if(lhs.owned_, rhs.owned_));
    }
    [[nodiscard]] static db_expr greatest(std::span<const db_expr> args) {
        return db_expr(detail::db_owned_expression::make_greatest(args));
    }
    [[nodiscard]] static db_expr greatest(std::initializer_list<db_expr> args) {
        return greatest(std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr least(std::span<const db_expr> args) {
        return db_expr(detail::db_owned_expression::make_least(args));
    }
    [[nodiscard]] static db_expr least(std::initializer_list<db_expr> args) {
        return least(std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static inline db_expr case_when(
        std::span<const db_case_branch> branches, const db_expr& otherwise = {});
    [[nodiscard]] static inline db_expr case_when(
        std::initializer_list<db_case_branch> branches, const db_expr& otherwise = {});

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
        return db_expr(detail::db_owned_expression::make_jsonb_set(
            target.owned_, path, new_value.owned_, create_if_missing));
    }
    [[nodiscard]] static db_expr jsonb_set(const db_expr& target, std::initializer_list<std::string_view> path,
        const db_expr& new_value, bool create_if_missing = true) {
        return jsonb_set(target, std::span<const std::string_view>(path.begin(), path.size()), new_value, create_if_missing);
    }
    [[nodiscard]] static db_expr jsonb_strip_nulls(const db_expr& target) {
        return call("jsonb_strip_nulls", std::span<const db_expr>(&target, 1));
    }

    [[nodiscard]] static inline db_expr aggregate(std::string_view function, std::span<const db_expr> args,
        bool distinct = false, std::span<const db_order_term> order = {});
    [[nodiscard]] static inline db_expr aggregate(std::string_view function, std::initializer_list<db_expr> args,
        bool distinct = false, std::initializer_list<db_order_term> order = {});
    [[nodiscard]] static db_expr count() {
        return db_expr(detail::db_owned_expression::make_count_star());
    }
    [[nodiscard]] static inline db_expr count(const db_expr& arg, bool distinct = false);
    [[nodiscard]] static inline db_expr sum(const db_expr& arg, bool distinct = false);
    [[nodiscard]] static inline db_expr min(const db_expr& arg);
    [[nodiscard]] static inline db_expr max(const db_expr& arg);
    [[nodiscard]] static inline db_expr avg(const db_expr& arg, bool distinct = false);
    [[nodiscard]] static inline db_expr bool_and(const db_expr& arg);
    [[nodiscard]] static inline db_expr bool_or(const db_expr& arg);
    [[nodiscard]] static inline db_expr jsonb_agg(const db_expr& arg, std::span<const db_order_term> order = {});
    [[nodiscard]] static inline db_expr jsonb_agg(const db_expr& arg, std::initializer_list<db_order_term> order);

    [[nodiscard]] bool empty() const noexcept {
        return owned_.empty();
    }
    [[nodiscard]] db_expression expression(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
        if (owned_.empty()) {
            throw std::invalid_argument("cannot evaluate an empty database expression");
        }
        return owned_.import_into(target, table, alias);
    }
    [[nodiscard]] inline db_selection as(borrowed_text alias_name) const;

    [[nodiscard]] db_expr cast(const db_type_definition& type) const {
        return db_expr(detail::db_owned_expression::make_cast(owned_, type));
    }
    [[nodiscard]] db_expr cast(db_data_type type) const {
        return cast(db_type_definition{.data_type_ = type});
    }
    [[nodiscard]] db_expr at_time_zone(const db_expr& zone) const {
        return db_expr(detail::db_owned_expression::make_at_time_zone(owned_, zone.owned_));
    }
    [[nodiscard]] db_expr at_time_zone(std::string_view zone) const {
        return at_time_zone(value(zone));
    }
    [[nodiscard]] db_expr extract(db_date_part part) const {
        return db_expr(detail::db_owned_expression::make_extract(part, owned_));
    }
    [[nodiscard]] db_expr subscript(const db_expr& index) const {
        return db_expr(detail::db_owned_expression::make_subscript(owned_, index.owned_));
    }
    [[nodiscard]] db_expr subscript(std::int64_t index) const {
        return subscript(value(index));
    }
    [[nodiscard]] db_expr operator[](std::int64_t index) const {
        return subscript(index);
    }
    [[nodiscard]] db_expr collate(std::string_view collation) const {
        return db_expr(detail::db_owned_expression::make_collate(owned_, collation));
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
        return db_expr(detail::db_owned_expression::make_filter(owned_, predicate.owned_));
    }
    [[nodiscard]] inline db_expr over(const db_window_options& window) const;
    [[nodiscard]] inline db_expr within_group(std::span<const db_order_term> order) const;
    [[nodiscard]] inline db_expr within_group(std::initializer_list<db_order_term> order) const;
    [[nodiscard]] inline db_order_term asc(db_nulls_order nulls = db_nulls_order::default_value) const;
    [[nodiscard]] inline db_order_term desc(db_nulls_order nulls = db_nulls_order::default_value) const;

    [[nodiscard]] db_expr operator-() const {
        return unary_expr(db_unary_operator::negate);
    }
    [[nodiscard]] db_expr operator~() const {
        return unary_expr(db_unary_operator::bit_not);
    }
    [[nodiscard]] db_predicate is_distinct_from(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::is_distinct_from, rhs);
    }
    [[nodiscard]] db_predicate is_not_distinct_from(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::is_not_distinct_from, rhs);
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
        return unary_predicate(db_unary_operator::is_null);
    }
    [[nodiscard]] db_predicate is_not_null() const {
        return unary_predicate(db_unary_operator::is_not_null);
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
        return db_predicate(detail::db_owned_expression::make_between(owned_,
            detail::make_immediate_db_parameter(std::forward<lower_type>(lower)),
            detail::make_immediate_db_parameter(std::forward<upper_type>(upper)), false));
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate not_between(lower_type&& lower, upper_type&& upper) const {
        return db_predicate(detail::db_owned_expression::make_between(owned_,
            detail::make_immediate_db_parameter(std::forward<lower_type>(lower)),
            detail::make_immediate_db_parameter(std::forward<upper_type>(upper)), true));
    }

    [[nodiscard]] const detail::db_owned_expression& owned() const& noexcept RUVIA_LIFETIMEBOUND {
        return owned_;
    }
    [[nodiscard]] detail::db_owned_expression&& owned() && noexcept {
        return std::move(owned_);
    }

private:
    template <typename, fixed_string>
    friend class db_field_reference;
    [[nodiscard]] db_expr binary(db_binary_operator op, const db_expr& rhs) const {
        return db_expr(detail::db_owned_expression::make_binary(owned_, op, rhs.owned_));
    }
    [[nodiscard]] db_expr unary_expr(db_unary_operator op) const {
        return db_expr(detail::db_owned_expression::make_unary(op, owned_));
    }
    [[nodiscard]] db_expr apply_json_path(db_binary_operator op, std::span<const std::string_view> path) const {
        return db_expr(detail::db_owned_expression::make_json_path_op(owned_, op, path));
    }
    [[nodiscard]] db_predicate compare_expr(db_binary_operator op, const db_expr& rhs) const {
        return db_predicate(detail::db_owned_expression::make_binary(owned_, op, rhs.owned_));
    }
    [[nodiscard]] db_predicate unary_predicate(db_unary_operator op) const {
        return db_predicate(detail::db_owned_expression::make_unary(op, owned_));
    }

    detail::db_owned_expression owned_;
};

[[nodiscard]] inline db_expr operator+(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::add, rhs.owned()));
}
[[nodiscard]] inline db_expr operator-(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::subtract, rhs.owned()));
}
[[nodiscard]] inline db_expr operator*(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::multiply, rhs.owned()));
}
[[nodiscard]] inline db_expr operator/(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::divide, rhs.owned()));
}
[[nodiscard]] inline db_expr operator%(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::modulo, rhs.owned()));
}
[[nodiscard]] inline db_expr operator&(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::bit_and, rhs.owned()));
}
[[nodiscard]] inline db_expr operator|(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::bit_or, rhs.owned()));
}
[[nodiscard]] inline db_expr operator^(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::bit_xor, rhs.owned()));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator+(const db_expr& lhs, v_type&& rhs) {
    return lhs + db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator-(const db_expr& lhs, v_type&& rhs) {
    return lhs - db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator*(const db_expr& lhs, v_type&& rhs) {
    return lhs * db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator/(const db_expr& lhs, v_type&& rhs) {
    return lhs / db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator%(const db_expr& lhs, v_type&& rhs) {
    return lhs % db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator&(const db_expr& lhs, v_type&& rhs) {
    return lhs & db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator|(const db_expr& lhs, v_type&& rhs) {
    return lhs | db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator^(const db_expr& lhs, v_type&& rhs) {
    return lhs ^ db_expr::value(std::forward<v_type>(rhs));
}
[[nodiscard]] inline db_predicate operator==(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::equal, rhs.owned()));
}
[[nodiscard]] inline db_predicate operator!=(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::not_equal, rhs.owned()));
}
[[nodiscard]] inline db_predicate operator<(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::less, rhs.owned()));
}
[[nodiscard]] inline db_predicate operator<=(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::less_equal, rhs.owned()));
}
[[nodiscard]] inline db_predicate operator>(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::greater, rhs.owned()));
}
[[nodiscard]] inline db_predicate operator>=(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::greater_equal, rhs.owned()));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator==(const db_expr& lhs, v_type&& rhs) {
    return lhs == db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator!=(const db_expr& lhs, v_type&& rhs) {
    return lhs != db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator<(const db_expr& lhs, v_type&& rhs) {
    return lhs < db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator<=(const db_expr& lhs, v_type&& rhs) {
    return lhs <= db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator>(const db_expr& lhs, v_type&& rhs) {
    return lhs > db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator>=(const db_expr& lhs, v_type&& rhs) {
    return lhs >= db_expr::value(std::forward<v_type>(rhs));
}

struct db_order_term final {
    db_expr expression_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};
// span 及元素中的文本均为借用；over 调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_window_options final {
    borrowed_span<db_expr> partition_by_{};
    borrowed_span<db_order_term> order_by_{};
    std::optional<db_window_frame_options> frame_{};
};
struct db_case_branch final {
    db_expr when_{};
    db_expr then_{};
};

struct db_selection final {
    borrowed_text column_{};
    db_expr expression_{};

    db_selection() = default;
    db_selection(const char* column)
        : column_(column) {}
    db_selection(std::string_view column)
        : column_(column) {}
    template <typename traits_type, typename allocator_type>
    db_selection(std::basic_string<char, traits_type, allocator_type>&&) = delete;
    template <typename traits_type, typename allocator_type>
    db_selection(const std::basic_string<char, traits_type, allocator_type>&&) = delete;
    db_selection(borrowed_text column, db_expr expression)
        : column_(column),
          expression_(std::move(expression)) {}
    // A real SQL wildcard, not a named field. Its qualifier is owned.
    [[nodiscard]] static db_selection star(std::string_view table = {}) {
        db_selection result;
        result.expression_ = db_expr::star(table);
        result.wildcard_ = true;
        return result;
    }
    [[nodiscard]] bool is_wildcard() const noexcept {
        return wildcard_;
    }
    [[nodiscard]] bool has_expression() const noexcept {
        return !expression_.empty();
    }
    [[nodiscard]] db_expression expression(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
        return expression_.empty() ? db_expression{} : expression_.expression(target, table, alias);
    }

private:
    bool wildcard_{false};
};

struct db_assignment final {
    borrowed_text column_{};
    db_expr value_{};

    [[nodiscard]] bool empty() const noexcept {
        return value_.empty();
    }
    [[nodiscard]] db_expression expression(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
        return value_.empty() ? db_expression{} : value_.expression(target, table, alias);
    }
};

// span 及元素中的文本均为借用；on_conflict 调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_conflict_options final {
    borrowed_span<borrowed_text> columns_{};
    borrowed_text constraint_{};
    db_expression target_where_{};
    borrowed_span<db_assignment> update_{};
    db_expression update_where_{};
    bool do_nothing_{false};
    // MariaDB matches any unique key. This explicit mode does not pretend to
    // implement PostgreSQL's conflict target semantics.
    bool any_unique_key_{false};
};

inline db_expr db_expr::case_when(std::span<const db_case_branch> branches, const db_expr& otherwise) {
    return db_expr(detail::db_owned_expression::make_case_when(branches, otherwise.owned_));
}
inline db_expr db_expr::case_when(std::initializer_list<db_case_branch> branches, const db_expr& otherwise) {
    return case_when(std::span<const db_case_branch>(branches.begin(), branches.size()), otherwise);
}
inline db_expr db_expr::aggregate(std::string_view function, std::span<const db_expr> args,
    bool distinct, std::span<const db_order_term> order) {
    return db_expr(detail::db_owned_expression::make_aggregate(function, args, distinct, order));
}
inline db_expr db_expr::aggregate(std::string_view function, std::initializer_list<db_expr> args,
    bool distinct, std::initializer_list<db_order_term> order) {
    return aggregate(function, std::span<const db_expr>(args.begin(), args.size()), distinct,
        std::span<const db_order_term>(order.begin(), order.size()));
}
inline db_expr db_expr::count(const db_expr& arg, bool distinct) {
    return aggregate("count", std::span<const db_expr>(&arg, 1), distinct);
}
inline db_expr db_expr::sum(const db_expr& arg, bool distinct) {
    return aggregate("sum", std::span<const db_expr>(&arg, 1), distinct);
}
inline db_expr db_expr::min(const db_expr& arg) {
    return aggregate("min", std::span<const db_expr>(&arg, 1));
}
inline db_expr db_expr::max(const db_expr& arg) {
    return aggregate("max", std::span<const db_expr>(&arg, 1));
}
inline db_expr db_expr::avg(const db_expr& arg, bool distinct) {
    return aggregate("avg", std::span<const db_expr>(&arg, 1), distinct);
}
inline db_expr db_expr::bool_and(const db_expr& arg) {
    return aggregate("bool_and", std::span<const db_expr>(&arg, 1));
}
inline db_expr db_expr::bool_or(const db_expr& arg) {
    return aggregate("bool_or", std::span<const db_expr>(&arg, 1));
}
inline db_expr db_expr::jsonb_agg(const db_expr& arg, std::span<const db_order_term> order) {
    return aggregate("jsonb_agg", std::span<const db_expr>(&arg, 1), false, order);
}
inline db_expr db_expr::jsonb_agg(const db_expr& arg, std::initializer_list<db_order_term> order) {
    return jsonb_agg(arg, std::span<const db_order_term>(order.begin(), order.size()));
}
inline db_selection db_expr::as(borrowed_text alias_name) const {
    return db_selection(alias_name, *this);
}
inline db_expr db_expr::over(const db_window_options& window) const {
    return db_expr(detail::db_owned_expression::make_over(owned_, window));
}
inline db_expr db_expr::within_group(std::span<const db_order_term> order) const {
    return db_expr(detail::db_owned_expression::make_within_group(owned_, order));
}
inline db_expr db_expr::within_group(std::initializer_list<db_order_term> order) const {
    return within_group(std::span<const db_order_term>(order.begin(), order.size()));
}
inline db_order_term db_expr::asc(db_nulls_order nulls) const {
    return db_order_term{*this, db_order_direction::asc, nulls};
}
inline db_order_term db_expr::desc(db_nulls_order nulls) const {
    return db_order_term{*this, db_order_direction::desc, nulls};
}

class db_statement final {
public:
    db_statement(const db_statement&) = delete;
    db_statement& operator=(const db_statement&) = delete;
    db_statement(db_statement&&) noexcept = default;
    db_statement& operator=(db_statement&& other);
    [[nodiscard]] std::string_view sql() const& noexcept RUVIA_LIFETIMEBOUND {
        return sql_;
    }
    std::string_view sql() const&& = delete;
    [[nodiscard]] std::span<const db_value> params() const& noexcept RUVIA_LIFETIMEBOUND {
        return params_;
    }
    std::span<const db_value> params() const&& = delete;
    [[nodiscard]] bool returns_rows() const noexcept {
        return returns_rows_;
    }

private:
    friend class db_query;
    friend class db_session;
    friend class db_transaction;
    friend struct detail::db_query_plan;
    template <typename, typename>
    friend class db_query_builder;
    db_statement(std::string sql, std::vector<db_value> params, bool rows)
        : sql_(std::move(sql)),
          params_(std::move(params)),
          returns_rows_(rows) {}
    std::string sql_;
    std::vector<db_value> params_;
    bool returns_rows_{false};
};

// A synchronous relational query AST and expression owner. Values, identifiers
// and nested queries become owned data at each construction call. Expression
// arguments must belong to this owner; import_expression() copies foreign nodes.
// Returned expression views borrow this owner's storage; its memory resource must
// outlive it. Repository and builder entry points copy expression nodes before
// returning, so their source owner can then be destroyed. Compilation is the sole
// place that emits SQL and assigns parameter positions.

class db_query final {
public:
    db_query();
    db_query(const db_query&) = delete;
    db_query& operator=(const db_query&) = delete;
    db_query(db_query&&) noexcept;
    db_query& operator=(db_query&&) noexcept;
    ~db_query();

    [[nodiscard]] db_query clone() const;
    [[nodiscard]] bool returns_rows() const;
    [[nodiscard]] bool has_where() const;
    // Classifies the current statement and its active CTE/set-operation graph,
    // never snapshots retained after a source replacement or failed append.
    [[nodiscard]] bool has_writes() const;
    [[nodiscard]] bool has_grouping() const;
    db_query& cache(const db_cache_setting_type& setting);
    db_query& cache(std::string_view id, std::optional<std::chrono::milliseconds> duration = {});

    // Trusted SQL syntax interleaved with expression arguments. Values belong in
    // value() arguments, never in the syntax strings. parts.size() == args.size()+1.
    db_expression sql(std::span<const std::string_view> parts, std::span<const db_expression> args);
    db_expression sql(std::initializer_list<std::string_view> parts, std::initializer_list<db_expression> args) {
        return sql(std::span<const std::string_view>(parts.begin(), parts.size()), std::span<const db_expression>(args.begin(), args.size()));
    }
    db_expression sql(std::string_view expression) {
        return sql(std::span<const std::string_view>(&expression, 1), {});
    }
    db_expression column(std::string_view name, std::string_view table = {});
    db_expression star(std::string_view table = {});
    // Owns the value in this query's resource before returning. Compiled for
    // PostgreSQL, a bound value is untyped wherever the SQL around it gives it a
    // type: the other operand, the target column, the selected function or
    // operator overload, a Boolean context (AND/OR/NOT, IS TRUE...), an
    // explicit cast(). Where PostgreSQL cannot type it ("any"/polymorphic
    // arguments such as json_build_object, concat or array_agg; arithmetic with
    // two bound operands; unary minus or ~; IS [NOT] NULL; GREATEST and friends
    // over bound values only) it carries its C++ category: signed integers
    // $n::int8, unsigned integers $n::numeric (the full uint64 range), doubles
    // ::float8, booleans ::bool, text ::text. Integers in int4-only built-in
    // arguments (left(), round(x, n), jsonb -> n) render as $n::int4.
    db_expression value(const db_value& value);
    template <detail::db_parameter value_type>
        requires(!std::same_as<std::remove_cvref_t<value_type>, db_value>)
    db_expression value(value_type&& value) {
        return this->value(detail::make_immediate_db_parameter(std::forward<value_type>(value)));
    }
    db_expression excluded(std::string_view column);
    db_expression null_value();
    db_expression default_value();
    db_expression call(std::string_view function, std::span<const db_expression> args = {}, std::span<const db_named_argument> named = {});
    db_expression call(std::string_view function, std::initializer_list<db_expression> args) {
        return call(function, std::span<const db_expression>(args.begin(), args.size()));
    }
    db_expression coalesce(std::span<const db_expression> args);
    db_expression coalesce(std::initializer_list<db_expression> args) {
        return coalesce(std::span<const db_expression>(args.begin(), args.size()));
    }
    db_expression null_if(db_expression value, db_expression other);
    db_expression greatest(std::span<const db_expression> args);
    db_expression greatest(std::initializer_list<db_expression> args) {
        return greatest(std::span<const db_expression>(args.begin(), args.size()));
    }
    db_expression least(std::span<const db_expression> args);
    db_expression least(std::initializer_list<db_expression> args) {
        return least(std::span<const db_expression>(args.begin(), args.size()));
    }
    db_expression binary(db_expression lhs, db_binary_operator op, db_expression rhs);
    db_expression unary(db_unary_operator op, db_expression value);
    db_expression between(db_expression value, db_expression lower, db_expression upper, bool negate = false);
    db_expression tuple(std::span<const db_expression> values);
    db_expression tuple(std::initializer_list<db_expression> values) {
        return tuple(std::span<const db_expression>(values.begin(), values.size()));
    }
    db_expression list(std::span<const db_expression> values);
    db_expression list(std::initializer_list<db_expression> values) {
        return list(std::span<const db_expression>(values.begin(), values.size()));
    }
    db_expression array(std::span<const db_expression> values);
    db_expression array(std::initializer_list<db_expression> values) {
        return array(std::span<const db_expression>(values.begin(), values.size()));
    }
    db_expression any(db_expression array);
    db_expression all(db_expression array);
    db_expression cast(db_expression value, const db_type_definition& type);
    db_expression cast(db_expression value, db_data_type type) {
        return cast(value, db_type_definition{.data_type_ = type});
    }
    db_expression alias(db_expression value, std::string_view name);
    db_expression case_when(std::span<const db_case_branch> branches, db_expression otherwise = {});
    db_expression case_when(std::initializer_list<db_case_branch> branches, db_expression otherwise = {}) {
        return case_when(std::span<const db_case_branch>(branches.begin(), branches.size()), otherwise);
    }
    // The const& composition entry points (exists, subquery, from, join,
    // insert_from, with, combine) copy their argument into this query. The
    // rvalue overloads take over an argument already allocated from this
    // query's resource and copy any other; either way the argument is left
    // moved-from unless it is this query itself.
    db_expression exists(const db_query& query);
    db_expression exists(db_query&& query);
    db_expression subquery(const db_query& query);
    db_expression subquery(db_query&& query);
    db_expression aggregate(std::string_view function, std::span<const db_expression> args, bool distinct = false, std::span<const db_order_term> order = {});
    db_expression aggregate(std::string_view function, std::initializer_list<db_expression> args, bool distinct = false, std::span<const db_order_term> order = {}) {
        return aggregate(function, std::span<const db_expression>(args.begin(), args.size()), distinct, order);
    }
    db_expression filter(db_expression aggregate, db_expression predicate);
    db_expression over(db_expression function, const db_window_options& window);
    db_expression within_group(db_expression function, std::span<const db_order_term> order);
    db_expression extract(db_date_part part, db_expression value);
    db_expression subscript(db_expression array, db_expression index);
    db_expression collate(db_expression value, std::string_view collation);
    db_expression at_time_zone(db_expression value, db_expression zone);
    db_expression at_time_zone(db_expression value, std::string_view zone) {
        return at_time_zone(value, this->value(zone));
    }
    // Copies the expression and its referenced subqueries into this query's resource;
    // the source must remain alive during import, but is unchanged and need not outlive the result.
    // Both qualifiers must be empty or valid nonempty names. A mapping rewrites exact
    // column/star qualifiers, including correlated references in nested queries, but not
    // references bound by a local alias or unaliased table name (qualified or unqualified).
    // Raw SQL text is not rewritten. Throws std::invalid_argument for an empty expression,
    // invalid mapping, or a mapped correlated reference captured by a local target binding.
    // Throws std::length_error if expression/query nesting exceeds 256 levels;
    // a chain of one AND or OR operator counts as a single level.
    db_expression import_expression(db_expression expression, std::string_view source_qualifier = {}, std::string_view target_qualifier = {});
    // Renders one expression with inline literals, never bound parameters.
    [[nodiscard]] static std::string render_expression(db_expression expression, db_driver driver);

    // Replacing SELECT, GROUP BY, DISTINCT ON or RETURNING lists preserves
    // the previous clause on validation or allocation failure.
    db_query& select(std::span<const db_expression> projections);
    db_query& select(std::initializer_list<db_expression> projections) {
        return select(std::span<const db_expression>(projections.begin(), projections.size()));
    }
    db_query& select(db_expression projection) {
        return select(std::span<const db_expression>(&projection, 1));
    }
    db_query& add_select(db_expression projection);
    // FROM, JOIN, UPDATE/DELETE/INSERT target aliases and CTE names are single
    // unqualified identifiers: a dotted name could never be referenced.
    db_query& from(std::string_view table, std::string_view alias = {});
    db_query& from(const db_query& query, std::string_view alias, const db_source_options& options = {});
    db_query& from(db_query&& query, std::string_view alias, const db_source_options& options = {});
    db_query& from_function(db_expression function, std::string_view alias, const db_source_options& options = {});
    db_query& clear_source();
    db_query& join(db_join_type type, std::string_view table, db_expression on = {}, std::string_view alias = {}, std::span<const std::string_view> using_columns = {});
    db_query& join(db_join_type type, std::string_view table_value, db_expression on, std::string_view alias, std::initializer_list<std::string_view> using_columns) {
        return join(type, table_value, on, alias, std::span<const std::string_view>(using_columns.begin(), using_columns.size()));
    }
    db_query& join(db_join_type type, const db_query& query, db_expression on, std::string_view alias, const db_source_options& options = {});
    db_query& join(db_join_type type, db_query&& query, db_expression on, std::string_view alias, const db_source_options& options = {});
    db_query& join_function(db_join_type type, db_expression function, db_expression on, std::string_view alias, const db_source_options& options = {});
    db_query& where(db_expression predicate);
    db_query& and_where(db_expression predicate);
    db_query& or_where(db_expression predicate);
    db_query& clear_where();
    db_query& group_by(std::span<const db_expression> expressions);
    db_query& group_by(std::initializer_list<db_expression> expressions) {
        return group_by(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    db_query& add_group_by(db_expression expression);
    db_query& having(db_expression predicate);
    db_query& and_having(db_expression predicate);
    db_query& or_having(db_expression predicate);
    db_query& clear_having();
    db_query& order_by(db_expression expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value);
    db_query& add_order_by(db_expression expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value);
    db_query& clear_order();
    db_query& limit(std::optional<query_row_count> count);
    template <std::integral integer_type>
        requires(!std::same_as<integer_type, bool>)
    db_query& limit(integer_type count) {
        if (!std::cmp_greater_equal(+count, 0)) {
            throw std::invalid_argument("database query limit must not be negative");
        }
        return set_limit_value(static_cast<std::uint64_t>(count));
    }
    db_query& offset(std::optional<query_row_count> count);
    template <std::integral integer_type>
        requires(!std::same_as<integer_type, bool>)
    db_query& offset(integer_type count) {
        if (!std::cmp_greater_equal(+count, 0)) {
            throw std::invalid_argument("database query offset must not be negative");
        }
        return set_offset_value(static_cast<std::uint64_t>(count));
    }
    db_query& distinct(bool enabled = true);
    db_query& distinct_on(std::span<const db_expression> expressions);
    db_query& distinct_on(std::initializer_list<db_expression> expressions) {
        return distinct_on(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    db_query& returning(std::span<const db_expression> expressions);
    db_query& returning(std::initializer_list<db_expression> expressions) {
        return returning(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    // Existing positional rows cannot be reinterpreted with a different column
    // list. Use insert_rows() to append named rows and grow the column union.
    // insert_into(), update() and delete_from() switch the statement kind only
    // while the query holds no clause beyond WHERE and WITH: a retained FROM
    // would otherwise widen the target (use update_from / delete_using).
    db_query& insert_into(std::string_view table, std::span<const std::string_view> columns = {}, std::string_view alias = {});
    db_query& insert_into(std::string_view table_value, std::initializer_list<std::string_view> columns, std::string_view alias = {}) {
        return insert_into(table_value, std::span<const std::string_view>(columns.begin(), columns.size()), alias);
    }
    db_query& values(std::span<const db_expression> row);
    db_query& values(std::initializer_list<db_expression> row) {
        return values(std::span<const db_expression>(row.begin(), row.size()));
    }
    // Appends a row-major batch to an INSERT, matching fields by column name.
    // Missing fields in old/new rows become DEFAULT. values.size() must equal
    // columns.size() * row_count and every expression must belong to this query.
    // One explicit default-only row is supported; multiple all-default rows
    // require at least one named column. A rejected batch preserves SQL semantics.
    db_query& insert_rows(std::span<const std::string_view> columns,
        std::span<const db_expression> values, std::size_t row_count);
    db_query& insert_from(const db_query& query);
    db_query& insert_from(db_query&& query);
    db_query& on_conflict(const db_conflict_options& conflict);
    db_query& update(std::string_view table, std::string_view alias = {});
    db_query& set(std::string_view column, db_expression value);
    db_query& set(const db_assignment& assignment);
    // A batch is published only after every assignment is prepared. Failure
    // preserves the previous assignments; duplicate columns use the last value.
    db_query& set(std::span<const db_assignment> assignments);
    db_query& set(std::initializer_list<db_assignment> assignments) {
        return set(std::span<const db_assignment>(assignments.begin(), assignments.size()));
    }
    db_query& update_from(std::string_view table, std::string_view alias = {});
    db_query& update_from(const db_query& query, std::string_view alias);
    db_query& delete_from(std::string_view table, std::string_view alias = {});
    db_query& delete_using(std::string_view table, std::string_view alias = {});
    db_query& delete_using(const db_query& query, std::string_view alias);
    // Lock target names are the unqualified FROM names or aliases they lock.
    db_query& lock(const db_lock_options& options);
    db_query& clear_lock();
    db_query& with(std::string_view name, const db_query& query, const db_cte_options& options = {});
    db_query& with(std::string_view name, db_query&& query, const db_cte_options& options = {});
    db_query& combine(db_set_operation operation, const db_query& query);
    db_query& combine(db_set_operation operation, db_query&& query);

    // PostgreSQL bound mode numbers each parameter node once and rejects a
    // statement needing more than 65535 parameters (the protocol's Int16
    // count) with std::length_error. In bound mode, raw SQL text must not
    // contain PostgreSQL positional parameters ($1); pass value() arguments.
    [[nodiscard]] db_statement compile(db_driver driver,
        db_parameter_mode mode = db_parameter_mode::bound) const;

private:
    friend class detail::db_owned_expression;
    friend class detail::db_query_compiler;
    friend class detail::db_relation_plan;
    friend struct detail::qualified_expression_importer;
    friend class db_schema;
#ifdef RUVIA_ENABLE_REDIS
    // The result cache reads each statement's cache options.
    friend class detail::db_query_cache_state;
    friend struct detail::db_query_plan;
#endif
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;
    template <typename, typename>
    friend class db_repository;

    [[nodiscard]] bool cacheable() const;
    [[nodiscard]] std::optional<bool> cache_enabled() const;
    [[nodiscard]] std::optional<std::chrono::milliseconds> cache_duration() const;
    [[nodiscard]] std::string_view cache_id() const;
    [[nodiscard]] detail::db_cache_shape cache_shape() const;
    void copy_cache(const db_query& source, detail::db_cache_shape shape);
    void with_columns(std::string_view name, const db_query& query, db_materialization materialization,
        std::span<const std::string> columns);

    struct storage_deleter_type final {
        void operator()(detail::db_query_storage* storage) const noexcept;
    };
    using storage_owner_type = std::unique_ptr<detail::db_query_storage, storage_deleter_type>;
    explicit db_query(storage_owner_type storage) noexcept;
    [[nodiscard]] static storage_owner_type copy_storage(const detail::db_query_storage& source);
    [[nodiscard]] detail::db_query_storage& storage();
    [[nodiscard]] const detail::db_query_storage& storage() const;
    [[nodiscard]] std::size_t require_expression(db_expression expression) const;
    void require_select_query() const;
    db_query& replace_expressions(std::vector<std::size_t>& target, std::span<const db_expression> expressions);
    db_query& combine_clause_predicate(std::size_t& clause, db_binary_operator op, db_expression predicate);
    // Every expression builder constructs exactly its own node kind and
    // appends it here; the returned handle addresses the appended node.
    [[nodiscard]] detail::db_query_node make_node(detail::db_node_kind kind, std::span<const db_expression> args = {}) const;
    db_expression push_node(detail::db_query_node&& node);
    // Callers prepare clause metadata and reserve every publication slot first.
    [[nodiscard]] db_query adopt(db_query&& query) const;
    [[nodiscard]] db_expression add_subquery(detail::db_node_kind kind, db_query&& query);
    db_query& set_limit_value(std::uint64_t count);
    db_query& set_offset_value(std::uint64_t count);
    storage_owner_type storage_;
};

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
    friend class detail::db_owned_expression;

    [[nodiscard]] db_predicate compare(db_binary_operator op, const db_tuple& rhs) const {
        return db_predicate(detail::db_owned_expression::make_binary(owned_, op, rhs.owned_));
    }
    [[nodiscard]] db_predicate compare_list(db_binary_operator op, std::span<const db_tuple> rows) const {
        return db_predicate(detail::db_owned_expression::make_in_expr_list(owned_, op, rows));
    }
    detail::db_owned_expression owned_{};
};

}  // namespace ruvia

namespace ruvia {

namespace detail {
template <typename e_type>
struct db_entity_access;
}  // namespace detail

enum class db_relation_kind : unsigned char { one_to_one,
    many_to_one,
    one_to_many,
    many_to_many };

template <fixed_string local_name, fixed_string referenced_name>
struct db_join_column final {
    static constexpr auto local = local_name;
    static constexpr auto referenced = referenced_name;
};

template <typename... j_type>
struct db_join_columns final {
    using tuple_type = std::tuple<j_type...>;
};
template <fixed_string field_name>
struct db_inverse final {
    static constexpr auto name = field_name;
};
namespace detail {
template <typename t_type>
struct is_db_inverse : std::false_type {};
template <fixed_string n>
struct is_db_inverse<db_inverse<n>> : std::true_type {};
template <typename t_type>
struct db_inverse_name {
    static constexpr auto value = fixed_string{""};
};
template <fixed_string n>
struct db_inverse_name<db_inverse<n>> {
    static constexpr auto value = n;
};
template <typename... t_type>
struct relation_join_tuple {
    using type = std::tuple<t_type...>;
};
template <typename t_type>
    requires requires { typename t_type::tuple_type; }
struct relation_join_tuple<t_type> {
    using type = typename t_type::tuple_type;
};
template <typename... t_type>
struct first_inverse_name {
    static constexpr auto value = fixed_string{""};
};
template <typename t_type, typename... rest_type>
struct first_inverse_name<t_type, rest_type...> {
    static constexpr auto value = [] {
        if constexpr (is_db_inverse<t_type>::value) {
            return db_inverse_name<t_type>::value;
        } else {
            return first_inverse_name<rest_type...>::value;
        }
    }();
};
}  // namespace detail
template <fixed_string table, typename owner, typename inverse_type>
struct db_join_table final {
    static constexpr auto name = table;
    using owner_columns_type = typename owner::tuple_type;
    using inverse_columns_type = typename inverse_type::tuple_type;
};
namespace detail {
template <typename t_type>
struct is_db_join_table : std::false_type {};
template <fixed_string n, typename o_type, typename i_type>
struct is_db_join_table<db_join_table<n, o_type, i_type>> : std::true_type {};
}  // namespace detail

template <fixed_string field_name, typename target_type, typename... js_type>
struct db_many_to_one final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    using join_columns_type = std::tuple<js_type...>;
    static constexpr auto kind = db_relation_kind::many_to_one;
    static constexpr bool is_collection = false;
    static constexpr bool is_owning = true;
    static constexpr auto inverse_name = fixed_string{""};
};
template <fixed_string field_name, typename target_type, typename... mapping_type>
struct db_one_to_one final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    static constexpr auto kind = db_relation_kind::one_to_one;
    static constexpr bool is_collection = false;
    static constexpr bool is_owning = !(sizeof...(mapping_type) == 1 && (detail::is_db_inverse<mapping_type>::value && ...));
    using join_columns_type = typename detail::relation_join_tuple<mapping_type...>::type;
    static constexpr auto inverse_name = detail::first_inverse_name<mapping_type...>::value;
};
template <fixed_string field_name, typename target_type, fixed_string inverse>
struct db_one_to_many final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    static constexpr auto kind = db_relation_kind::one_to_many;
    static constexpr bool is_collection = true;
    static constexpr bool is_owning = false;
    static constexpr auto inverse_name = inverse;
    using join_columns_type = std::tuple<>;
};
template <fixed_string field_name, typename target_type, typename mapping_type>
struct db_many_to_many final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    using join_table_type = mapping_type;
    static constexpr auto kind = db_relation_kind::many_to_many;
    static constexpr bool is_collection = true;
    static constexpr bool is_owning = detail::is_db_join_table<mapping_type>::value;
    static constexpr auto inverse_name = [] {
        if constexpr (detail::is_db_inverse<mapping_type>::value) {
            return mapping_type::name;
        } else {
            return fixed_string{""};
        }
    }();
    using join_columns_type = std::tuple<>;
};

enum class db_generated_type : unsigned char { none,
    stored,
    virtual_value };

template <typename enum_name_type = fixed_string<1>, typename default_expression_type = fixed_string<1>>
struct db_column_options final {
    db_data_type data_type_{db_data_type::inferred};
    bool primary_key_{false};
    bool generated_{false};
    db_generated_type generated_type_{db_generated_type::none};
    bool nullable_{false};
    bool read_only_{false};
    bool create_date_{false};
    bool update_date_{false};
    bool delete_date_{false};
    bool version_{false};
    std::size_t length_{0};
    unsigned precision_{0};
    unsigned scale_{0};
    enum_name_type enum_name_{""};
    default_expression_type default_expression_{""};
    constexpr bool operator==(const db_column_options&) const = default;
};

namespace detail {

template <typename t_type>
struct db_entity_type_traits {
    using value_type = t_type;
    static constexpr db_data_type data_type = db_data_type::inferred;
};
template <>
struct db_entity_type_traits<bool> {
    using value_type = bool;
    static constexpr db_data_type data_type = db_data_type::boolean;
};
template <>
struct db_entity_type_traits<std::int16_t> {
    using value_type = std::int16_t;
    static constexpr db_data_type data_type = db_data_type::small_int;
};
template <>
struct db_entity_type_traits<std::int32_t> {
    using value_type = std::int32_t;
    static constexpr db_data_type data_type = db_data_type::integer;
};
template <>
struct db_entity_type_traits<std::int64_t> {
    using value_type = std::int64_t;
    static constexpr db_data_type data_type = db_data_type::big_int;
};
template <>
struct db_entity_type_traits<float> {
    using value_type = float;
    static constexpr db_data_type data_type = db_data_type::real;
};
template <>
struct db_entity_type_traits<double> {
    using value_type = double;
    static constexpr db_data_type data_type = db_data_type::double_value;
};
template <>
struct db_entity_type_traits<std::string> {
    using value_type = std::string;
    static constexpr db_data_type data_type = db_data_type::text;
};
template <>
struct db_entity_type_traits<string> {
    using value_type = string;
    static constexpr db_data_type data_type = db_data_type::text;
};
template <typename t_type>
struct db_entity_type_traits<std::optional<t_type>> {
    using value_type = typename db_entity_type_traits<t_type>::value_type;
    static constexpr db_data_type data_type = db_entity_type_traits<t_type>::data_type;
};
template <typename t_type>
struct db_entity_type_traits<std::vector<t_type>> {
    using value_type = std::vector<t_type>;
    static constexpr db_data_type data_type = db_data_type::array;
};

// Column descriptor predicates shared by typed field references and the
// repository builders so both agree on which columns accept writes and
// arithmetic updates.
template <typename c_type>
consteval bool is_writable_db_column() noexcept {
    return c_type::options.generated_type_ == db_generated_type::none && !c_type::options.read_only_;
}
// Columns a conflict update writes from EXCLUDED when the caller names none.
template <typename c_type>
consteval bool is_conflict_updatable_db_column() noexcept {
    return !c_type::options.primary_key_ && !c_type::options.generated_ && is_writable_db_column<c_type>();
}
template <typename c_type>
consteval bool is_numeric_db_column() noexcept {
    return !is_std_vector<typename c_type::value_type>::value &&
           (c_type::data_type == db_data_type::small_int ||
               c_type::data_type == db_data_type::integer ||
               c_type::data_type == db_data_type::big_int ||
               c_type::data_type == db_data_type::numeric ||
               c_type::data_type == db_data_type::real ||
               c_type::data_type == db_data_type::double_value);
}
consteval bool is_integer_db_data_type(db_data_type type) noexcept {
    return type == db_data_type::small_int || type == db_data_type::integer || type == db_data_type::big_int;
}

// Every rule here depends only on the descriptor, so a malformed column is
// rejected where it is declared instead of when a schema compiles it; the
// repository relies on these invariants (for example, the soft-delete filter
// requires a writable delete_date_ column).
template <typename t_type>
consteval bool valid_db_column_options(const auto& options) noexcept {
    const auto data_type = options.data_type_ == db_data_type::inferred ? db_entity_type_traits<t_type>::data_type : options.data_type_;
    const bool integer = !is_std_vector<t_type>::value && is_integer_db_data_type(data_type);
    const bool computed = options.generated_type_ != db_generated_type::none;
    const bool writable = !computed && !options.read_only_;
    const int lifecycle_flags = int{options.create_date_} + int{options.update_date_} + int{options.delete_date_} + int{options.version_};
    // create_date_ only selects a schema default; the repository writes the other three.
    const bool repository_stamped = options.update_date_ || options.delete_date_ || options.version_;
    return !(options.generated_ && computed) &&
           !(options.read_only_ && (options.generated_ || computed)) &&
           (options.enum_name_.view().empty() || options.data_type_ == db_data_type::inferred || options.data_type_ == db_data_type::array) &&
           (!options.generated_ || (integer && !options.nullable_)) &&
           (!options.version_ || (integer && !options.nullable_)) &&
           (!options.primary_key_ || (!options.nullable_ && !options.delete_date_)) &&
           lifecycle_flags <= 1 &&
           (!repository_stamped || writable);
}

// Table names may be schema-qualified; generated aliases and constraint names
// use the unqualified table name.
constexpr std::string_view unqualified_db_name(std::string_view name) noexcept {
    const auto dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1);
}

template <typename t_type, bool collection = false>
struct db_relation_slot final {
    enum class state_type : unsigned char { unset,
        null,
        value };
    using stored_type = std::conditional_t<collection, entity_rows<t_type>, t_type>;
    using pointer = std::unique_ptr<stored_type>;
    db_relation_slot() noexcept = default;
    db_relation_slot(const db_relation_slot&) = delete;
    db_relation_slot& operator=(const db_relation_slot&) = delete;
    db_relation_slot(db_relation_slot&& other) noexcept
        : state_(std::exchange(other.state_, state_type::unset)),
          value_(std::move(other.value_)) {}
    db_relation_slot& operator=(db_relation_slot&&) = delete;
    state_type state_{state_type::unset};
    pointer value_;
    void reset() noexcept {
        value_.reset();
        state_ = state_type::unset;
    }
};

template <template <typename> class predicate_type, typename... ts_type>
struct tuple_filter;
template <template <typename> class predicate_type>
struct tuple_filter<predicate_type> {
    using type = std::tuple<>;
};
template <template <typename> class predicate_type, typename t_type, typename... ts_type>
struct tuple_filter<predicate_type, t_type, ts_type...> {
    using tail = typename tuple_filter<predicate_type, ts_type...>::type;
    using type = std::conditional_t<predicate_type<t_type>::value, decltype(std::tuple_cat(std::declval<std::tuple<t_type>>(), std::declval<tail>())), tail>;
};
template <template <typename> class predicate_type, typename... ts_type>
using tuple_filter_t = typename tuple_filter<predicate_type, ts_type...>::type;

}  // namespace detail

// Columns and projections require db_mapped_value: all text is owned.
// PostgreSQL arrays use std::vector of owning scalar or optional elements.
// Decoding retains one-dimensional element order and discards the lower bound;
// multidimensional arrays are rejected with db_conversion_error.
// Option combinations that cannot hold (see valid_db_column_options) do not
// name a column type, so the declaration fails to compile.
template <fixed_string field_name, db_mapped_value t_type, db_column_options options_type = db_column_options{}>
    requires(detail::valid_db_column_options<t_type>(options_type))
struct db_column final {
    static_assert(!detail::is_optional<t_type>::value, "use nullable column options and set_null for entity fields; optional is only for array elements");
    static constexpr auto name = field_name;
    using value_type = t_type;
    static consteval auto normalize_options(auto opts) noexcept {
        if (opts.delete_date_) {
            opts.nullable_ = true;
        }
        return opts;
    }
    static constexpr auto options = normalize_options(options_type);
    static constexpr db_data_type data_type = options_type.data_type_ == db_data_type::inferred
                                                  ? detail::db_entity_type_traits<t_type>::data_type
                                                  : options_type.data_type_;
};

template <typename t_type>
struct is_db_column : std::false_type {};
template <fixed_string name, typename t_type, db_column_options options_type>
struct is_db_column<db_column<name, t_type, options_type>> : std::true_type {};
template <typename t_type>
struct is_db_relation : std::false_type {};
template <fixed_string name, typename target_type, typename... joins_type>
struct is_db_relation<db_many_to_one<name, target_type, joins_type...>> : std::true_type {};
template <fixed_string name, typename target_type, typename... mapping_type>
struct is_db_relation<db_one_to_one<name, target_type, mapping_type...>> : std::true_type {};
template <fixed_string name, typename target_type, fixed_string inverse>
struct is_db_relation<db_one_to_many<name, target_type, inverse>> : std::true_type {};
template <fixed_string name, typename target_type, typename mapping_type>
struct is_db_relation<db_many_to_many<name, target_type, mapping_type>> : std::true_type {};

namespace detail {

template <fixed_string table, typename... members_type>
class db_entity_storage final {
    static_assert(((is_db_column<members_type>::value || is_db_relation<members_type>::value) && ...),
        "SQL entities require RUVIA_DB_COLUMN or SQL relation descriptors");
    using columns_tuple_type = tuple_filter_t<is_db_column, members_type...>;
    using relations_tuple_type = tuple_filter_t<is_db_relation, members_type...>;
    static_assert(unique_entity_columns<members_type...>(), "duplicate database entity member name");
    static consteval std::size_t count_lifecycle_columns(auto predicate) noexcept {
        return [&]<typename... cols>(std::tuple<cols...>*) {
            return (std::size_t{predicate(cols::options)} + ... + std::size_t{0});
        }(static_cast<columns_tuple_type*>(nullptr));
    }
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.create_date_; }) <= 1,
        "database entity may declare at most one create_date_ column");
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.update_date_; }) <= 1,
        "database entity may declare at most one update_date_ column");
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.delete_date_; }) <= 1,
        "database entity may declare at most one delete_date_ column");
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.version_; }) <= 1,
        "database entity may declare at most one version_ column");
    template <typename r_type>
    using relation_slot_type = db_relation_slot<typename r_type::target_entity_type, r_type::is_collection>;
    template <std::size_t... indexes>
    static auto make_relation_slots(std::index_sequence<indexes...>) {
        return std::tuple<relation_slot_type<std::tuple_element_t<indexes, relations_tuple_type>>...>{};
    }
    using column_storage = typename entity_storage_from_tuple<columns_tuple_type>::type;
    using relation_slots_type = decltype(make_relation_slots(std::make_index_sequence<std::tuple_size_v<relations_tuple_type>>{}));
    template <fixed_string name>
    static consteval std::size_t relation_index() {
        constexpr auto result_value = descriptor_index<relations_tuple_type, name>();
        static_assert(result_value < std::tuple_size_v<relations_tuple_type>, "unknown database entity relation");
        return result_value;
    }
    // Member names are unique across columns and relations.
    template <fixed_string name>
    static consteval bool is_relation() {
        return descriptor_index<relations_tuple_type, name>() < std::tuple_size_v<relations_tuple_type>;
    }
    template <fixed_string name>
    auto& relation_slot() {
        return std::get<relation_index<name>()>(relation_slots_);
    }
    template <fixed_string name>
    const auto& relation_slot() const {
        return std::get<relation_index<name>()>(relation_slots_);
    }

public:
    using columns_type = columns_tuple_type;
    using relations_type = relations_tuple_type;
    static constexpr std::string_view table_name() noexcept {
        return table.view();
    }
    template <fixed_string name>
    static consteval std::size_t column_index() {
        return column_storage::template column_index<name>();
    }
    template <fixed_string name>
    static consteval std::string_view column_name() noexcept {
        return column_storage::template column_name<name>();
    }
    db_entity_storage() = default;
    db_entity_storage(const db_entity_storage&) = delete;
    db_entity_storage& operator=(const db_entity_storage&) = delete;
    db_entity_storage(db_entity_storage&&) noexcept = default;
    template <fixed_string name>
    auto& get() & RUVIA_LIFETIMEBOUND {
        if constexpr (is_relation<name>()) {
            auto& s = relation_slot<name>();
            if (s.state_ != decltype(s.state_)::value) {
                throw std::logic_error("database entity relation is not loaded");
            }
            return *s.value_;
        } else {
            return columns_.template get<name>();
        }
    }
    template <fixed_string name>
    auto& get() && = delete;
    template <fixed_string name>
    const auto& get() const& RUVIA_LIFETIMEBOUND {
        return const_cast<db_entity_storage*>(this)->get<name>();
    }
    template <fixed_string name>
    const auto& get() const&& = delete;
    template <fixed_string name, typename v_type>
    void set(v_type&& value) {
        static_assert(!is_relation<name>(), "relations cannot be assigned; use relation loading access");
        columns_.template set<name>(std::forward<v_type>(value));
    }
    template <fixed_string name>
    void set_null()
        requires(!is_relation<name>() && std::tuple_element_t<column_index<name>(), columns_type>::options.nullable_)
    {
        columns_.template set_null<name>();
    }
    template <fixed_string name>
    void reset() {
        if constexpr (is_relation<name>()) {
            relation_slot<name>().reset();
        } else {
            columns_.template reset<name>();
        }
    }
    template <fixed_string name>
    bool is_set() const {
        if constexpr (is_relation<name>()) {
            return relation_slot<name>().state_ != decltype(relation_slot<name>().state_)::unset;
        } else {
            return columns_.template is_set<name>();
        }
    }
    template <fixed_string name>
    bool is_null() const {
        if constexpr (is_relation<name>()) {
            return relation_slot<name>().state_ == decltype(relation_slot<name>().state_)::null;
        } else {
            return columns_.template is_null<name>();
        }
    }

private:
    template <typename>
    friend struct db_entity_access;
    column_storage columns_;
    relation_slots_type relation_slots_;
};

template <typename e_type>
struct db_entity_access final {
    static auto& storage(e_type& entity) noexcept {
        return entity.entity_;
    }
    template <fixed_string name>
    static auto& emplace_relation(e_type& entity) {
        auto& slot = relation_slot<name, false>(entity);
        using stored_type = typename std::remove_reference_t<decltype(slot)>::stored_type;
        slot.value_ = std::make_unique<stored_type>();
        slot.state_ = decltype(slot.state_)::value;
        return *slot.value_;
    }

    template <fixed_string name>
    static auto& ensure_relation_collection(e_type& entity) {
        auto& slot = relation_slot<name, true>(entity);
        if (slot.state_ != decltype(slot.state_)::value) {
            using stored_type = typename std::remove_reference_t<decltype(slot)>::stored_type;
            slot.value_ = std::make_unique<stored_type>();
            slot.state_ = decltype(slot.state_)::value;
        }
        return *slot.value_;
    }

    template <fixed_string name>
    static void set_relation_null(e_type& entity) {
        auto& slot = relation_slot<name, false>(entity);
        slot.reset();
        slot.state_ = decltype(slot.state_)::null;
    }

private:
    template <fixed_string name, bool expected_collection>
    static auto& relation_slot(e_type& entity) noexcept {
        auto& owner_value = storage(entity);
        using owner_type = std::remove_reference_t<decltype(owner_value)>;
        static_assert(owner_type::template is_relation<name>());
        using relation_type = std::tuple_element_t<owner_type::template relation_index<name>(), typename e_type::relations_type>;
        if constexpr (expected_collection) {
            static_assert(relation_type::is_collection, "ensure_relation_collection requires a collection relation");
        } else {
            static_assert(!relation_type::is_collection, "to-one relation operation is not valid on a collection relation");
        }
        return owner_value.template relation_slot<name>();
    }
};

template <typename columns>
inline constexpr bool sql_columns = []<std::size_t... indices>(std::index_sequence<indices...>) {
    return (is_db_column<std::tuple_element_t<indices, columns>>::value && ...);
}(std::make_index_sequence<std::tuple_size_v<columns>>{});

}  // namespace detail

template <typename entity>
concept sql_entity = requires(const entity& value) {
    typename entity::columns_type;
    typename entity::relations_type;
    typename entity::sql_entity_type;
    requires std::same_as<entity, typename entity::sql_entity_type>;
    requires detail::sql_columns<typename entity::columns_type>;
    { entity::table_name() } -> std::convertible_to<std::string_view>;
};

template <typename projection>
concept sql_projection = requires(const projection& value) {
    typename projection::columns_type;
    typename projection::db_projection_type;
    requires std::same_as<projection, typename projection::db_projection_type>;
    requires detail::sql_columns<typename projection::columns_type>;
};

template <typename e_type, fixed_string field_name>
class db_field_reference final {
    static_assert(sql_entity<e_type>, "SQL field references require a SQL entity");
    static_assert(e_type::template column_index<field_name>() < std::tuple_size_v<typename e_type::columns_type>);
    using column_type = std::tuple_element_t<e_type::template column_index<field_name>(), typename e_type::columns_type>;
    static constexpr bool is_writable = detail::is_writable_db_column<column_type>();
    static constexpr bool is_numeric = detail::is_numeric_db_column<column_type>();

public:
    constexpr db_field_reference(borrowed_text alias = {}) noexcept
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
    [[nodiscard]] constexpr db_field_reference with_alias(borrowed_text alias) const noexcept {
        return db_field_reference(alias);
    }
    [[nodiscard]] db_expression expression(db_query& query, std::string_view fallback_alias = {}) const {
        const auto qualifier = !alias_.empty() ? alias_.view() : (fallback_alias.empty() ? e_type::table_name() : fallback_alias);
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
        return db_expr(detail::db_owned_expression::make_static_excluded(field_name.view()));
    }
    [[nodiscard]] db_order_term asc(db_nulls_order nulls = db_nulls_order::default_value) const {
        return to_expr().asc(nulls);
    }
    [[nodiscard]] db_order_term desc(db_nulls_order nulls = db_nulls_order::default_value) const {
        return to_expr().desc(nulls);
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
            { s.query() } -> std::same_as<const db_query&>;
        }
    [[nodiscard]] db_predicate in(const subquery_type& subquery) const {
        return compare_subquery(db_binary_operator::in, subquery.query());
    }
    template <typename subquery_type>
        requires requires(const subquery_type& s) {
            { s.query() } -> std::same_as<const db_query&>;
        }
    [[nodiscard]] db_predicate not_in(const subquery_type& subquery) const {
        return compare_subquery(db_binary_operator::not_in, subquery.query());
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
    [[nodiscard]] db_selection as(borrowed_text alias_name) const {
        return to_expr().as(alias_name);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_selection coalesce(v_type&& fallback, borrowed_text alias_name = field_name.view()) const {
        return db_expr::coalesce({to_expr(), db_expr::value(std::forward<v_type>(fallback))}).as(alias_name);
    }
    [[nodiscard]] db_selection coalesce_text(std::string_view fallback = {}, borrowed_text alias_name = field_name.view()) const {
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
        return db_assignment{field_name.view(), db_expr::value(std::forward<v_type>(value))};
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
        return db_assignment{field_name.view(), value};
    }
    [[nodiscard]] db_assignment operator=(const db_expr& value) const
        requires is_writable
    {
        return set(value);
    }
    [[nodiscard]] db_assignment set_null() const
        requires(is_writable && column_type::options.nullable_)
    {
        return db_assignment{field_name.view(), db_expr::null()};
    }
    [[nodiscard]] db_assignment set_null_if_empty(std::string_view value) const
        requires(is_writable && column_type::options.nullable_)
    {
        auto expr = db_expr::null_if(db_expr::value(value), db_expr::value(std::string_view{}));
        if constexpr (!column_type::options.enum_name_.view().empty()) {
            expr = expr.cast({.custom_name_ = column_type::options.enum_name_.view()});
        } else if constexpr (column_type::data_type != db_data_type::inferred &&
                             column_type::data_type != db_data_type::text &&
                             column_type::data_type != db_data_type::varchar &&
                             column_type::data_type != db_data_type::char_value) {
            expr = expr.cast({.data_type_ = column_type::data_type,
                .length_ = column_type::options.length_,
                .precision_ = column_type::options.precision_,
                .scale_ = column_type::options.scale_});
        }
        return db_assignment{field_name.view(), std::move(expr)};
    }
    [[nodiscard]] db_assignment set_default() const
        requires is_writable
    {
        return db_assignment{field_name.view(), db_expr::default_value()};
    }
    [[nodiscard]] db_assignment set_now() const
        requires is_writable
    {
        return db_assignment{field_name.view(), db_expr::now()};
    }
    [[nodiscard]] db_assignment set_excluded() const
        requires is_writable
    {
        return db_assignment{field_name.view(), excluded_expr()};
    }
    template <typename number_type = std::int64_t>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment increment(number_type value = number_type{1}) const {
        return db_assignment{field_name.view(), to_expr() + value};
    }
    template <typename number_type>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment operator+=(number_type value) const {
        return increment(value);
    }
    template <typename number_type = std::int64_t>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment decrement(number_type value = number_type{1}) const {
        return db_assignment{field_name.view(), to_expr() - value};
    }
    template <typename number_type>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment operator-=(number_type value) const {
        return decrement(value);
    }

private:
    template <typename t_type>
    [[nodiscard]] db_predicate compare_list(db_binary_operator op, std::span<const t_type> values) const {
        return db_predicate(detail::db_owned_expression::make_in_list(to_expr().owned(), op, values));
    }
    [[nodiscard]] db_predicate compare_subquery(db_binary_operator op, const db_query& subquery) const {
        return db_predicate(detail::db_owned_expression::make_in_subquery(to_expr().owned(), op, subquery));
    }
    [[nodiscard]] db_predicate compare_json(db_binary_operator op, std::string_view json_text) const {
        return to_expr().compare_expr(op, db_expr::value(json_text).cast(db_data_type::jsonb));
    }
    [[nodiscard]] db_predicate compare_json_keys(db_binary_operator op, std::span<const std::string_view> keys) const {
        return db_predicate(detail::db_owned_expression::make_json_path_op(to_expr().owned(), op, keys));
    }
    template <typename t_type>
    db_predicate compare_array(db_binary_operator op, std::span<const t_type> values) const {
        static_assert(detail::is_std_vector<typename column_type::value_type>::value, "array predicates require an array column");
        using item_type = typename detail::is_std_vector<typename column_type::value_type>::value_type;
        constexpr auto type = column_type::options.data_type_ == db_data_type::inferred || column_type::options.data_type_ == db_data_type::array
                                  ? detail::db_entity_type_traits<item_type>::data_type
                                  : column_type::options.data_type_;
        const db_type_definition array_type{
            .data_type_ = type,
            .length_ = column_type::options.length_,
            .precision_ = column_type::options.precision_,
            .scale_ = column_type::options.scale_,
            .array_ = true,
        };
        return db_predicate(detail::db_owned_expression::make_array_op(to_expr().owned(), op, values, array_type));
    }

    borrowed_text alias_{};
};

#define RUVIA_DB_COLUMN(name, value_type, ...) ::ruvia::db_column<::ruvia::fixed_string{#name}, value_type __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_DB_JOIN_COLUMN(local, referenced) ::ruvia::db_join_column<::ruvia::fixed_string{#local}, ::ruvia::fixed_string{#referenced}>
#define RUVIA_DB_JOIN_COLUMNS(...) ::ruvia::db_join_columns<__VA_ARGS__>
#define RUVIA_DB_INVERSE(name) ::ruvia::db_inverse<::ruvia::fixed_string{#name}>
#define RUVIA_DB_JOIN_TABLE(table, owner_columns_type, inverse_columns_type) ::ruvia::db_join_table<::ruvia::fixed_string{table}, owner_columns_type, inverse_columns_type>
#define RUVIA_DB_MANY_TO_ONE(name, target, ...) ::ruvia::db_many_to_one<::ruvia::fixed_string{#name}, target, __VA_ARGS__>
#define RUVIA_DB_ONE_TO_ONE(name, target, ...) ::ruvia::db_one_to_one<::ruvia::fixed_string{#name}, target, __VA_ARGS__>
#define RUVIA_DB_ONE_TO_MANY(name, target, inverse) ::ruvia::db_one_to_many<::ruvia::fixed_string{#name}, target, ::ruvia::fixed_string{#inverse}>
#define RUVIA_DB_MANY_TO_MANY(name, target, mapping_type) ::ruvia::db_many_to_many<::ruvia::fixed_string{#name}, target, mapping_type>

// set<field>() 检查整数范围与有限浮点溢出，数组及 optional 元素递归使用相同规则。
// 整数列不隐式接受浮点输入；调用方须显式选择转换策略。失败保留原值和 unset/null 状态。
// 此保证不拦截调用方通过可变 get() 或显式转换自行执行的数值赋值。
#define RUVIA_DB_ENTITY(name, table, ...)                                                                                   \
    struct name final {                                                                                                     \
    private:                                                                                                                \
        using storage_type = ::ruvia::detail::db_entity_storage<::ruvia::fixed_string{table}, __VA_ARGS__>;                 \
        storage_type entity_;                                                                                               \
        template <typename>                                                                                                 \
        friend struct ::ruvia::detail::db_entity_access;                                                                    \
        template <typename, bool>                                                                                           \
        friend struct ::ruvia::detail::db_relation_slot;                                                                    \
        RUVIA_DETAIL_ENTITY_OWNER_API(name, storage_type, entity_)                                                          \
        using sql_entity_type = name;                                                                                       \
        using columns_type = typename storage_type::columns_type;                                                           \
        using relations_type = typename storage_type::relations_type;                                                       \
        static constexpr std::string_view table_name() noexcept {                                                           \
            return storage_type::table_name();                                                                              \
        }                                                                                                                   \
        template <::ruvia::fixed_string field_name>                                                                         \
        static constexpr ::ruvia::db_field_reference<name, field_name> column(::ruvia::borrowed_text alias = {}) noexcept { \
            return ::ruvia::db_field_reference<name, field_name>(alias);                                                    \
        }                                                                                                                   \
        RUVIA_DETAIL_ENTITY_VALUE_API(entity_)                                                                              \
    };
namespace detail {
template <typename... columns>
struct db_projection_storage final {
    static_assert((::ruvia::is_db_column<columns>::value && ...));
    using type = ::ruvia::detail::entity_value_storage<columns...>;
};
}  // namespace detail

#define RUVIA_DB_PROJECTION(name, ...)                                                           \
    struct name final {                                                                          \
    private:                                                                                     \
        using storage_type = typename ::ruvia::detail::db_projection_storage<__VA_ARGS__>::type; \
        storage_type values_;                                                                    \
        RUVIA_DETAIL_ENTITY_OWNER_API(name, storage_type, values_)                               \
        using db_projection_type = name;                                                         \
        using columns_type = typename storage_type::columns_type;                                \
        RUVIA_DETAIL_ENTITY_VALUE_API(values_)                                                   \
    };

}  // namespace ruvia

namespace ruvia::detail {

[[noreturn]] inline void throw_invalid_postgresql_array() {
    throw db_conversion_error(db_conversion_error::code_type::invalid_format, "malformed one-dimensional PostgreSQL array");
}

struct postgresql_array_text final {
    std::string_view contents_;
    std::optional<std::size_t> elements_;
};

// PostgreSQL emits a dimension prefix only when the lower bound differs from
// one. A vector keeps the element sequence rather than the database subscripts.
[[nodiscard]] inline postgresql_array_text parse_postgresql_array_text(std::string_view source) {
    std::optional<std::size_t> elements;
    if (source.starts_with('[')) {
        std::int32_t lower{};
        std::int32_t upper{};
        const auto* const last = source.data() + source.size();
        const auto [lower_end, lower_error] = std::from_chars(source.data() + 1, last, lower);
        if (lower_error != std::errc{} || lower_end == last || *lower_end != ':') {
            throw_invalid_postgresql_array();
        }
        const auto [upper_end, upper_error] = std::from_chars(lower_end + 1, last, upper);
        if (upper_error != std::errc{} || upper_end == last || *upper_end != ']') {
            throw_invalid_postgresql_array();
        }
        // The server requires upper + 1 and the dimension length to fit int32.
        const auto length = std::int64_t{upper} - lower + 1;
        if (length <= 0 || length > (std::numeric_limits<std::int32_t>::max)() ||
            upper == (std::numeric_limits<std::int32_t>::max)() ||
            upper_end + 1 == last || upper_end[1] != '=') {
            throw_invalid_postgresql_array();
        }
        elements = static_cast<std::size_t>(length);
        source.remove_prefix(static_cast<std::size_t>(upper_end + 2 - source.data()));
    }
    if (source.size() < 2 || source.front() != '{' || source.back() != '}') {
        throw_invalid_postgresql_array();
    }
    return {source, elements};
}

template <typename visitor_type, typename reserve_type = void (*)(std::size_t)>
void for_each_postgresql_array_element(std::string_view source, visitor_type&& visit, reserve_type&& reserve = +[](std::size_t) noexcept {}) {
    const auto array = parse_postgresql_array_text(source);
    const auto contents = array.contents_;
    if (array.elements_) {
        // Every element occupies at least one byte plus a separator inside the
        // braces, so the arrived text bounds the declared length before reserve.
        if (*array.elements_ > (contents.size() - 1) / 2) {
            throw_invalid_postgresql_array();
        }
        reserve(*array.elements_);
    }
    std::string escaped_token;
    std::size_t element_count = 0;
    std::size_t token_start = 1;
    std::size_t chunk_start = 1;
    std::size_t token_end = 1;
    bool quoted = false;
    bool quoted_token = false;
    bool quote_closed = false;
    bool escaped = false;
    bool has_escapes = false;
    auto append = [&](std::size_t default_end) {
        const std::size_t end = quote_closed ? token_end : default_end;
        if (quoted || escaped || (end == token_start && !quoted_token) ||
            (array.elements_ && element_count >= *array.elements_)) {
            throw_invalid_postgresql_array();
        }
        std::string_view token;
        if (has_escapes) {
            if (end > chunk_start) {
                escaped_token.append(contents.data() + chunk_start, end - chunk_start);
            }
            token = escaped_token;
        } else {
            token = contents.substr(token_start, end - token_start);
        }
        ++element_count;
        if (token == "NULL" && !quoted_token) {
            visit(std::optional<std::string_view>{});
        } else {
            visit(std::optional<std::string_view>{token});
        }
        token_start = default_end + 1;
        chunk_start = token_start;
        token_end = token_start;
        quoted = false;
        quoted_token = false;
        quote_closed = false;
        has_escapes = false;
    };
    for (std::size_t i = 1; i + 1 < contents.size(); ++i) {
        const char ch = contents[i];
        if (escaped) {
            chunk_start = i;
            escaped = false;
        } else if (ch == '\\') {
            if (quote_closed) {
                throw_invalid_postgresql_array();
            }
            if (!has_escapes) {
                has_escapes = true;
                escaped_token.clear();
            }
            if (i > chunk_start) {
                escaped_token.append(contents.data() + chunk_start, i - chunk_start);
            }
            escaped = true;
            quoted_token = true;
        } else if (ch == '"') {
            if (quoted) {
                quoted = false;
                quote_closed = true;
                token_end = i;
            } else {
                if (i != token_start || quoted_token || quote_closed) {
                    throw_invalid_postgresql_array();
                }
                quoted = true;
                quoted_token = true;
                token_start = i + 1;
                chunk_start = i + 1;
            }
        } else if (ch == ',' && !quoted) {
            append(i);
        } else {
            if (quote_closed || (!quoted && (ch == '{' || ch == '}'))) {
                throw_invalid_postgresql_array();
            }
        }
    }
    if (contents.size() > 2) {
        append(contents.size() - 1);
    }
    if (array.elements_ && element_count != *array.elements_) {
        throw_invalid_postgresql_array();
    }
}

template <db_mapped_value t_type>
[[nodiscard]] t_type decode_array_element(std::string_view text) {
    if constexpr (is_optional<t_type>::value) {
        using v_type = typename is_optional<t_type>::value_type;
        return decode_array_element<v_type>(text);
    } else {
        return convert_db_text<t_type>(text);
    }
}

template <db_mapped_value t_type>
void decode_db_field(const db_field& field, t_type& out) {
    using u_type = std::remove_cv_t<t_type>;
    if constexpr (is_optional<u_type>::value) {
        using v_type = typename is_optional<u_type>::value_type;
        if (!field.value()) {
            out.reset();
            return;
        }
        auto& inner = out.emplace(entity_value_slot<v_type>::make_value());
        decode_db_field(field, inner);
    } else if constexpr (is_std_vector<u_type>::value) {
        using v_type = typename is_std_vector<u_type>::value_type;
        const auto source_value = field.value();
        if (!source_value) {
            throw_invalid_postgresql_array();
        }
        out.clear();
        for_each_postgresql_array_element(
            *source_value,
            [&out](std::optional<std::string_view> element) {
                if (!element) {
                    if constexpr (is_optional<v_type>::value) {
                        out.emplace_back(std::nullopt);
                    } else {
                        throw db_conversion_error(
                            db_conversion_error::code_type::invalid_format, "NULL array element");
                    }
                } else {
                    out.push_back(decode_array_element<v_type>(*element));
                }
            },
            [&out](std::size_t count) { out.reserve(count); });
    } else if constexpr (std::is_same_v<u_type, std::string>) {
        if (auto value = field.as<std::string_view>()) {
            out.assign(*value);
        } else {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "NULL in non-nullable entity column");
        }
    } else if constexpr (std::is_same_v<u_type, string>) {
        if (auto value = field.as<std::string_view>()) {
            out.assign_owned(*value);
        } else {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "NULL in non-nullable entity column");
        }
    } else {
        if (auto value = field.as<u_type>()) {
            out = *value;
        } else {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "NULL in non-nullable entity column");
        }
    }
}

template <typename e_type, typename c_type>
void decode_entity_field(e_type& entity, const db_field& field) {
    const auto raw = field.value();
    if (!raw) {
        if constexpr (c_type::options.nullable_) {
            entity.template set_null<c_type::name>();
            return;
        } else {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "NULL in non-nullable database entity column");
        }
    }
    using value_type = typename c_type::value_type;
    if constexpr (std::is_same_v<value_type, std::string> || std::is_same_v<value_type, string>) {
        entity.template set<c_type::name>(*raw);
    } else {
        auto value = entity_value_slot<value_type>::make_value();
        decode_db_field(field, value);
        entity.template set<c_type::name>(std::move(value));
    }
}

// SQL permits repeated result names; a mapped column must name exactly one.
struct db_result_column_lookup final {
    unique_lookup_status state_{unique_lookup_status::absent};
    std::size_t index_{0};
};

[[nodiscard]] inline db_result_column_lookup find_unique_db_result_column(
    std::span<const std::string> names, std::string_view name) noexcept {
    db_result_column_lookup result;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == name) {
            if (result.state_ == unique_lookup_status::found) {
                return {unique_lookup_status::repeated, index};
            }
            result = {unique_lookup_status::found, index};
        }
    }
    return result;
}

template <typename e_type>
class db_entity_row_decoder final {
    using columns_type = typename e_type::columns_type;
    static constexpr auto column_count = std::tuple_size_v<columns_type>;
    static constexpr auto missing_column = (std::numeric_limits<std::size_t>::max)();

public:
    explicit db_entity_row_decoder(const std::array<bool, column_count>& selected, bool wildcard = false)
        : selected_(selected),
          wildcard_(wildcard) {}

    e_type decode(const db_row& row) {
        const auto names = row.column_names();
        if (!schema_ || schema_->data() != names.data() || schema_->size() != names.size()) {
            // Entity column names are unique, so a result naming exactly the
            // entity columns in declaration order cannot repeat a name.
            bool ordered = names.size() == column_count;
            for_each_descriptor<columns_type>([&]<typename column_type, std::size_t i> {
                ordered = ordered && names[i] == column_type::name.view();
            });
            for_each_descriptor<columns_type>([&]<typename column_type, std::size_t i> {
                if (ordered) {
                    indices_[i] = i;
                    return;
                }
                if (!selected_[i] && !wildcard_) {
                    indices_[i] = missing_column;
                    return;
                }
                const auto lookup = find_unique_db_result_column(names, column_type::name.view());
                if (lookup.state_ == unique_lookup_status::repeated) {
                    throw db_conversion_error(
                        db_conversion_error::code_type::invalid_format, "database result has ambiguous mapped column names");
                }
                indices_[i] = lookup.state_ == unique_lookup_status::found ? lookup.index_ : missing_column;
            });
            schema_ = names;
        }
        e_type result;
        for_each_descriptor<columns_type>([&]<typename column_type, std::size_t i> {
            if (!selected_[i] && (!wildcard_ || indices_[i] == missing_column)) {
                return;
            }
            if (indices_[i] == missing_column) {
                throw db_conversion_error(
                    db_conversion_error::code_type::missing_column, "database result has no such column");
            }
            decode_entity_field<e_type, column_type>(result, row[indices_[i]]);
        });
        return result;
    }

private:
    std::array<bool, column_count> selected_{};
    std::array<std::size_t, column_count> indices_;
    bool wildcard_{false};
    // Used only while synchronously mapping a live, immutable db_rows result.
    std::optional<std::span<const std::string>> schema_{};
};

template <typename t_type>
void append_entity_array_value(std::string& output, const t_type& value) {
    if constexpr (is_optional<t_type>::value) {
        if (value) {
            append_entity_array_value(output, *value);
        } else {
            output += "NULL";
        }
    } else if constexpr (std::is_same_v<t_type, std::string> || std::is_same_v<t_type, string>) {
        output.push_back('"');
        auto remaining = std::string_view(value);
        while (!remaining.empty()) {
            const auto next_value = std::ranges::find_if(remaining, [](char ch) noexcept {
                return ch == '"' || ch == '\\' || ch == '\0';
            });
            if (next_value == remaining.end()) {
                output.append(remaining);
                break;
            }
            const auto special = static_cast<std::size_t>(next_value - remaining.begin());
            if (special != 0) {
                output.append(remaining.data(), special);
            }
            auto escaped_end = special;
            do {
                const char ch = remaining[escaped_end];
                if (ch == '\0') {
                    throw std::invalid_argument("PostgreSQL array text cannot contain NUL");
                }
                output.push_back('\\');
                output.push_back(ch);
                ++escaped_end;
            } while (escaped_end < remaining.size() &&
                     (remaining[escaped_end] == '"' || remaining[escaped_end] == '\\' || remaining[escaped_end] == '\0'));
            remaining.remove_prefix(escaped_end);
        }
        output.push_back('"');
    } else if constexpr (std::is_same_v<t_type, bool>) {
        output += value ? "t" : "f";
    } else if constexpr (std::is_floating_point_v<t_type>) {
        append_db_number(output, value);
    } else if constexpr (std::is_signed_v<t_type>) {
        append_db_number(output, static_cast<std::int64_t>(value));
    } else {
        append_db_number(output, static_cast<std::uint64_t>(value));
    }
}

template <typename t_type>
db_value entity_db_value(const t_type& value) {
    if constexpr (std::is_same_v<t_type, std::string> || std::is_same_v<t_type, string>) {
        return db_value(std::string_view(value));
    } else if constexpr (std::is_same_v<t_type, bool> || std::is_arithmetic_v<t_type>) {
        return db_value(value);
    } else if constexpr (is_std_vector<t_type>::value) {
        std::string encoded;
        encoded.reserve(2U + value.size() * 4U);
        encoded.push_back('{');
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                encoded.push_back(',');
            }
            first = false;
            if constexpr (std::is_same_v<typename is_std_vector<t_type>::value_type, bool>) {
                append_entity_array_value(encoded, static_cast<bool>(item));
            } else {
                append_entity_array_value(encoded, item);
            }
        }
        encoded.push_back('}');
        return db_value_access::owned_string(std::move(encoded));
    } else {
        static_assert(std::is_same_v<t_type, void>, "unsupported entity value type");
    }
}

}  // namespace ruvia::detail

namespace ruvia {

// Decoding owns every text/array element independently of db_rows. SQL NULL
// requires an optional destination; malformed values keep db_conversion_error.
template <db_mapped_value value_type>
[[nodiscard]] value_type decode_db_field(const db_field& field) {
    auto result = detail::entity_value_slot<value_type>::make_value();
    detail::decode_db_field(field, result);
    return result;
}

// A projection describes a subset of columns. Every declared column is required
// and must be unique in the result; unrelated result columns are ignored.
template <typename entity_type>
    requires(sql_entity<entity_type> || sql_projection<entity_type>)
[[nodiscard]] entity_type decode_db_row(const db_row& row) {
    std::array<bool, std::tuple_size_v<typename entity_type::columns_type>> selected;
    selected.fill(true);
    detail::db_entity_row_decoder<entity_type> decoder(selected);
    return decoder.decode(row);
}

template <typename entity_type>
    requires(sql_entity<entity_type> || sql_projection<entity_type>)
[[nodiscard]] entity_rows<entity_type> decode_db_rows(const db_rows& rows) {
    auto result = detail::db_result_access::make_entity_rows<entity_type>(rows.size());
    std::array<bool, std::tuple_size_v<typename entity_type::columns_type>> selected;
    selected.fill(true);
    detail::db_entity_row_decoder<entity_type> decoder(selected);
    for (const auto& row : rows) {
        result.push_back(decoder.decode(row));
    }
    return result;
}

}  // namespace ruvia
