#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_relation_metadata.h"
namespace ruvia {

// Whether a migration and the row recording it commit together.
//
// PostgreSQL runs DDL inside transactions, so wrapping both is what keeps an
// interrupted migration from being applied but unrecorded -- and re-applied on
// the next start. A few statements are refused inside a transaction block
// (CREATE INDEX CONCURRENTLY, VACUUM, ALTER TYPE ... ADD VALUE before 12), and
// those name the exception rather than forcing every migration to give up
// atomicity for one of them.
//
// MariaDB commits DDL implicitly whatever this says, so it applies to
// PostgreSQL only.
enum class db_migration_atomicity : std::uint8_t {
    transactional,
    unwrapped,
};

struct db_migration_options final {
    std::string id_{};
    std::string sql_{};
    db_migration_atomicity atomicity_{db_migration_atomicity::transactional};
};

// Immutable owning migration descriptor. Migrations are startup data, so the
// descriptor accepts temporary strings without imposing a hidden lifetime
// requirement on the asynchronous runner.
//
// `sql` is exactly one statement. Neither backend accepts more than one per
// call -- libpq's extended protocol refuses multiple commands and the MariaDB
// connection never enables CLIENT_MULTI_STATEMENTS -- so a schema change that
// needs several statements is several migrations. A trailing ';' is allowed.
// Comments follow the selected backend's syntax; PostgreSQL line comments end
// at either CR or LF.
//
// `id` identifies an applied migration for the rest of the schema's life. It is
// compared with the migrations table's collation, so ids that differ only in
// letter case are rejected up front rather than resolving differently per
// backend.
class db_migration final {
public:
    explicit db_migration(db_migration_options options)
        : id_(std::move(options.id_)),
          sql_(std::move(options.sql_)),
          atomicity_(options.atomicity_) {}

    [[nodiscard]] std::string_view id() const noexcept {
        return id_;
    }

    [[nodiscard]] std::string_view sql() const noexcept {
        return sql_;
    }

    [[nodiscard]] constexpr db_migration_atomicity atomicity() const noexcept {
        return atomicity_;
    }

private:
    std::string id_;
    std::string sql_;
    db_migration_atomicity atomicity_;
};

struct db_migrator_options final {
    borrowed_text table_{"ruvia_schema_migrations"};
    std::chrono::seconds lock_timeout_{30};
    std::pmr::memory_resource* resource_{nullptr};
};

class db_migration_report final {
public:
    db_migration_report(const db_migration_report&) = delete;
    db_migration_report& operator=(const db_migration_report&) = delete;
    db_migration_report(db_migration_report&&) noexcept = default;
    db_migration_report& operator=(db_migration_report&&) = delete;

    [[nodiscard]] std::span<const std::pmr::string> applied() const& noexcept;
    [[nodiscard]] std::span<const std::pmr::string> applied() const&& = delete;
    [[nodiscard]] std::span<const std::pmr::string> skipped() const& noexcept;
    [[nodiscard]] std::span<const std::pmr::string> skipped() const&& = delete;
    [[nodiscard]] bool changed() const noexcept;

private:
    friend class db_migrator;
    friend class detail::db_migration_runner;

    explicit db_migration_report(std::pmr::memory_resource* resource);
    db_migration_report(resolved_pmr_resource_tag, std::pmr::memory_resource* resource);

    std::pmr::vector<std::pmr::string> applied_;
    std::pmr::vector<std::pmr::string> skipped_;
};

// Applies pending migrations synchronously on the calling thread, holding a
// backend lock so that concurrent deployers serialize. It runs its own event
// loop and blocks until done, so it belongs in startup code, never on a worker.
//
// On PostgreSQL a migration and the row recording it commit together unless the
// migration opts out. MariaDB commits DDL implicitly, so there they are two
// statements and an interruption between them leaves the change applied and
// unrecorded, to be retried on the next run: write MariaDB migrations to be
// re-applicable -- CREATE TABLE IF NOT EXISTS and friends -- or guard them.
//
// The text of every applied migration is recorded as a digest. Editing one that
// has already run is reported instead of silently skipped, because the edit
// would otherwise reach only machines that had not migrated yet.
//
// db_config's timeouts apply here as they do on a worker: without connect_timeout
// or query_timeout_ a stalled backend blocks startup indefinitely.
// Migration locks and statements share one dedicated connection, independent
// of db_config::pool_size_per_worker_.
class db_migrator final {
public:
    // The configuration and migration-table name are copied into options.resource;
    // their source PMR storage may be released after construction. The supplied
    // resource itself must outlive this migrator.
    explicit db_migrator(const db_config& config, const db_migrator_options& options = {});
    ~db_migrator();

