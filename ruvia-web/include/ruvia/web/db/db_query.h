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

#include "ruvia/web/db/db_cache.h"
#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_parameter_pack.h"

namespace ruvia {

namespace detail {
class db_query_storage;
class db_query_compiler;
class db_relation_plan;
class db_query_cache_state;
struct db_query_plan;
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
    std::string custom_name_{};
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
    friend class detail::db_query_compiler;
    db_expression(const detail::db_query_storage* owner_value, std::size_t node_value) noexcept
        : owner_(owner_value),
          node_(node_value) {}
    const detail::db_query_storage* owner_{nullptr};
    std::size_t node_{0};
};

struct db_order_term final {
    db_expression expression_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
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
struct db_window_options final {
    std::vector<db_expression> partition_by_{};
    std::vector<db_order_term> order_by_{};
    std::optional<db_window_frame_options> frame_{};
};
struct db_case_branch final {
    db_expression when_{};
    db_expression then_{};
};
struct db_named_argument final {
    std::string name_{};
    db_expression value_{};
};
struct db_source_column final {
    std::string name_{};
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
    std::vector<std::string> columns_{};
};
struct db_lock_options final {
    db_row_lock mode_{db_row_lock::update};
    bool nowait_{false};
    bool skip_locked_{false};
    std::vector<std::string> tables_{};
};
struct db_assignment final {
    std::string column_{};
    db_expression value_{};
};
struct db_conflict_options final {
    std::vector<std::string> columns_{};
    std::string constraint_{};
    db_expression target_where_{};
    std::vector<db_assignment> update_{};
    db_expression update_where_{};
    bool do_nothing_{false};
    // MariaDB matches any unique key. This explicit mode does not pretend to
    // implement PostgreSQL's conflict target semantics.
    bool any_unique_key_{false};
};

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

// A synchronous relational query AST. Values, identifiers and nested queries
// become owned data at each construction call. Compilation is the sole place
// that emits SQL and assigns parameter positions.
class db_query final {
public:
    using expr_type = db_expression;

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
    expr_type sql(std::span<const std::string_view> parts, std::span<const expr_type> args);
    expr_type sql(std::initializer_list<std::string_view> parts, std::initializer_list<expr_type> args) {
        return sql(std::span<const std::string_view>(parts.begin(), parts.size()), std::span<const expr_type>(args.begin(), args.size()));
    }
    expr_type sql(std::string_view expression) {
        return sql(std::span<const std::string_view>(&expression, 1), {});
    }
    expr_type column(std::string_view name, std::string_view table = {});
    expr_type star(std::string_view table = {});
    // Owns the value in this query's resource before returning.
    expr_type value(const db_value& value);
    template <detail::db_parameter value_type>
        requires(!std::same_as<std::remove_cvref_t<value_type>, db_value>)
    expr_type value(value_type&& value) {
        return this->value(detail::make_immediate_db_parameter(std::forward<value_type>(value)));
    }
    expr_type excluded(std::string_view column);
    expr_type null_value();
    expr_type default_value();
    expr_type call(std::string_view function, std::span<const expr_type> args = {}, std::span<const db_named_argument> named = {});
    expr_type call(std::string_view function, std::initializer_list<expr_type> args) {
        return call(function, std::span<const expr_type>(args.begin(), args.size()));
    }
    expr_type coalesce(std::span<const expr_type> args);
    expr_type coalesce(std::initializer_list<expr_type> args) {
        return coalesce(std::span<const expr_type>(args.begin(), args.size()));
    }
    expr_type null_if(expr_type value, expr_type other);
    expr_type greatest(std::span<const expr_type> args);
    expr_type greatest(std::initializer_list<expr_type> args) {
        return greatest(std::span<const expr_type>(args.begin(), args.size()));
    }
    expr_type least(std::span<const expr_type> args);
    expr_type least(std::initializer_list<expr_type> args) {
        return least(std::span<const expr_type>(args.begin(), args.size()));
    }
    expr_type binary(expr_type lhs, db_binary_operator op, expr_type rhs);
    expr_type unary(db_unary_operator op, expr_type value);
    expr_type between(expr_type value, expr_type lower, expr_type upper, bool negate = false);
    expr_type tuple(std::span<const expr_type> values);
    expr_type tuple(std::initializer_list<expr_type> values) {
        return tuple(std::span<const expr_type>(values.begin(), values.size()));
    }
    expr_type list(std::span<const expr_type> values);
    expr_type list(std::initializer_list<expr_type> values) {
        return list(std::span<const expr_type>(values.begin(), values.size()));
    }
    expr_type array(std::span<const expr_type> values);
    expr_type array(std::initializer_list<expr_type> values) {
        return array(std::span<const expr_type>(values.begin(), values.size()));
    }
    expr_type any(expr_type array);
    expr_type all(expr_type array);
    expr_type cast(expr_type value, const db_type_definition& type);
    expr_type cast(expr_type value, db_data_type type) {
        return cast(value, db_type_definition{.data_type_ = type});
    }
    expr_type alias(expr_type value, std::string_view name);
    expr_type case_when(std::span<const db_case_branch> branches, expr_type otherwise = {});
    expr_type case_when(std::initializer_list<db_case_branch> branches, expr_type otherwise = {}) {
        return case_when(std::span<const db_case_branch>(branches.begin(), branches.size()), otherwise);
    }
    expr_type exists(const db_query& query);
    expr_type subquery(const db_query& query);
    expr_type aggregate(std::string_view function, std::span<const expr_type> args, bool distinct = false, std::span<const db_order_term> order = {});
    expr_type aggregate(std::string_view function, std::initializer_list<expr_type> args, bool distinct = false, std::span<const db_order_term> order = {}) {
        return aggregate(function, std::span<const expr_type>(args.begin(), args.size()), distinct, order);
    }
    expr_type filter(expr_type aggregate, expr_type predicate);
    expr_type over(expr_type function, const db_window_options& window);
    expr_type within_group(expr_type function, std::span<const db_order_term> order);
    expr_type extract(db_date_part part, expr_type value);
    expr_type subscript(expr_type array, expr_type index);
    expr_type collate(expr_type value, std::string_view collation);
    expr_type import_expression(expr_type expression, std::string_view source_qualifier = {}, std::string_view target_qualifier = {});
    [[nodiscard]] static std::pmr::string render_expression(expr_type expression, db_driver driver,
        std::pmr::memory_resource* resource, db_parameter_mode mode = db_parameter_mode::literal);

