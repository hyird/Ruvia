#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_parameter_pack.h"

namespace ruvia {

namespace detail {
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
    std::uint64_t offset_{0};
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
struct db_source_options final {
    bool lateral_{false};
    bool with_ordinality_{false};
    std::vector<db_source_column> columns_{};
};
struct db_cte_options final {
    bool recursive_{false};
    db_materialization materialization_{db_materialization::default_value};
    std::vector<borrowed_text> columns_{};
};
struct db_lock_options final {
    db_row_lock mode_{db_row_lock::update};
    bool nowait_{false};
    bool skip_locked_{false};
    std::vector<borrowed_text> tables_{};
};

class db_query;
class db_predicate;
class db_expr;
struct db_order_term;
struct db_window_options;
struct db_case_branch;
struct db_selection;
struct db_assignment;

namespace detail {
struct db_expr_tree_deleter final {
    std::pmr::memory_resource* resource_{nullptr};
    void operator()(db_expr_tree* tree) const noexcept;
};

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
        db_expression expression, std::string_view table = {}, bool aliased = false) noexcept;
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
    [[nodiscard]] static db_owned_expression make_static_excluded(
        std::string_view name, std::string_view table) noexcept;
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
    [[nodiscard]] static db_owned_expression make_in_list(
        const db_owned_expression& lhs, db_binary_operator op, std::span<const db_value> values);
    [[nodiscard]] static db_owned_expression make_in_expr_list(
        const db_owned_expression& lhs, db_binary_operator op, std::span<const db_owned_expression> values);
    [[nodiscard]] static db_owned_expression make_in_subquery(
        const db_owned_expression& lhs, db_binary_operator op, const db_query& subquery);
    [[nodiscard]] static db_owned_expression make_exists(const db_query& subquery);
    [[nodiscard]] static db_owned_expression make_array_op(const db_owned_expression& lhs,
        db_binary_operator op, std::span<const db_value> values, const db_type_definition& array_type);
    [[nodiscard]] static db_owned_expression make_json_keys_op(
        const db_owned_expression& lhs, db_binary_operator op, std::span<const std::string_view> keys);

private:
    using tree_deleter_type = db_expr_tree_deleter;
    using tree_owner_type = std::unique_ptr<db_expr_tree, tree_deleter_type>;

    [[nodiscard]] static tree_owner_type allocate_tree(std::pmr::memory_resource* resource = nullptr);
    [[nodiscard]] static db_owned_expression from_tree(tree_owner_type tree) noexcept;
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
        : owned_(detail::db_owned_expression::from_borrowed(expression)) {}
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
    db_expr(db_expression expression, std::string_view table = {}, bool aliased = false) noexcept
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
        return call("jsonb_strip_nulls", {target});
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

