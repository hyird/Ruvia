#pragma once

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_migration.h"
#include "ruvia/web/db/db_procedure.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_relation_metadata.h"

namespace ruvia {

enum class db_identity : std::uint8_t { none,
    by_default,
    always };
enum class db_referential_action : std::uint8_t { no_action,
    restrict,
    cascade,
    set_null,
    set_default };
enum class db_constraint_kind : std::uint8_t { primary_key,
    unique,
    foreign_key,
    check };
enum class db_drop_behavior : std::uint8_t { restrict,
    cascade };
enum class db_index_method : std::uint8_t { btree,
    hash,
    gin,
    gist,
    sp_gist,
    brin };
enum class db_trigger_timing : std::uint8_t { before,
    after,
    instead_of };
enum class db_trigger_event : std::uint8_t { insert,
    update,
    delete_value,
    truncate };

struct db_trigger_definition final {
    std::string name_{};
    std::string table_{};
    std::string function_{};
    db_trigger_timing timing_{db_trigger_timing::before};
    std::vector<db_trigger_event> events_{};
    bool for_each_row_{true};
    std::vector<std::string> update_columns_{};
    db_expression when_{};
    std::vector<std::string> arguments_{};
};

struct db_schema_column final {
    std::string name_{};
    db_type_definition type_{};
    bool nullable_{false};
    bool primary_key_{false};
    bool unique_{false};
    db_identity identity_{db_identity::none};
    db_query::expr_type default_value_{};
    db_generated_type generated_type_{db_generated_type::none};
    db_expression as_expression_{};
};

struct db_schema_constraint final {
    std::string name_{};
    db_constraint_kind kind_{db_constraint_kind::check};
    std::vector<std::string> columns_{};
    std::string referenced_table_{};
    std::vector<std::string> referenced_columns_{};
    db_referential_action on_delete_{db_referential_action::no_action};
    db_referential_action on_update_{db_referential_action::no_action};
    bool deferrable_{false};
    bool initially_deferred_{false};
    bool not_valid_{false};
    db_query::expr_type check_{};
};

struct db_table_definition final {
    std::string name_{};
    std::vector<db_schema_column> columns_{};
    std::vector<db_schema_constraint> constraints_{};
    bool if_not_exists_{false};
    bool temporary_{false};
    bool unlogged_{false};
};

struct db_index_key final {
    std::string column_{};
    db_query::expr_type expression_{};
    db_order_direction order_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
    std::string operator_class_{};
};

struct db_index_definition final {
    std::string name_{};
    std::string table_{};
    std::vector<db_index_key> keys_{};
    bool unique_{false};
    bool if_not_exists_{false};
    bool concurrently_{false};
    db_index_method method_{db_index_method::btree};
    std::vector<std::string> include_{};
    db_query::expr_type where_{};
};

struct db_schema_options final {
    db_driver driver_{db_driver::unspecified};
    std::pmr::memory_resource* resource_{nullptr};
};

struct db_column_default final {
    std::string column_{};
    db_expression value_{};
};
struct db_column_generated final {
    std::string column_{};
    db_expression expression_{};
};
struct db_entity_table_options final {
    std::vector<db_column_default> defaults_{};
    std::vector<db_column_generated> generated_columns_{};
    std::vector<db_schema_constraint> constraints_{};
    bool if_not_exists_{false};
    bool temporary_{false};
    bool unlogged_{false};
};
struct db_table_option final {
    std::string name_{};
    std::variant<bool, std::int64_t, std::string> value_{false};
};
struct db_hypertable_options final {
    std::string table_{};
    std::string time_column_{};
    db_expression chunk_interval_{};
    bool create_default_indexes_{true};
    bool if_not_exists_{false};
    bool migrate_data_{false};
};
struct db_compression_order final {
    std::string column_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};
struct db_compression_options final {
    bool enabled_{true};
    std::vector<std::string> segment_by_{};
    std::vector<db_compression_order> order_by_{};
};

// Explicit, versioned schema changes. Each operation consumes its borrowed
// expressions synchronously and stores only generated, owning SQL. Nothing
// here connects to a database or synchronizes a running application's schema.
class db_schema final {
public:
    explicit db_schema(db_schema_options options);
    db_schema(const db_schema&) = delete;
    db_schema& operator=(const db_schema&) = delete;
    db_schema(db_schema&&) noexcept = default;
    db_schema& operator=(db_schema&&) = delete;