    db_migrator(const db_migrator&) = delete;
    db_migrator& operator=(const db_migrator&) = delete;
    db_migrator(db_migrator&&) noexcept;
    db_migrator& operator=(db_migrator&&) noexcept;

    [[nodiscard]] db_migration_report migrate(std::span<const db_migration> migrations) const;

    [[nodiscard]] static db_migration_report migrate(const db_config& config,
        std::span<const db_migration> migrations, const db_migrator_options& options = {});

private:
    class storage_type;
    struct storage_deleter_type final {
        std::pmr::memory_resource* resource_{nullptr};
        void operator()(storage_type* storage) const noexcept;
    };
    using storage_owner_type = std::unique_ptr<storage_type, storage_deleter_type>;

    [[nodiscard]] static storage_owner_type make_storage(
        const db_config& config, const db_migrator_options& options);

    storage_owner_type storage_;
};

}  // namespace ruvia

namespace ruvia {

class db_schema;

enum class db_table_lock_mode : std::uint8_t {
    access_share,
    row_share,
    row_exclusive,
    share_update_exclusive,
    share,
    share_row_exclusive,
    exclusive,
    access_exclusive,
};

struct db_variable_definition final {
    borrowed_text name_{};
    db_type_definition type_{};
    db_expression default_value_{};
};

// Structured PL/pgSQL for migration blocks and trigger functions. All inputs
// are consumed synchronously. The procedure owns its generated body in PMR.
class db_procedure final {
public:
    explicit db_procedure(std::pmr::memory_resource* resource = nullptr);
    db_procedure(const db_procedure&) = delete;
    db_procedure& operator=(const db_procedure&) = delete;
    db_procedure(db_procedure&&) noexcept = default;
    db_procedure& operator=(db_procedure&&) = delete;

    void declare_variable(const db_variable_definition& variable);
    void declare_row(std::string_view name, std::string_view table);
    void declare_record(std::string_view name);
    void assign(std::string_view target, db_expression value);
    void select_into(const db_query& query, std::span<const std::string_view> targets, bool strict = false);
    void execute(const db_query& query);
    void perform(db_expression expression);
    void apply(const db_schema& schema);
    void begin_if(db_expression condition);
    void else_if(db_expression condition);
    void otherwise();
    void end_if();
    void begin_block();
    void catch_sql_state(std::string_view state);
    void end_block();
    void begin_for_query(std::string_view target, const db_query& query);
    void begin_foreach(std::string_view target, db_expression array, unsigned slice = 0);
    void end_loop();
    void execute_dynamic(db_expression command, std::span<const db_expression> using_args = {});
    void execute_dynamic(db_expression command, std::initializer_list<db_expression> using_args) {
        execute_dynamic(command, std::span<const db_expression>(using_args.begin(), using_args.size()));
    }
    void lock_table(std::string_view table, db_table_lock_mode mode = db_table_lock_mode::access_exclusive, bool nowait = false);
    void return_value(db_expression expression = {});
    void raise_exception(std::string_view message, std::string_view sql_state = "P0001");
    [[nodiscard]] std::pmr::string body(std::pmr::memory_resource* resource = nullptr) const;

private:
    enum class block_type : std::uint8_t { if_value,
        else_value,
        begin,
        exception,
        loop };
    void register_variable(std::string_view name);
    void append_expression(std::pmr::string& output, db_expression value) const;
    std::pmr::memory_resource* resource_;
    std::pmr::string declarations_;
    std::pmr::string statements_;
    std::pmr::vector<std::pmr::string> variables_;
    std::pmr::vector<block_type> blocks_;
};

}  // namespace ruvia

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
enum class db_function_language : std::uint8_t { plpgsql,
    sql };