    db_query& select(std::span<const expr_type> projections);
    db_query& select(std::initializer_list<expr_type> projections) {
        return select(std::span<const expr_type>(projections.begin(), projections.size()));
    }
    db_query& select(expr_type projection) {
        return select(std::span<const expr_type>(&projection, 1));
    }
    db_query& add_select(expr_type projection);
    db_query& from(std::string_view table, std::string_view alias = {});
    db_query& from(const db_query& query, std::string_view alias, const db_source_options& options = {});
    db_query& from_function(expr_type function, std::string_view alias, const db_source_options& options = {});
    db_query& join(db_join_type type, std::string_view table, expr_type on = {}, std::string_view alias = {}, std::span<const std::string_view> using_columns = {});
    db_query& join(db_join_type type, const db_query& query, expr_type on, std::string_view alias, const db_source_options& options = {});
    db_query& join_function(db_join_type type, expr_type function, expr_type on, std::string_view alias, const db_source_options& options = {});
    db_query& where(expr_type predicate);
    db_query& and_where(expr_type predicate);
    db_query& or_where(expr_type predicate);
    db_query& group_by(std::span<const expr_type> expressions);
    db_query& group_by(std::initializer_list<expr_type> expressions) {
        return group_by(std::span<const expr_type>(expressions.begin(), expressions.size()));
    }
    db_query& add_group_by(expr_type expression);
    db_query& having(expr_type predicate);
    db_query& and_having(expr_type predicate);
    db_query& order_by(expr_type expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value);
    db_query& add_order_by(expr_type expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value);
    db_query& clear_order();
    db_query& limit(std::optional<std::uint64_t> count);
    db_query& offset(std::optional<std::uint64_t> count);
    db_query& distinct(bool enabled = true);
    db_query& distinct_on(std::span<const expr_type> expressions);
    db_query& distinct_on(std::initializer_list<expr_type> expressions) {
        return distinct_on(std::span<const expr_type>(expressions.begin(), expressions.size()));
    }
    db_query& returning(std::span<const expr_type> expressions);
    db_query& returning(std::initializer_list<expr_type> expressions) {
        return returning(std::span<const expr_type>(expressions.begin(), expressions.size()));
    }
    db_query& insert_into(std::string_view table, std::span<const std::string_view> columns = {}, std::string_view alias = {});
    db_query& insert_into(std::string_view table_value, std::initializer_list<std::string_view> columns, std::string_view alias = {}) {
        return insert_into(table_value, std::span<const std::string_view>(columns.begin(), columns.size()), alias);
    }
    db_query& values(std::span<const expr_type> row);
    db_query& values(std::initializer_list<expr_type> row) {
        return values(std::span<const expr_type>(row.begin(), row.size()));
    }
    db_query& insert_from(const db_query& query);
    db_query& on_conflict(const db_conflict_options& conflict);
    db_query& update(std::string_view table, std::string_view alias = {});
    db_query& set(std::string_view column, expr_type value);
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
    friend class detail::db_query_compiler;
    friend class detail::db_relation_plan;
    friend class db_schema;
    friend class detail::db_query_cache_state;
    friend class db_transaction;
    friend class db_handle;
    friend struct detail::db_query_plan;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;
    [[nodiscard]] bool cacheable() const;
    [[nodiscard]] std::optional<bool> cache_enabled() const;
    [[nodiscard]] std::optional<std::chrono::milliseconds> cache_duration() const;
    [[nodiscard]] std::string_view cache_id() const;
    void copy_cache(const db_query& source, std::string_view suffix = {});
    struct storage_deleter_type final {
        std::pmr::memory_resource* resource_{nullptr};
        void operator()(detail::db_query_storage* storage) const noexcept;
    };
    using storage_owner_type = std::unique_ptr<detail::db_query_storage, storage_deleter_type>;
    explicit db_query(storage_owner_type storage) noexcept;
    [[nodiscard]] static storage_owner_type copy_storage(const detail::db_query_storage& source, std::pmr::memory_resource* resource);
    [[nodiscard]] detail::db_query_storage& storage();
    [[nodiscard]] const detail::db_query_storage& storage() const;
    [[nodiscard]] std::size_t require_expression(expr_type expression) const;
    void require_select_query() const;
    [[nodiscard]] bool uses_source_name(std::string_view name) const;
    [[nodiscard]] bool uses_projection_name(std::string_view name) const;
    [[nodiscard]] std::optional<db_query> prepare_entity_read(
        std::span<const std::string_view> primary_key, std::string_view root_alias,
        db_driver driver) const;
    storage_owner_type storage_;
};

}  // namespace ruvia
