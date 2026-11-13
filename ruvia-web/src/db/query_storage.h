#pragma once

#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/db/db_query.h"

#include "db/db_query_execution.h"
#include "db/db_sql_format.h"

namespace ruvia::detail {

constexpr std::size_t no_db_node = std::numeric_limits<std::size_t>::max();

enum class db_node_kind : std::uint8_t {
    sql,
    column,
    star,
    value,
    default_value,
    excluded,
    function,
    coalesce,
    null_if,
    greatest,
    least,
    binary,
    unary,
    between,
    tuple,
    list,
    array,
    any,
    all,
    cast,
    alias,
    case_value,
    exists,
    subquery,
    aggregate,
    filter,
    window,
    within_group,
    extract,
    subscript,
    collate,
    at_time_zone,
};
enum class db_query_kind : std::uint8_t { select,
    values,
    insert,
    update,
    delete_value };
enum class db_source_kind : std::uint8_t { table,
    query,
    function };

struct db_stored_type final {
    db_stored_type() = default;
    db_stored_type(const db_stored_type&) = default;
    db_stored_type& operator=(const db_stored_type&) = default;
    db_stored_type(db_stored_type&&) noexcept = default;
    db_stored_type& operator=(db_stored_type&&) noexcept = default;
    db_stored_type(const db_type_definition& source_value)
        : data_type_(source_value.data_type_),
          custom_name_(source_value.custom_name_),
          length_(source_value.length_),
          precision_(source_value.precision_),
          scale_(source_value.scale_),
          array_(source_value.array_) {}
    db_data_type data_type_{db_data_type::inferred};
    std::string custom_name_;
    std::size_t length_{0};
    unsigned precision_{0};
    unsigned scale_{0};
    bool array_{false};
};
struct db_stored_order final {
    std::size_t expression_{no_db_node};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};
struct db_stored_named_argument final {
    db_stored_named_argument(std::string_view name, std::size_t expression)
        : name_(name),
          expression_(expression) {}
    std::string name_;
    std::size_t expression_;
};

struct db_query_node final {
    db_query_node(db_node_kind kind)
        : kind_(kind),
          value_(nullptr) {}
    db_query_node(db_node_kind kind, db_value&& value)
        : kind_(kind),
          value_(std::move(value)) {}
    db_query_node(const db_query_node& source_value)
        : kind_(source_value.kind_),
          value_(db_value(source_value.value_)),
          text_(source_value.text_),
          qualifier_(source_value.qualifier_),
          args_(source_value.args_),
          orders_(source_value.orders_),
          type_(source_value.type_),
          binary_(source_value.binary_),
          unary_(source_value.unary_),
          date_part_(source_value.date_part_),
          left_(source_value.left_),
          right_(source_value.right_),
          query_(source_value.query_),
          flag_(source_value.flag_),
          explicit_alias_(source_value.explicit_alias_),
          frame_(source_value.frame_) {
        named_.reserve(source_value.named_.size());
        for (const auto& argument : source_value.named_) {
            named_.emplace_back(argument.name_, argument.expression_);
        }
    }
    db_query_node(db_query_node&&) noexcept = default;
    db_query_node& operator=(db_query_node&&) = delete;
    db_node_kind kind_;
    db_value value_;
    std::string text_;
    std::string qualifier_;
    std::vector<std::size_t> args_;
    std::vector<db_stored_named_argument> named_;
    std::vector<db_stored_order> orders_;
    db_stored_type type_;
    db_binary_operator binary_{db_binary_operator::equal};
    db_unary_operator unary_{db_unary_operator::not_value};
    db_date_part date_part_{db_date_part::epoch};
    std::size_t left_{no_db_node};
    std::size_t right_{no_db_node};
    std::size_t query_{no_db_node};
    bool flag_{false};
    bool explicit_alias_{false};
    std::optional<db_window_frame_options> frame_{};
};
// AND and OR are associative, so a left-deep chain of one of them (as built
// by and_where/or_where or `p = std::move(p) && q`) is a single nesting level
// for import and compilation. True when `child` continues `parent`'s chain.
[[nodiscard]] inline bool continues_logical_chain(
    const std::vector<db_query_node>& nodes, const db_query_node& parent, std::size_t child) noexcept {
    if (parent.kind_ != db_node_kind::binary || child == no_db_node ||
        (parent.binary_ != db_binary_operator::and_value && parent.binary_ != db_binary_operator::or_value)) {
        return false;
    }
    const auto& node = nodes[child];
    return node.kind_ == db_node_kind::binary && node.binary_ == parent.binary_;
}
struct db_stored_source_column final {
    db_stored_source_column(const db_source_column& source_value)
        : name_(source_value.name_),
          type_(source_value.type_) {}
    db_stored_source_column(const db_stored_source_column&) = default;
    db_stored_source_column& operator=(const db_stored_source_column&) = default;
    db_stored_source_column(db_stored_source_column&&) noexcept = default;
    db_stored_source_column& operator=(db_stored_source_column&&) noexcept = default;
    std::string name_;
    db_stored_type type_;
};
struct db_query_source final {
    db_query_source() = default;
    db_query_source(const db_query_source&) = default;
    db_query_source& operator=(const db_query_source&) = default;
    db_query_source(db_query_source&&) noexcept = default;
    db_query_source& operator=(db_query_source&&) noexcept = default;
    db_source_kind kind_{db_source_kind::table};
    std::string name_;
    std::string alias_;
    std::size_t expression_{no_db_node};
    std::size_t query_{no_db_node};
    bool lateral_{false};
    bool ordinality_{false};
    std::vector<db_stored_source_column> columns_;
};
// Whether a FROM item binds `name`: its alias when present, otherwise its
// table name or, for a schema-qualified table, any dot-aligned suffix of it.
[[nodiscard]] inline bool source_binds_name(const db_query_source& source_value, std::string_view name) noexcept {
    if (!source_value.alias_.empty()) {
        return source_value.alias_ == name;
    }
    const std::string_view table = source_value.name_;
    return table == name ||
           (table.size() > name.size() && table.ends_with(name) && table[table.size() - name.size() - 1] == '.');
}
struct db_stored_join final {
    db_stored_join(db_join_type type, db_query_source source_value, std::size_t on)
        : type_(type),
          source_(std::move(source_value)),
          on_(on) {}
    db_stored_join(const db_stored_join& source_value)
        : type_(source_value.type_),
          source_(source_value.source_),
          on_(source_value.on_),
          using_columns_(source_value.using_columns_) {}
    db_join_type type_;
    db_query_source source_;
    std::size_t on_{no_db_node};
    std::vector<std::string> using_columns_;
};
struct db_stored_assignment final {
    db_stored_assignment(std::string_view column, std::size_t expression)
        : column_(column),
          expression_(expression) {}
    std::string column_;
    std::size_t expression_;
};
struct db_stored_cte final {
    template <class column_range>
    db_stored_cte(std::string_view name, std::size_t query, bool recursive, db_materialization materialization,
        const column_range& columns)
        : name_(name),
          query_(query),
          recursive_(recursive),
          materialization_(materialization) {
        columns_.reserve(std::ranges::size(columns));
        for (const auto& column : columns) {
            columns_.emplace_back(std::string_view(column));
        }
    }
    db_stored_cte(const db_stored_cte& source_value)
        : name_(source_value.name_),
          query_(source_value.query_),
          recursive_(source_value.recursive_),
          materialization_(source_value.materialization_),
          columns_(source_value.columns_) {}
    std::string name_;
    std::size_t query_;
    bool recursive_{false};
    db_materialization materialization_{db_materialization::default_value};
    std::vector<std::string> columns_;
};
struct db_stored_set_operation final {
    db_set_operation operation_;
    std::size_t query_;
};
struct db_stored_conflict final {
    db_stored_conflict() = default;
    db_stored_conflict(const db_stored_conflict&) = default;
    db_stored_conflict& operator=(const db_stored_conflict&) = default;
    db_stored_conflict(db_stored_conflict&&) noexcept = default;
    db_stored_conflict& operator=(db_stored_conflict&&) noexcept = default;
    std::vector<std::string> columns_;
    std::string constraint_;
    std::size_t target_where_{no_db_node};
    std::vector<db_stored_assignment> assignments_;
    std::size_t update_where_{no_db_node};
    bool do_nothing_{false};
    bool any_unique_key_{false};
};
struct db_stored_lock final {
    db_stored_lock(const db_lock_options& source_value)
        : mode_(source_value.mode_),
          nowait_(source_value.nowait_),
          skip_locked_(source_value.skip_locked_) {
        tables_.reserve(source_value.tables_.size());
        for (const auto& table : source_value.tables_) {
            tables_.emplace_back(table);
        }
    }
    db_stored_lock(const db_stored_lock&) = default;
    db_stored_lock& operator=(const db_stored_lock&) = default;
    db_stored_lock(db_stored_lock&&) noexcept = default;
    db_stored_lock& operator=(db_stored_lock&&) noexcept = default;
    db_row_lock mode_;
    bool nowait_{false};
    bool skip_locked_{false};
    std::vector<std::string> tables_;
};

class db_query_storage final {
public:
    db_query_storage() = default;
    db_query_storage(const db_query_storage& source);

