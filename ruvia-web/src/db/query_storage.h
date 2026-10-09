#pragma once

#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/web/db/db_query.h"

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
    explicit db_stored_type(std::pmr::memory_resource* resource)
        : custom_name_(resource) {}
    db_stored_type(const db_stored_type& source_value, std::pmr::memory_resource* resource)
        : data_type_(source_value.data_type_),
          custom_name_(source_value.custom_name_, resource),
          length_(source_value.length_),
          precision_(source_value.precision_),
          scale_(source_value.scale_),
          array_(source_value.array_) {}
    db_stored_type(const db_type_definition& source_value, std::pmr::memory_resource* resource)
        : data_type_(source_value.data_type_),
          custom_name_(source_value.custom_name_, resource),
          length_(source_value.length_),
          precision_(source_value.precision_),
          scale_(source_value.scale_),
          array_(source_value.array_) {}
    db_data_type data_type_{db_data_type::inferred};
    std::pmr::string custom_name_;
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
    db_stored_named_argument(std::string_view name, std::size_t expression, std::pmr::memory_resource* resource)
        : name_(name, resource),
          expression_(expression) {}
    std::pmr::string name_;
    std::size_t expression_;
};

struct db_query_node final {
    db_query_node(db_node_kind kind, std::pmr::memory_resource* resource)
        : kind_(kind),
          value_(nullptr),
          text_(resource),
          qualifier_(resource),
          args_(resource),
          named_(resource),
          orders_(resource),
          type_(resource) {}
    db_query_node(const db_query_node& source_value, std::pmr::memory_resource* resource)
        : kind_(source_value.kind_),
          value_(clone_db_value_for_resource(source_value.value_, resource)),
          text_(source_value.text_, resource),
          qualifier_(source_value.qualifier_, resource),
          args_(source_value.args_, resource),
          named_(resource),
          orders_(source_value.orders_, resource),
          type_(source_value.type_, resource),
          binary_(source_value.binary_),
          unary_(source_value.unary_),
          date_part_(source_value.date_part_),
          left_(source_value.left_),
          right_(source_value.right_),
          query_(source_value.query_),
          flag_(source_value.flag_),
          frame_(source_value.frame_) {
        named_.reserve(source_value.named_.size());
        for (const auto& argument : source_value.named_) {
            named_.emplace_back(argument.name_, argument.expression_, resource);
        }
    }
    db_query_node(db_query_node&&) noexcept = default;
    db_query_node& operator=(db_query_node&&) = delete;
    db_node_kind kind_;
    db_value value_;
    std::pmr::string text_;
    std::pmr::string qualifier_;
    std::pmr::vector<std::size_t> args_;
    std::pmr::vector<db_stored_named_argument> named_;
    std::pmr::vector<db_stored_order> orders_;
    db_stored_type type_;
    db_binary_operator binary_{db_binary_operator::equal};
    db_unary_operator unary_{db_unary_operator::not_value};
    db_date_part date_part_{db_date_part::epoch};
    std::size_t left_{no_db_node};
    std::size_t right_{no_db_node};
    std::size_t query_{no_db_node};
    bool flag_{false};
    std::optional<db_window_frame_options> frame_{};
};
struct db_stored_source_column final {
    db_stored_source_column(const db_source_column& source_value, std::pmr::memory_resource* resource)
        : name_(source_value.name_, resource),
          type_(source_value.type_, resource) {}
    db_stored_source_column(const db_stored_source_column& source_value, std::pmr::memory_resource* resource)
        : name_(source_value.name_, resource),
          type_(source_value.type_, resource) {}
    std::pmr::string name_;
    db_stored_type type_;
};
struct db_query_source final {
    explicit db_query_source(std::pmr::memory_resource* resource)
        : name_(resource),
          alias_(resource),
          columns_(resource) {}
    db_query_source(const db_query_source& source_value, std::pmr::memory_resource* resource)
        : kind_(source_value.kind_),
          name_(source_value.name_, resource),
          alias_(source_value.alias_, resource),
          expression_(source_value.expression_),
          query_(source_value.query_),
          lateral_(source_value.lateral_),
          ordinality_(source_value.ordinality_),
          columns_(resource) {
        for (const auto& column : source_value.columns_) {
            columns_.emplace_back(column, resource);
        }
    }
    db_source_kind kind_{db_source_kind::table};
    std::pmr::string name_;
    std::pmr::string alias_;
    std::size_t expression_{no_db_node};
    std::size_t query_{no_db_node};
    bool lateral_{false};
    bool ordinality_{false};
    std::pmr::vector<db_stored_source_column> columns_;
};
struct db_stored_join final {
    db_stored_join(db_join_type type, db_query_source source_value, std::size_t on, std::pmr::memory_resource* resource)
        : type_(type),
          source_(std::move(source_value)),
          on_(on),
          using_columns_(resource) {}
    db_stored_join(const db_stored_join& source_value, std::pmr::memory_resource* resource)
        : type_(source_value.type_),
          source_(source_value.source_, resource),
          on_(source_value.on_),
          using_columns_(source_value.using_columns_, resource) {}
    db_join_type type_;
    db_query_source source_;
    std::size_t on_{no_db_node};
    std::pmr::vector<std::pmr::string> using_columns_;
};
struct db_stored_assignment final {
    db_stored_assignment(std::string_view column, std::size_t expression, std::pmr::memory_resource* resource)
        : column_(column, resource),
          expression_(expression) {}
    std::pmr::string column_;
    std::size_t expression_;
};
struct db_stored_cte final {
    db_stored_cte(std::string_view name, std::size_t query, const db_cte_options& options, std::pmr::memory_resource* resource)
        : name_(name, resource),
          query_(query),
          recursive_(options.recursive_),
          materialization_(options.materialization_),
          columns_(resource) {
        for (const auto& column : options.columns_) {
            columns_.emplace_back(column);
        }
    }
    db_stored_cte(const db_stored_cte& source_value, std::pmr::memory_resource* resource)
        : name_(source_value.name_, resource),
          query_(source_value.query_),
          recursive_(source_value.recursive_),
          materialization_(source_value.materialization_),
          columns_(source_value.columns_, resource) {}
    std::pmr::string name_;
    std::size_t query_;
    bool recursive_{false};
    db_materialization materialization_{db_materialization::default_value};
    std::pmr::vector<std::pmr::string> columns_;
};
struct db_stored_set_operation final {
    db_set_operation operation_;
    std::size_t query_;
};
struct db_stored_conflict final {
    explicit db_stored_conflict(std::pmr::memory_resource* resource)
        : columns_(resource),
          constraint_(resource),
          assignments_(resource) {}
    db_stored_conflict(const db_stored_conflict& source_value, std::pmr::memory_resource* resource)
        : columns_(source_value.columns_, resource),
          constraint_(source_value.constraint_, resource),
          target_where_(source_value.target_where_),
          assignments_(resource),
          update_where_(source_value.update_where_),
          do_nothing_(source_value.do_nothing_),
          any_unique_key_(source_value.any_unique_key_) {
        for (const auto& assignment : source_value.assignments_) {
            assignments_.emplace_back(assignment.column_, assignment.expression_, resource);
        }
    }
    std::pmr::vector<std::pmr::string> columns_;
    std::pmr::string constraint_;
    std::size_t target_where_{no_db_node};
    std::pmr::vector<db_stored_assignment> assignments_;
    std::size_t update_where_{no_db_node};
    bool do_nothing_{false};
    bool any_unique_key_{false};
};
struct db_stored_lock final {
    db_stored_lock(const db_lock_options& source_value, std::pmr::memory_resource* resource)
        : mode_(source_value.mode_),
          nowait_(source_value.nowait_),
          skip_locked_(source_value.skip_locked_),
          tables_(resource) {
        for (const auto& table : source_value.tables_) {
            tables_.emplace_back(table);
        }
    }
    db_stored_lock(const db_stored_lock& source_value, std::pmr::memory_resource* resource)
        : mode_(source_value.mode_),
          nowait_(source_value.nowait_),
          skip_locked_(source_value.skip_locked_),
          tables_(source_value.tables_, resource) {}
    db_row_lock mode_;
    bool nowait_{false};
    bool skip_locked_{false};
    std::pmr::vector<std::pmr::string> tables_;
};