enum class db_function_volatility : std::uint8_t { volatile_value,
    stable,
    immutable };

struct db_function_parameter final {
    borrowed_text name_{};
    db_type_definition type_{};
    db_expression default_value_{};
};

struct db_function_definition final {
    borrowed_text name_{};
    std::pmr::vector<db_function_parameter> parameters_{};
    db_type_definition return_type_{};
    bool returns_set_{false};
    db_function_language language_{db_function_language::plpgsql};
    db_function_volatility volatility_{db_function_volatility::volatile_value};
    bool strict_{false};
    bool replace_{false};
    borrowed_text body_{};
};

struct db_trigger_definition final {
    borrowed_text name_{};
    borrowed_text table_{};
    borrowed_text function_{};
    db_trigger_timing timing_{db_trigger_timing::before};
    std::pmr::vector<db_trigger_event> events_{};
    bool for_each_row_{true};
    std::pmr::vector<borrowed_text> update_columns_{};
    borrowed_text old_table_{};
    borrowed_text new_table_{};
    db_expression when_{};
    std::pmr::vector<borrowed_text> arguments_{};
};

struct db_schema_column final {
    borrowed_text name_{};
    db_type_definition type_{};
    bool nullable_{false};
    bool primary_key_{false};
    bool unique_{false};
    db_identity identity_{db_identity::none};
    db_expression default_value_{};
    db_generated_type generated_type_{db_generated_type::none};
    db_expression as_expression_{};
};

struct db_schema_constraint final {
    borrowed_text name_{};
    db_constraint_kind kind_{db_constraint_kind::check};
    std::pmr::vector<borrowed_text> columns_{};
    borrowed_text referenced_table_{};
    std::pmr::vector<borrowed_text> referenced_columns_{};
    db_referential_action on_delete_{db_referential_action::no_action};
    db_referential_action on_update_{db_referential_action::no_action};
    bool deferrable_{false};
    bool initially_deferred_{false};
    bool not_valid_{false};
    db_expression check_{};
};

struct db_table_definition final {
    borrowed_text name_{};
    std::pmr::vector<db_schema_column> columns_{};
    std::pmr::vector<db_schema_constraint> constraints_{};
    bool if_not_exists_{false};
    bool temporary_{false};
    bool unlogged_{false};
};

struct db_index_key final {
    borrowed_text column_{};
    db_expression expression_{};
    db_order_direction order_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
    borrowed_text operator_class_{};
};

struct db_index_definition final {
    borrowed_text name_{};
    borrowed_text table_{};
    std::pmr::vector<db_index_key> keys_{};
    bool unique_{false};
    bool if_not_exists_{false};
    bool concurrently_{false};
    db_index_method method_{db_index_method::btree};
    std::pmr::vector<borrowed_text> include_{};
    db_expression where_{};
};

struct db_schema_options final {
    db_driver driver_{db_driver::unspecified};
    std::pmr::memory_resource* resource_{nullptr};
};

struct db_column_default final {
    borrowed_text column_{};
    db_expression value_{};
};
struct db_column_generated final {
    borrowed_text column_{};
    db_expression expression_{};
};
struct db_entity_table_options final {
    std::pmr::vector<db_column_default> defaults_{};
    std::pmr::vector<db_column_generated> generated_columns_{};
    std::pmr::vector<db_schema_constraint> constraints_{};
    bool if_not_exists_{false};
    bool temporary_{false};
    bool unlogged_{false};
};
struct db_table_option final {
    borrowed_text name_{};
    std::variant<bool, std::int64_t, borrowed_text> value_{false};
};
struct db_hypertable_options final {
    borrowed_text table_{};
    borrowed_text time_column_{};
    db_expression chunk_interval_{};
    bool create_default_indexes_{true};
    bool if_not_exists_{false};
    bool migrate_data_{false};
};
struct db_compression_order final {
    borrowed_text column_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};