    [[nodiscard]] const detail::db_owned_expression& owned() const& noexcept {
        return owned_;
    }
    [[nodiscard]] detail::db_owned_expression&& owned() && noexcept {
        return std::move(owned_);
    }

private:
    template <typename, fixed_string>
    friend class db_field_reference;
    friend db_expr operator+(const db_expr& lhs, const db_expr& rhs);
    friend db_expr operator-(const db_expr& lhs, const db_expr& rhs);
    friend db_expr operator*(const db_expr& lhs, const db_expr& rhs);
    friend db_expr operator/(const db_expr& lhs, const db_expr& rhs);
    friend db_expr operator%(const db_expr& lhs, const db_expr& rhs);
    friend db_expr operator&(const db_expr& lhs, const db_expr& rhs);
    friend db_expr operator|(const db_expr& lhs, const db_expr& rhs);
    friend db_expr operator^(const db_expr& lhs, const db_expr& rhs);
    template <detail::db_parameter v_type>
    friend db_expr operator+(const db_expr& lhs, v_type&& rhs);
    template <detail::db_parameter v_type>
    friend db_expr operator-(const db_expr& lhs, v_type&& rhs);
    template <detail::db_parameter v_type>
    friend db_expr operator*(const db_expr& lhs, v_type&& rhs);
    template <detail::db_parameter v_type>
    friend db_expr operator/(const db_expr& lhs, v_type&& rhs);
    template <detail::db_parameter v_type>
    friend db_expr operator%(const db_expr& lhs, v_type&& rhs);
    friend db_predicate operator==(const db_expr& lhs, const db_expr& rhs);
    friend db_predicate operator!=(const db_expr& lhs, const db_expr& rhs);
    friend db_predicate operator<(const db_expr& lhs, const db_expr& rhs);
    friend db_predicate operator<=(const db_expr& lhs, const db_expr& rhs);
    friend db_predicate operator>(const db_expr& lhs, const db_expr& rhs);
    friend db_predicate operator>=(const db_expr& lhs, const db_expr& rhs);
    template <detail::db_parameter v_type>
    friend db_predicate operator==(const db_expr& lhs, v_type&& rhs);
    template <detail::db_parameter v_type>
    friend db_predicate operator!=(const db_expr& lhs, v_type&& rhs);
    template <detail::db_parameter v_type>
    friend db_predicate operator<(const db_expr& lhs, v_type&& rhs);
    template <detail::db_parameter v_type>
    friend db_predicate operator<=(const db_expr& lhs, v_type&& rhs);
    template <detail::db_parameter v_type>
    friend db_predicate operator>(const db_expr& lhs, v_type&& rhs);
    template <detail::db_parameter v_type>
    friend db_predicate operator>=(const db_expr& lhs, v_type&& rhs);

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
    return lhs.binary(db_binary_operator::add, rhs);
}
[[nodiscard]] inline db_expr operator-(const db_expr& lhs, const db_expr& rhs) {
    return lhs.binary(db_binary_operator::subtract, rhs);
}
[[nodiscard]] inline db_expr operator*(const db_expr& lhs, const db_expr& rhs) {
    return lhs.binary(db_binary_operator::multiply, rhs);
}
[[nodiscard]] inline db_expr operator/(const db_expr& lhs, const db_expr& rhs) {
    return lhs.binary(db_binary_operator::divide, rhs);
}
[[nodiscard]] inline db_expr operator%(const db_expr& lhs, const db_expr& rhs) {
    return lhs.binary(db_binary_operator::modulo, rhs);
}
[[nodiscard]] inline db_expr operator&(const db_expr& lhs, const db_expr& rhs) {
    return lhs.binary(db_binary_operator::bit_and, rhs);
}
[[nodiscard]] inline db_expr operator|(const db_expr& lhs, const db_expr& rhs) {
    return lhs.binary(db_binary_operator::bit_or, rhs);
}
[[nodiscard]] inline db_expr operator^(const db_expr& lhs, const db_expr& rhs) {
    return lhs.binary(db_binary_operator::bit_xor, rhs);
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator+(const db_expr& lhs, v_type&& rhs) {
    return lhs.binary(db_binary_operator::add, db_expr::value(std::forward<v_type>(rhs)));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator-(const db_expr& lhs, v_type&& rhs) {
    return lhs.binary(db_binary_operator::subtract, db_expr::value(std::forward<v_type>(rhs)));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator*(const db_expr& lhs, v_type&& rhs) {
    return lhs.binary(db_binary_operator::multiply, db_expr::value(std::forward<v_type>(rhs)));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator/(const db_expr& lhs, v_type&& rhs) {
    return lhs.binary(db_binary_operator::divide, db_expr::value(std::forward<v_type>(rhs)));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator%(const db_expr& lhs, v_type&& rhs) {
    return lhs.binary(db_binary_operator::modulo, db_expr::value(std::forward<v_type>(rhs)));
}

[[nodiscard]] inline db_predicate operator==(const db_expr& lhs, const db_expr& rhs) {
    return lhs.compare_expr(db_binary_operator::equal, rhs);
}
[[nodiscard]] inline db_predicate operator!=(const db_expr& lhs, const db_expr& rhs) {
    return lhs.compare_expr(db_binary_operator::not_equal, rhs);
}
[[nodiscard]] inline db_predicate operator<(const db_expr& lhs, const db_expr& rhs) {
    return lhs.compare_expr(db_binary_operator::less, rhs);
}
[[nodiscard]] inline db_predicate operator<=(const db_expr& lhs, const db_expr& rhs) {
    return lhs.compare_expr(db_binary_operator::less_equal, rhs);
}
[[nodiscard]] inline db_predicate operator>(const db_expr& lhs, const db_expr& rhs) {
    return lhs.compare_expr(db_binary_operator::greater, rhs);
}
[[nodiscard]] inline db_predicate operator>=(const db_expr& lhs, const db_expr& rhs) {
    return lhs.compare_expr(db_binary_operator::greater_equal, rhs);
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator==(const db_expr& lhs, v_type&& rhs) {
    return lhs.compare_expr(db_binary_operator::equal, db_expr::value(std::forward<v_type>(rhs)));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator!=(const db_expr& lhs, v_type&& rhs) {
    return lhs.compare_expr(db_binary_operator::not_equal, db_expr::value(std::forward<v_type>(rhs)));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator<(const db_expr& lhs, v_type&& rhs) {
    return lhs.compare_expr(db_binary_operator::less, db_expr::value(std::forward<v_type>(rhs)));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator<=(const db_expr& lhs, v_type&& rhs) {
    return lhs.compare_expr(db_binary_operator::less_equal, db_expr::value(std::forward<v_type>(rhs)));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator>(const db_expr& lhs, v_type&& rhs) {
    return lhs.compare_expr(db_binary_operator::greater, db_expr::value(std::forward<v_type>(rhs)));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator>=(const db_expr& lhs, v_type&& rhs) {
    return lhs.compare_expr(db_binary_operator::greater_equal, db_expr::value(std::forward<v_type>(rhs)));
}

struct db_order_term final {
    db_expr expression_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};
struct db_window_options final {
    std::vector<db_expr> partition_by_{};
    std::vector<db_order_term> order_by_{};
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
    [[nodiscard]] bool has_expression() const noexcept {
        return !expression_.empty();
    }
    [[nodiscard]] db_expression expression(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
        return expression_.empty() ? db_expression{} : expression_.expression(target, table, alias);
    }
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

struct db_conflict_options final {
    std::vector<borrowed_text> columns_{};
    borrowed_text constraint_{};
    db_expression target_where_{};
    std::vector<db_assignment> update_{};
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
inline db_expr db_expr::jsonb_agg(const db_expr& arg, std::span<const db_order_term> order) {
    const std::array args{arg};
    return aggregate("jsonb_agg", args, false, order);
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
    db_statement& operator=(db_statement&&) = delete;
    [[nodiscard]] std::string_view sql() const& noexcept {
        return sql_;
    }
    std::string_view sql() const&& = delete;
    [[nodiscard]] std::span<const db_value> params() const& noexcept {
        return params_;
    }
    std::span<const db_value> params() const&& = delete;
    [[nodiscard]] bool returns_rows() const noexcept {
        return returns_rows_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return sql_.get_allocator().resource();
    }

private:
    friend class db_query;
    friend class db_handle;
    friend class db_transaction;
    friend struct detail::db_query_plan;
    db_statement(std::pmr::string sql, std::pmr::vector<db_value> params, bool rows)
        : sql_(std::move(sql)),
          params_(std::move(params)),
          returns_rows_(rows) {}
    std::pmr::string sql_;
    std::pmr::vector<db_value> params_;
    bool returns_rows_{false};
};

// A synchronous relational query AST and expression owner. Values, identifiers
// and nested queries become owned data at each construction call. Expression
// arguments must belong to this owner; import_expression() copies foreign nodes.
// Returned expression views borrow this owner's storage; its memory resource must
// outlive it. Repository and builder entry points copy expression nodes before
// returning, so their source owner can then be destroyed. Compilation is the sole
// place that emits SQL and assigns parameter positions.
struct db_upsert_options;
namespace detail {
template <typename entity_type>
void apply_entity_upsert(db_query& query, std::span<const std::string_view> inserted_columns,
    const db_upsert_options& options, db_driver driver, std::string_view qualifier = entity_type::table_name());
}  // namespace detail

class db_query final {
public:
    explicit db_query(std::pmr::memory_resource* resource = nullptr);
    db_query(const db_query&) = delete;
    db_query& operator=(const db_query&) = delete;
    db_query(db_query&&) noexcept;
    db_query& operator=(db_query&&) noexcept;
    ~db_query();

    [[nodiscard]] std::pmr::memory_resource* resource() const;
    [[nodiscard]] db_query clone(std::pmr::memory_resource* resource) const;
    [[nodiscard]] bool returns_rows() const;
    [[nodiscard]] bool has_where() const;
    [[nodiscard]] bool has_writes() const;
    [[nodiscard]] bool has_grouping() const;
    db_query& cache(const db_cache_setting_type& setting);
    db_query& cache(std::string_view id, std::optional<std::chrono::milliseconds> milliseconds = {});

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
    db_expression exists(const db_query& query);
    db_expression subquery(const db_query& query);
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
    // Throws std::length_error if expression/query nesting exceeds 256 levels.
    db_expression import_expression(db_expression expression, std::string_view source_qualifier = {}, std::string_view target_qualifier = {});
    [[nodiscard]] static std::pmr::string render_expression(db_expression expression, db_driver driver,
        std::pmr::memory_resource* resource, db_parameter_mode mode = db_parameter_mode::literal);

    db_query& select(std::span<const db_expression> projections);
    db_query& select(std::initializer_list<db_expression> projections) {
        return select(std::span<const db_expression>(projections.begin(), projections.size()));
    }
    db_query& select(db_expression projection) {
        return select(std::span<const db_expression>(&projection, 1));
    }
    db_query& add_select(db_expression projection);
    db_query& from(std::string_view table, std::string_view alias = {});
    db_query& from(const db_query& query, std::string_view alias, const db_source_options& options = {});
    db_query& from_function(db_expression function, std::string_view alias, const db_source_options& options = {});
    db_query& clear_source();
    db_query& join(db_join_type type, std::string_view table, db_expression on = {}, std::string_view alias = {}, std::span<const std::string_view> using_columns = {});
    db_query& join(db_join_type type, std::string_view table_value, db_expression on, std::string_view alias, std::initializer_list<std::string_view> using_columns) {
        return join(type, table_value, on, alias, std::span<const std::string_view>(using_columns.begin(), using_columns.size()));
    }
    db_query& join(db_join_type type, const db_query& query, db_expression on, std::string_view alias, const db_source_options& options = {});
    db_query& join_function(db_join_type type, db_expression function, db_expression on, std::string_view alias, const db_source_options& options = {});
    db_query& where(db_expression predicate);
    db_query& and_where(db_expression predicate);
    db_query& or_where(db_expression predicate);
    db_query& group_by(std::span<const db_expression> expressions);
    db_query& group_by(std::initializer_list<db_expression> expressions) {
        return group_by(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    db_query& add_group_by(db_expression expression);
    db_query& having(db_expression predicate);
    db_query& and_having(db_expression predicate);
    db_query& or_having(db_expression predicate);
    db_query& order_by(db_expression expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value);
    db_query& add_order_by(db_expression expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value);
    db_query& clear_order();
    db_query& limit(std::optional<std::uint64_t> count);
    db_query& offset(std::optional<std::uint64_t> count);
    db_query& distinct(bool enabled = true);
    db_query& distinct_on(std::span<const db_expression> expressions);
    db_query& distinct_on(std::initializer_list<db_expression> expressions) {
        return distinct_on(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    db_query& returning(std::span<const db_expression> expressions);
    db_query& returning(std::initializer_list<db_expression> expressions) {
        return returning(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    db_query& insert_into(std::string_view table, std::span<const std::string_view> columns = {}, std::string_view alias = {});
    db_query& insert_into(std::string_view table_value, std::initializer_list<std::string_view> columns, std::string_view alias = {}) {
        return insert_into(table_value, std::span<const std::string_view>(columns.begin(), columns.size()), alias);
    }
    db_query& values(std::span<const db_expression> row);
    db_query& values(std::initializer_list<db_expression> row) {
        return values(std::span<const db_expression>(row.begin(), row.size()));
    }
    db_query& insert_from(const db_query& query);
    db_query& on_conflict(const db_conflict_options& conflict);
    db_query& update(std::string_view table, std::string_view alias = {});
    db_query& set(std::string_view column, db_expression value);
    db_query& set(const db_assignment& assignment);
    db_query& set(std::span<const db_assignment> assignments);
    db_query& set(std::initializer_list<db_assignment> assignments) {
        return set(std::span<const db_assignment>(assignments.begin(), assignments.size()));
    }
    db_query& update_from(std::string_view table, std::string_view alias = {});
    db_query& update_from(const db_query& query, std::string_view alias);
    db_query& delete_from(std::string_view table, std::string_view alias = {});
    db_query& delete_using(std::string_view table, std::string_view alias = {});
    db_query& delete_using(const db_query& query, std::string_view alias);
    db_query& lock(const db_lock_options& options);
    db_query& clear_lock();
    db_query& with(std::string_view name, const db_query& query, const db_cte_options& options = {});
    db_query& combine(db_set_operation operation, const db_query& query);

    [[nodiscard]] db_statement compile(db_driver driver, std::pmr::memory_resource* resource,
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
    template <typename entity_type>
    friend void detail::apply_entity_upsert(db_query& query, std::span<const std::string_view> inserted_columns,
        const db_upsert_options& options, db_driver driver, std::string_view qualifier);
    [[nodiscard]] bool cacheable() const;
    [[nodiscard]] std::optional<bool> cache_enabled() const;
    [[nodiscard]] std::optional<std::chrono::milliseconds> cache_duration() const;
    [[nodiscard]] std::string_view cache_id() const;
    [[nodiscard]] detail::db_cache_shape cache_shape() const;
    void copy_cache(const db_query& source, detail::db_cache_shape shape);
    void with_columns(std::string_view name, const db_query& query, db_materialization materialization,
        std::span<const std::pmr::string> columns);
    void set_conflict(std::span<const borrowed_text> columns, std::string_view constraint,
        db_expression target_where, std::span<const std::pair<std::string_view, db_expression>> updates,
        db_expression update_where, bool do_nothing, bool any_unique_key);
    struct storage_deleter_type final {
        std::pmr::memory_resource* resource_{nullptr};
        void operator()(detail::db_query_storage* storage) const noexcept;
    };
    using storage_owner_type = std::unique_ptr<detail::db_query_storage, storage_deleter_type>;
    explicit db_query(storage_owner_type storage) noexcept;
    [[nodiscard]] static storage_owner_type copy_storage(const detail::db_query_storage& source, std::pmr::memory_resource* resource);
    [[nodiscard]] detail::db_query_storage& storage();
    [[nodiscard]] const detail::db_query_storage& storage() const;
    [[nodiscard]] std::size_t require_expression(db_expression expression) const;
    void require_select_query() const;
    // Every expression builder constructs exactly its own node kind and
    // appends it here; the returned handle addresses the appended node.
    [[nodiscard]] detail::db_query_node make_node(detail::db_node_kind kind, std::span<const db_expression> args = {}) const;
    db_expression push_node(detail::db_query_node&& node);
    [[nodiscard]] std::size_t add_subquery(const db_query& query);
    storage_owner_type storage_;
};

}  // namespace ruvia