class db_query_storage final {
public:
    explicit db_query_storage(std::pmr::memory_resource* resource)
        : resource_(resource),
          nodes_(resource),
          queries_(resource),
          target_(resource),
          target_alias_(resource),
          columns_(resource),
          projections_(resource),
          groups_(resource),
          orders_(resource),
          distinct_on_(resource),
          returning_(resource),
          joins_(resource),
          rows_(resource),
          assignments_(resource),
          ctes_(resource),
          set_operations_(resource),
          cache_id_(resource) {}

    std::pmr::memory_resource* resource_;
    std::pmr::vector<db_query_node> nodes_;
    std::pmr::vector<db_query> queries_;
    db_query_kind kind_{db_query_kind::select};
    std::pmr::string target_;
    std::pmr::string target_alias_;
    std::pmr::vector<std::pmr::string> columns_;
    std::pmr::vector<std::size_t> projections_;
    std::pmr::vector<std::size_t> groups_;
    std::pmr::vector<db_stored_order> orders_;
    bool distinct_{false};
    std::pmr::vector<std::size_t> distinct_on_;
    std::pmr::vector<std::size_t> returning_;
    std::optional<db_query_source> source_{};
    std::pmr::vector<db_stored_join> joins_;
    std::pmr::vector<std::pmr::vector<std::size_t>> rows_;
    std::pmr::vector<db_stored_assignment> assignments_;
    std::size_t insert_query_{no_db_node};
    std::size_t predicate_{no_db_node};
    std::size_t having_{no_db_node};
    std::optional<std::uint64_t> limit_{};
    std::optional<std::uint64_t> offset_{};
    std::optional<db_stored_conflict> conflict_{};
    std::optional<db_stored_lock> lock_{};
    std::pmr::vector<db_stored_cte> ctes_;
    std::pmr::vector<db_stored_set_operation> set_operations_;

    std::optional<bool> cache_enabled_{};
    std::optional<std::chrono::milliseconds> cache_duration_{};
    std::pmr::string cache_id_;

    [[nodiscard]] bool returns_rows() const noexcept {
        return kind_ == db_query_kind::select || kind_ == db_query_kind::values || !returning_.empty();
    }
};

void validate_query_shape(const db_query_storage& storage);

}  // namespace ruvia::detail