struct db_compression_options final {
    bool enabled_{true};
    std::pmr::vector<borrowed_text> segment_by_{};
    std::pmr::vector<db_compression_order> order_by_{};
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
    void alter_column_type(std::string_view table, std::string_view column, const db_type_definition& type, db_expression using_value = {});
    void set_column_nullable(std::string_view table, std::string_view column, bool nullable);
    void set_column_default(std::string_view table, std::string_view column, db_expression expression);
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
    void perform(db_expression expression);
    void run(const db_procedure& procedure);
    void create_trigger_function(std::string_view name, const db_procedure& procedure, bool replace = false);
    void drop_trigger_function(std::string_view name, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void create_trigger(const db_trigger_definition& trigger);
    void drop_trigger(std::string_view table, std::string_view name, bool if_exists = false);
    void create_function(const db_function_definition& function);
    void create_function(const db_function_definition& function, const db_procedure& body);
    void create_function(const db_function_definition& function, const db_query& sql_body);
    void drop_function(std::string_view name, std::span<const db_type_definition> parameter_types = {},
        db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void lock_table(std::string_view table, db_table_lock_mode mode = db_table_lock_mode::access_exclusive, bool nowait = false);
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
    void append_type(std::pmr::string& sql, const db_type_definition& type) const;
    void append_column(std::pmr::string& sql, const db_schema_column& column) const;
    void append_constraint(std::pmr::string& sql, const db_schema_constraint& constraint) const;
    void append_expression(std::pmr::string& sql, db_expression expression) const;
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
    db_table_definition table_value{.name_ = entity_type::table_name(), .columns_ = std::pmr::vector<db_schema_column>(resource_), .constraints_ = std::pmr::vector<db_schema_constraint>(options.constraints_.begin(), options.constraints_.end(), resource_), .if_not_exists_ = options.if_not_exists_, .temporary_ = options.temporary_, .unlogged_ = options.unlogged_};
    db_query defaults(resource_);
    std::pmr::vector<borrowed_text> primary(resource_);
    bool conflicting_generation = false;
    bool conflicting_read_only = false;
    detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
        conflicting_generation |= c_type::options.generated_ && c_type::options.generated_type_ != db_generated_type::none;
        conflicting_read_only |= c_type::options.read_only_ && (c_type::options.generated_ || c_type::options.generated_type_ != db_generated_type::none);
        db_schema_column column{.name_ = c_type::name.view(),
            .type_ = entity_column_type<c_type>(),
            .nullable_ = c_type::options.nullable_ || c_type::options.delete_date_,
            .generated_type_ = c_type::options.generated_type_};
        if constexpr (!c_type::options.enum_name_.view().empty()) {
            if (driver_ != db_driver::postgresql) {
                throw std::invalid_argument("entity enum_name requires PostgreSQL");
            }
        }
        if constexpr (!c_type::options.default_expression_.view().empty()) {
            column.default_value_ = defaults.sql(c_type::options.default_expression_.view());
        } else if constexpr (c_type::options.create_date_ || c_type::options.update_date_) {
            column.default_value_ = defaults.call("now");
        } else if constexpr (c_type::options.version_) {
            column.default_value_ = defaults.value(1);
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
    });
    if (conflicting_generation) {
        throw std::invalid_argument("computed columns cannot also request generated identity values");
    }
    if (conflicting_read_only) {
        throw std::invalid_argument("read-only columns cannot also request generated values");
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
    std::pmr::vector<std::pmr::string> generated_names(resource_);
    generated_names.reserve(1 + std::tuple_size_v<typename entity_type::relations_type> * 2);
    if (!primary.empty()) {
        const auto dot = entity_type::table_name().rfind('.');
        const auto local = entity_type::table_name().substr(dot == std::string_view::npos ? 0 : dot + 1);
        auto& pkey_name = generated_names.emplace_back(local);
        pkey_name += "_pkey";
        table_value.constraints_.push_back({.name_ = pkey_name, .kind_ = db_constraint_kind::primary_key, .columns_ = std::move(primary)});
    }
    detail::for_each_descriptor<typename entity_type::relations_type>([&]<typename r_type, std::size_t> {
        if constexpr (!r_type::is_collection && r_type::is_owning) {
            detail::validate_db_relation<entity_type, r_type>();
            using mapping_type = detail::db_relation_mapping<entity_type, r_type>;
            using target_type = typename r_type::target_entity_type;
            const auto local_table = entity_type::table_name().substr(entity_type::table_name().rfind('.') == std::string_view::npos ? 0 : entity_type::table_name().rfind('.') + 1);
            auto& fkey_name = generated_names.emplace_back(local_table);
            fkey_name.push_back('_');
            fkey_name.append(r_type::name.view());
            fkey_name.append("_fkey");
            db_schema_constraint fk{.name_ = fkey_name, .kind_ = db_constraint_kind::foreign_key, .columns_ = std::pmr::vector<borrowed_text>(resource_), .referenced_table_ = target_type::table_name(), .referenced_columns_ = std::pmr::vector<borrowed_text>(resource_)};
            detail::for_each_descriptor<typename mapping_type::join_columns_type>([&]<typename j_type, std::size_t> { fk.columns_.emplace_back(j_type::local.view()); fk.referenced_columns_.emplace_back(j_type::referenced.view()); });
            table_value.constraints_.push_back(std::move(fk));
            if constexpr (r_type::kind == db_relation_kind::one_to_one) {
                auto& key_name = generated_names.emplace_back(local_table);
                key_name.push_back('_');
                key_name.append(r_type::name.view());
                key_name.append("_key");
                table_value.constraints_.push_back({.name_ = key_name, .kind_ = db_constraint_kind::unique, .columns_ = table_value.constraints_.back().columns_});
            }
        }
    });
    create_table(table_value);
}

template <typename entity_type>
void db_schema::create_relation_tables() {
    detail::for_each_descriptor<typename entity_type::relations_type>([&]<typename r_type, std::size_t> {
        if constexpr (r_type::kind == db_relation_kind::many_to_many && r_type::is_owning) {
            detail::validate_db_relation<entity_type, r_type>();
            using m_type = detail::db_relation_mapping<entity_type, r_type>;
            using target_type = typename r_type::target_entity_type;
            db_table_definition table_value{.name_ = m_type::table_name(), .columns_ = std::pmr::vector<db_schema_column>(resource_), .constraints_ = std::pmr::vector<db_schema_constraint>(resource_)};
            const auto dot = m_type::table_name().rfind('.');
            const auto local_table = m_type::table_name().substr(dot == std::string_view::npos ? 0 : dot + 1);
            std::pmr::string owner_fk_name(local_table, resource_);
            owner_fk_name += "_owner_fkey";
            std::pmr::string inverse_fk_name(local_table, resource_);
            inverse_fk_name += "_inverse_fkey";
            std::pmr::string pkey_name(local_table, resource_);
            pkey_name += "_pkey";
            const auto add_side = [&]<typename referenced_entity_type, typename columns_type>(const std::pmr::string& fk_name) {
                db_schema_constraint fk{.name_ = fk_name,
                    .kind_ = db_constraint_kind::foreign_key,
                    .columns_ = std::pmr::vector<borrowed_text>(resource_),
                    .referenced_table_ = referenced_entity_type::table_name(),
                    .referenced_columns_ = std::pmr::vector<borrowed_text>(resource_)};
                detail::for_each_descriptor<columns_type>([&]<typename j_type, std::size_t> {
                    using c_type = std::tuple_element_t<referenced_entity_type::template column_index<j_type::referenced>(), typename referenced_entity_type::columns_type>;
                    table_value.columns_.push_back({.name_ = j_type::local.view(), .type_ = entity_column_type<c_type>(), .nullable_ = false});
                    fk.columns_.emplace_back(j_type::local.view());
                    fk.referenced_columns_.emplace_back(j_type::referenced.view());
                });
                table_value.constraints_.push_back(std::move(fk));
            };
            add_side.template operator()<entity_type, typename m_type::source_columns_type>(owner_fk_name);
            add_side.template operator()<target_type, typename m_type::target_columns_type>(inverse_fk_name);
            std::pmr::vector<borrowed_text> primary(resource_);
            primary.reserve(table_value.columns_.size());
            for (const auto& column : table_value.columns_) {
                primary.push_back(column.name_);
            }
            table_value.constraints_.push_back({.name_ = pkey_name, .kind_ = db_constraint_kind::primary_key, .columns_ = std::move(primary)});
            create_table(table_value);
        }
    });
}

}  // namespace ruvia