    void create_schema(std::string_view name, bool if_not_exists = false);
    void drop_schema(std::string_view name, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void create_table(const db_table_definition& table);
    template <typename entity_type>
    void create_table(const db_entity_table_options& options = {});
    template <typename entity_type>
    void create_relation_tables();
    void drop_table(std::string_view table, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void rename_table(std::string_view table, std::string_view name);
    void add_column(std::string_view table, const db_schema_column& column, bool if_not_exists = false);
    void drop_column(std::string_view table, std::string_view column, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void rename_column(std::string_view table, std::string_view column, std::string_view name);
    void alter_column_type(std::string_view table, std::string_view column, const db_type_definition& type, db_query::expr_type using_value = {});
    void set_column_nullable(std::string_view table, std::string_view column, bool nullable);
    void set_column_default(std::string_view table, std::string_view column, db_query::expr_type expression);
    void drop_column_default(std::string_view table, std::string_view column);
    void add_constraint(std::string_view table, const db_schema_constraint& constraint);
    void drop_constraint(std::string_view table, std::string_view name, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void validate_constraint(std::string_view table, std::string_view name);
    void create_index(const db_index_definition& index);
    void drop_index(std::string_view name, bool concurrently = false, bool if_exists = false);
    void create_enum(std::string_view name, std::span<const std::string_view> values);
    void add_enum_value(std::string_view name, std::string_view value, bool if_not_exists = false);
    void drop_enum(std::string_view name, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void create_extension(std::string_view name, bool if_not_exists = true);
    void create_view(std::string_view name, const db_query& query, bool replace = false, bool materialized = false);
    void drop_view(std::string_view name, bool materialized = false, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void execute(const db_query& query);
    void perform(db_query::expr_type expression);
    void run(const db_procedure& procedure);
    void create_trigger_function(std::string_view name, const db_procedure& procedure, bool replace = false);
    void drop_trigger_function(std::string_view name, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void create_trigger(const db_trigger_definition& trigger);
    void drop_trigger(std::string_view table, std::string_view name, bool if_exists = false);
    void set_table_options(std::string_view table, std::span<const db_table_option> options);
    void set_database_option(std::string_view database, std::string_view name, std::string_view value);
    void create_hypertable(const db_hypertable_options& options);
    void set_chunk_time_interval(std::string_view table, db_expression interval);
    void set_compression(std::string_view table, const db_compression_options& options);
    void add_compression_policy(std::string_view table, db_expression after, bool if_not_exists = false);
    void remove_compression_policy(std::string_view table, bool if_exists = false);
    void add_retention_policy(std::string_view table, db_expression after, bool if_not_exists = false);
    void remove_retention_policy(std::string_view table, bool if_exists = false);

    // PostgreSQL batches are one atomic DO statement. A nontransactional
    // operation must be the batch's only operation. MariaDB DDL has implicit
    // commits, so each generated statement receives a stable numbered id.
    [[nodiscard]] std::pmr::vector<db_migration> compile(std::string_view id) const;
    [[nodiscard]] db_driver driver() const noexcept {
        return driver_;
    }

private:
    friend class db_procedure;
    struct statement_type final {
        std::pmr::string sql_;
        db_migration_atomicity atomicity_{db_migration_atomicity::transactional};
        bool procedural_{false};
    };
    void append(std::pmr::string sql, db_migration_atomicity atomicity = db_migration_atomicity::transactional, bool procedural = false);
    void require_postgresql() const;
    [[nodiscard]] std::pmr::string table_prefix(std::string_view table) const;
    void append_type(std::pmr::string& sql, const db_type_definition& type) const;
    void append_column(std::pmr::string& sql, const db_schema_column& column) const;
    void append_constraint(std::pmr::string& sql, const db_schema_constraint& constraint) const;
    void append_expression(std::pmr::string& sql, db_query::expr_type expression) const;
    template <typename column_type>
    static db_type_definition entity_column_type() {
        db_type_definition type{.data_type_ = column_type::data_type, .length_ = column_type::options.length_, .precision_ = column_type::options.precision_, .scale_ = column_type::options.scale_};
        if constexpr (detail::is_pmr_vector<typename column_type::value_type>::value) {
            using item_type = typename detail::is_pmr_vector<typename column_type::value_type>::value_type;
            if constexpr (column_type::options.data_type_ == db_data_type::inferred || column_type::options.data_type_ == db_data_type::array) {
                type.data_type_ = detail::db_entity_type_traits<item_type>::data_type;
            }
            type.array_ = true;
        }
        if constexpr (!column_type::options.enum_name_.view().empty()) {
            if constexpr (column_type::options.data_type_ != db_data_type::inferred && column_type::options.data_type_ != db_data_type::array) {
                throw std::invalid_argument("enum_name cannot also specify a scalar data_type");
            }
            type.custom_name_ = column_type::options.enum_name_.view();
            type.data_type_ = db_data_type::inferred;
        }
        return type;
    }

    db_driver driver_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<statement_type> statements_;
};

template <typename entity_type>
void db_schema::create_table(const db_entity_table_options& options) {
    db_table_definition table_value{.name_ = std::string(entity_type::table_name()), .constraints_ = options.constraints_, .if_not_exists_ = options.if_not_exists_, .temporary_ = options.temporary_, .unlogged_ = options.unlogged_};
    db_query defaults(resource_);
    std::vector<std::string> primary;
    bool conflicting_generation = false;
    [&]<std::size_t... i>(std::index_sequence<i...>) {
        const auto add = [&]<typename c_type> {
            conflicting_generation |= c_type::options.generated_ && c_type::options.generated_type_ != db_generated_type::none;
            db_schema_column column{.name_ = std::string(c_type::name.view()),
                .type_ = entity_column_type<c_type>(),
                .nullable_ = c_type::options.nullable_,
                .generated_type_ = c_type::options.generated_type_};
            if constexpr (!c_type::options.enum_name_.view().empty()) {
                if (driver_ != db_driver::postgresql) {
                    throw std::invalid_argument("entity enum_name requires PostgreSQL");
                }
            }
            if constexpr (!c_type::options.default_expression_.view().empty()) {
                column.default_value_ = defaults.sql(c_type::options.default_expression_.view());
            }
            if constexpr (c_type::options.primary_key_) {
                primary.push_back(column.name_);
            }
            if constexpr (c_type::options.generated_ && (c_type::data_type == db_data_type::small_int || c_type::data_type == db_data_type::integer || c_type::data_type == db_data_type::big_int)) {
                column.identity_ = db_identity::by_default;
            }
            for (const auto& value : options.generated_columns_) {
                if (value.column_ == column.name_) {
                    if (!column.as_expression_.empty()) {
                        throw std::invalid_argument("duplicate entity generated column");
                    }
                    column.as_expression_ = value.expression_;
                }
            }
            for (const auto& value : options.defaults_) {
                if (value.column_ == column.name_) {
                    if (!column.default_value_.empty()) {
                        throw std::invalid_argument("duplicate entity column default");
                    }
                    column.default_value_ = value.value_;
                }
            }
            table_value.columns_.push_back(std::move(column));
        };
        (add.template operator()<std::tuple_element_t<i, typename entity_type::columns_type>>(), ...);
    }(std::make_index_sequence<std::tuple_size_v<typename entity_type::columns_type>>{});
    if (conflicting_generation) {
        throw std::invalid_argument("computed columns cannot also request generated identity values");
    }
    for (const auto& value : options.generated_columns_) {
        bool found = false;
        for (const auto& column : table_value.columns_) {
            if (column.name_ == value.column_) {
                found = true;
                if (value.expression_.empty()) {
                    throw std::invalid_argument("entity generated column requires an expression");
                }
                if (column.generated_type_ == db_generated_type::none) {
                    throw std::invalid_argument("entity generated column requires generated metadata");
                }
            }
        }
        if (!found) {
            throw std::invalid_argument("entity generated column requires a known column");
        }
    }
    for (auto& column : table_value.columns_) {
        if (column.generated_type_ != db_generated_type::none) {
            if (column.as_expression_.empty()) {
                throw std::invalid_argument("generated column requires an expression");
            }
            if (column.identity_ != db_identity::none || !column.default_value_.empty()) {
                throw std::invalid_argument("generated column conflicts with identity or default");
            }
        }
    }
    for (const auto& value : options.defaults_) {
        bool found = false;
        for (const auto& column : table_value.columns_) {
            found |= column.name_ == value.column_;
        }
        if (!found || value.value_.empty()) {
            throw std::invalid_argument("entity default requires a known column and an expression");
        }
    }
    if (!primary.empty()) {
        const auto dot = entity_type::table_name().rfind('.');
        const auto local = entity_type::table_name().substr(dot == std::string_view::npos ? 0 : dot + 1);
        table_value.constraints_.push_back({.name_ = std::string(local) + "_pkey", .kind_ = db_constraint_kind::primary_key, .columns_ = std::move(primary)});
    }
    detail::for_each_db_descriptor<typename entity_type::relations_type>([&]<typename r_type, std::size_t> {
        if constexpr (!r_type::is_collection && r_type::is_owning) {
            detail::validate_db_relation<entity_type, r_type>();
            using mapping_type = detail::db_relation_mapping<entity_type, r_type>;
            using target_type = typename r_type::target_entity_type;
            const auto local_table = entity_type::table_name().substr(entity_type::table_name().rfind('.') == std::string_view::npos ? 0 : entity_type::table_name().rfind('.') + 1);
            const auto prefix = std::string(local_table) + "_" + std::string(r_type::name.view());
            db_schema_constraint fk{.name_ = prefix + "_fkey", .kind_ = db_constraint_kind::foreign_key, .referenced_table_ = std::string(target_type::table_name())};
            detail::for_each_db_descriptor<typename mapping_type::join_columns_type>([&]<typename j_type, std::size_t> { fk.columns_.emplace_back(j_type::local.view()); fk.referenced_columns_.emplace_back(j_type::referenced.view()); });
            table_value.constraints_.push_back(std::move(fk));
            if constexpr (r_type::kind == db_relation_kind::one_to_one) {
                table_value.constraints_.push_back({.name_ = prefix + "_key", .kind_ = db_constraint_kind::unique, .columns_ = table_value.constraints_.back().columns_});
            }
        }
    });
    create_table(table_value);
}

template <typename entity_type>
void db_schema::create_relation_tables() {
    detail::for_each_db_descriptor<typename entity_type::relations_type>([&]<typename r_type, std::size_t> {
        if constexpr (r_type::kind == db_relation_kind::many_to_many && r_type::is_owning) {
            detail::validate_db_relation<entity_type, r_type>();
            using m_type = detail::db_relation_mapping<entity_type, r_type>;
            using target_type = typename r_type::target_entity_type;
            db_table_definition table_value{.name_ = std::string(m_type::table_name())};
            const auto dot = m_type::table_name().rfind('.');
            const auto local_table = std::string(m_type::table_name().substr(dot == std::string_view::npos ? 0 : dot + 1));
            const auto add_side = [&]<typename referenced_entity_type, typename columns_type>(std::string_view side) {
                db_schema_constraint fk{.name_ = local_table + "_" + std::string(side) + "_fkey",
                    .kind_ = db_constraint_kind::foreign_key,
                    .referenced_table_ = std::string(referenced_entity_type::table_name())};
                detail::for_each_db_descriptor<columns_type>([&]<typename j_type, std::size_t> {
                    using c_type = std::tuple_element_t<referenced_entity_type::template column_index<j_type::referenced>(), typename referenced_entity_type::columns_type>;
                    table_value.columns_.push_back({.name_ = std::string(j_type::local.view()), .type_ = entity_column_type<c_type>(), .nullable_ = false});
                    fk.columns_.emplace_back(j_type::local.view());
                    fk.referenced_columns_.emplace_back(j_type::referenced.view());
                });
                table_value.constraints_.push_back(std::move(fk));
            };
            add_side.template operator()<entity_type, typename m_type::source_columns_type>("owner");
            add_side.template operator()<target_type, typename m_type::target_columns_type>("inverse");
            std::vector<std::string> primary;
            for (const auto& column : table_value.columns_) {
                primary.push_back(column.name_);
            }
            table_value.constraints_.push_back({.name_ = local_table + "_pkey", .kind_ = db_constraint_kind::primary_key, .columns_ = std::move(primary)});
            create_table(table_value);
        }
    });
}

}  // namespace ruvia