    std::vector<db_query_node> nodes_;
    std::vector<db_query> queries_;
    db_query_kind kind_{db_query_kind::select};
    std::string target_;
    std::string target_alias_;
    std::vector<std::string> columns_;
    std::vector<std::size_t> projections_;
    std::vector<std::size_t> groups_;
    std::vector<db_stored_order> orders_;
    bool distinct_{false};
    std::vector<std::size_t> distinct_on_;
    std::vector<std::size_t> returning_;
    std::optional<db_query_source> source_{};
    std::vector<db_stored_join> joins_;
    std::vector<std::vector<std::size_t>> rows_;
    std::vector<db_stored_assignment> assignments_;
    std::size_t insert_query_{no_db_node};
    std::size_t predicate_{no_db_node};
    std::size_t having_{no_db_node};
    std::optional<std::uint64_t> limit_{};
    std::optional<std::uint64_t> offset_{};
    std::optional<db_stored_conflict> conflict_{};
    std::optional<db_stored_lock> lock_{};
    std::vector<db_stored_cte> ctes_;
    std::vector<db_stored_set_operation> set_operations_;

    std::optional<bool> cache_enabled_{};
    std::optional<std::chrono::milliseconds> cache_duration_{};
    std::string cache_id_;
    // The result shape a repository builder derived this query as; with
    // cache_id_ it names the id-keyed entry.
    db_cache_shape cache_shape_{db_cache_shape::rows};

    [[nodiscard]] bool returns_rows() const noexcept {
        return kind_ == db_query_kind::select || kind_ == db_query_kind::values || !returning_.empty();
    }
};

}  // namespace ruvia::detail
