#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_session.h"

namespace ruvia::detail {

template <typename entity_type, fixed_string name>
struct db_relation_named final {
    static constexpr auto index = descriptor_index<typename entity_type::relations_type, name>();
    static_assert(index < std::tuple_size_v<typename entity_type::relations_type>, "unknown inverse database relation");
    using type = std::tuple_element_t<index, typename entity_type::relations_type>;
};

template <typename tuple_type>
struct reverse_db_join_columns;
template <typename... columns_type>
struct reverse_db_join_columns<std::tuple<columns_type...>> final {
    using type = std::tuple<db_join_column<columns_type::referenced, columns_type::local>...>;
};

// An owning one-to-one relation stores its join columns exactly like an owning
// many-to-one relation, so both normalize to the same mapping.
template <typename relation_type>
consteval db_relation_kind db_relation_mapping_kind() noexcept {
    return relation_type::kind == db_relation_kind::one_to_one && relation_type::is_owning
               ? db_relation_kind::many_to_one
               : relation_type::kind;
}

// One normalized mapping is consumed by schema generation and query planning.
// Target metadata is deliberately inspected only when a mapping is consumed,
// so mutually recursive and self-referential entity declarations stay valid.
template <typename source, typename relation_type,
    db_relation_kind kind = db_relation_mapping_kind<relation_type>(), bool owning = relation_type::is_owning>
struct db_relation_mapping;

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::many_to_one, true> {
    static constexpr bool through_join_table = false;
    using join_columns_type = typename relation_type::join_columns_type;
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::one_to_many, false> {
    using inverse_type = typename db_relation_named<typename relation_type::target_entity_type, relation_type::inverse_name>::type;
    static_assert(inverse_type::kind == db_relation_kind::many_to_one && inverse_type::is_owning,
        "one-to-many must refer to an owning many-to-one relation");
    static_assert(std::is_same_v<typename inverse_type::target_entity_type, source>,
        "inverse database relation must target its source entity");
    static constexpr bool through_join_table = false;
    using join_columns_type = typename reverse_db_join_columns<typename inverse_type::join_columns_type>::type;
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::one_to_one, false> {
    using inverse_type = typename db_relation_named<typename relation_type::target_entity_type, relation_type::inverse_name>::type;
    static_assert(inverse_type::kind == db_relation_kind::one_to_one && inverse_type::is_owning,
        "inverse one-to-one must refer to an owning one-to-one relation");
    static_assert(std::is_same_v<typename inverse_type::target_entity_type, source>,
        "inverse database relation must target its source entity");
    static constexpr bool through_join_table = false;
    using join_columns_type = typename reverse_db_join_columns<typename inverse_type::join_columns_type>::type;
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::many_to_many, true> {
    static constexpr bool through_join_table = true;
    using table = typename relation_type::join_table_type;
    using source_columns_type = typename table::owner_columns_type;
    using target_columns_type = typename table::inverse_columns_type;
    static constexpr std::string_view table_name() noexcept {
        return table::name.view();
    }
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::many_to_many, false> {
    using inverse_type = typename db_relation_named<typename relation_type::target_entity_type, relation_type::inverse_name>::type;
    static_assert(inverse_type::kind == db_relation_kind::many_to_many && inverse_type::is_owning,
        "inverse many-to-many must refer to an owning many-to-many relation");
    static_assert(std::is_same_v<typename inverse_type::target_entity_type, source>,
        "inverse database relation must target its source entity");
    static constexpr bool through_join_table = true;
    using table = typename inverse_type::join_table_type;
    using source_columns_type = typename table::inverse_columns_type;
    using target_columns_type = typename table::owner_columns_type;
    static constexpr std::string_view table_name() noexcept {
        return table::name.view();
    }
};

template <typename columns_type>
consteval bool unique_db_join_columns() {
    constexpr auto count = std::tuple_size_v<columns_type>;
    std::array<std::string_view, count> local{}, referenced{};
    for_each_descriptor<columns_type>([&]<typename column_type, std::size_t i> {
        local[i] = column_type::local.view();
        referenced[i] = column_type::referenced.view();
    });
    for (std::size_t i = 0; i < count; ++i) {
        if (local[i].empty() || referenced[i].empty()) {
            return false;
        }
        for (std::size_t j = i + 1; j < count; ++j) {
            if (local[i] == local[j] || referenced[i] == referenced[j]) {
                return false;
            }
        }
    }
    return true;
}

template <typename source, typename relation_type>
constexpr void validate_db_relation() {
    using target_type = typename relation_type::target_entity_type;
    using mapping_type = db_relation_mapping<source, relation_type>;
    if constexpr (mapping_type::through_join_table) {
        using owner = typename mapping_type::source_columns_type;
        using other_type = typename mapping_type::target_columns_type;
        static_assert(std::tuple_size_v<owner> > 0 && std::tuple_size_v<other_type> > 0,
            "a join table requires both owner and inverse columns");
        static_assert(unique_db_join_columns<owner>() && unique_db_join_columns<other_type>(),
            "join table column mappings must be nonempty and unique");
        static_assert([] {
            bool unique = true;
            for_each_descriptor<owner>([&]<typename left_type, std::size_t> {
                for_each_descriptor<other_type>([&]<typename right_type, std::size_t> {
                    unique &= left_type::local.view() != right_type::local.view();
                });
            });
            return unique;
        }(),
            "join table owner and inverse columns must have distinct names");
        for_each_descriptor<owner>([]<typename column_type, std::size_t> {
            (void)source::template column_index<column_type::referenced>();
        });
        for_each_descriptor<other_type>([]<typename column_type, std::size_t> {
            (void)target_type::template column_index<column_type::referenced>();
        });
    } else {
        using columns_type = typename mapping_type::join_columns_type;
        static_assert(std::tuple_size_v<columns_type> > 0, "a database relation requires join columns");
        static_assert(unique_db_join_columns<columns_type>(), "database relation column mappings must be nonempty and unique");
        for_each_descriptor<columns_type>([]<typename column_type, std::size_t> {
            (void)source::template column_index<column_type::local>();
            (void)target_type::template column_index<column_type::referenced>();
        });
    }
}

template <typename entity_type>
consteval std::size_t db_primary_key_count() {
    std::size_t count = 0;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        count += column_type::options.primary_key_ ? 1 : 0;
    });
    return count;
}

template <typename entity_type>
consteval auto db_primary_key_columns() {
    std::array<std::string_view, db_primary_key_count<entity_type>()> result_value{};
    std::size_t i = 0;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        if constexpr (column_type::options.primary_key_) {
            result_value[i++] = column_type::name.view();
        }
    });
    return result_value;
}

}  // namespace ruvia::detail

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

    db_migration(const db_migration& other)
        : id_(other.id_),
          sql_(other.sql_),
          atomicity_(other.atomicity_) {}
    db_migration(db_migration&&) = default;
    db_migration& operator=(const db_migration&) = default;
    db_migration& operator=(db_migration&&) = default;

    [[nodiscard]] std::string_view id() const& noexcept RUVIA_LIFETIMEBOUND {
        return id_;
    }
    std::string_view id() const&& = delete;

    [[nodiscard]] std::string_view sql() const& noexcept RUVIA_LIFETIMEBOUND {
        return sql_;
    }
    std::string_view sql() const&& = delete;

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
    // Positive lock wait; integer counts mean milliseconds. MariaDB rounds up
    // to whole seconds; PostgreSQL rejects values beyond its int32 millisecond range.
    timeout_duration lock_timeout_{std::chrono::seconds(30)};
};

class db_migration_report final {
public:
    db_migration_report(const db_migration_report&) = delete;
    db_migration_report& operator=(const db_migration_report&) = delete;
    db_migration_report(db_migration_report&&) noexcept = default;
    db_migration_report& operator=(db_migration_report&& other);

    [[nodiscard]] std::span<const std::string> applied() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::string> applied() const&& = delete;
    [[nodiscard]] std::span<const std::string> skipped() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::string> skipped() const&& = delete;
    [[nodiscard]] bool changed() const noexcept;

private:
    friend class db_migrator;
    friend class detail::db_migration_runner;

    db_migration_report() noexcept;

    std::vector<std::string> applied_;
    std::vector<std::string> skipped_;
};

// Applies pending migrations on the caller's event loop, holding a backend lock
// so that concurrent deployers serialize. Like db_client it borrows the loop's
// io_context and blocking pool (for a DNS host); it opens one dedicated
// connection per run, independent of any pool and of
// db_config::pool_size_per_worker_, that carries the lock and every statement.
//
// On PostgreSQL a migration and the row recording it commit together unless the
// migration opts out. MariaDB commits DDL implicitly, so there they are two
// statements and an interruption between them leaves the change applied and
// unrecorded, to be retried on the next run: write MariaDB migrations to be
// re-applicable -- CREATE TABLE IF NOT EXISTS and friends -- or guard them.
//
// Every applied migration must have a recorded SHA-256 digest matching its SQL.
// Missing columns, NULL/empty digests and mismatches are rejected, never adopted
// from the current source. Upgrade legacy history only after independently
// verifying the SQL actually applied; then record that SQL's digest explicitly.
//
// db_config's timeouts apply to each step: without connect_timeout or
// query_timeout_ a stalled backend blocks the run indefinitely unless the
// run's own operation_options bound it.
class db_migrator final {
public:
    // The configuration and migration-table name are copied into options.resource;
    // their source PMR storage may be released after construction. The supplied
    // resource itself must outlive this migrator and its runs. An invalid loop,
    // or a DNS host on a loop without a blocking pool, throws
    // std::invalid_argument.
    db_migrator(event_loop loop, const db_config& config, const db_migrator_options& options = {});
    ~db_migrator();

    db_migrator(const db_migrator&) = delete;
    db_migrator& operator=(const db_migrator&) = delete;
    db_migrator(db_migrator&&) noexcept;
    db_migrator& operator=(db_migrator&&) noexcept;

    // Validates and copies the migration list before returning the cold task,
    // which owns every input and must run on the bound loop (std::logic_error
    // otherwise). `operation` bounds the whole run, connecting included; a
    // non-positive timeout throws std::invalid_argument at the call and an
    // already-stopped token throws db_error(cancelled) before connecting. Its
    // expiry or stop request closes the dedicated connection, which releases
    // the lock, and throws db_error(timeout / cancelled); a migration already
    // committed stays recorded.
    [[nodiscard]] task<db_migration_report> migrate(
        std::span<const db_migration> migrations, operation_options operation = {}) const&;
    task<db_migration_report> migrate(std::span<const db_migration>, operation_options = {}) const&& = delete;

private:
    class storage_type;
    struct storage_deleter_type final {
        void operator()(storage_type* storage) const noexcept;
    };
    using storage_owner_type = std::unique_ptr<storage_type, storage_deleter_type>;

    [[nodiscard]] static storage_owner_type make_storage(
        event_loop loop, const db_config& config, const db_migrator_options& options);

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
// Failed begin_block() and apply() leave the prior body and control stack
// unchanged, so allocation failures may be caught and the operation retried.
class db_procedure final {
public:
    db_procedure() noexcept;
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
    [[nodiscard]] std::string body() const;

private:
    enum class block_type : std::uint8_t { if_value,
        else_value,
        begin,
        exception,
        loop };
    // Declarations publish the name only after their text is appended, so a
    // failed declaration leaves no variable behind.
    void require_unique_variable(std::string_view name) const;
    void append_expression(std::string& output, db_expression value) const;
    std::string declarations_;
    std::string statements_;
    std::vector<std::string> variables_;
    std::vector<block_type> blocks_;
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
    std::vector<db_function_parameter> parameters_{};
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
    std::vector<db_trigger_event> events_{};
    bool for_each_row_{true};
    std::vector<borrowed_text> update_columns_{};
    borrowed_text old_table_{};
    borrowed_text new_table_{};
    db_expression when_{};
    std::vector<borrowed_text> arguments_{};
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
    std::vector<borrowed_text> columns_{};
    borrowed_text referenced_table_{};
    std::vector<borrowed_text> referenced_columns_{};
    db_referential_action on_delete_{db_referential_action::no_action};
    db_referential_action on_update_{db_referential_action::no_action};
    bool deferrable_{false};
    bool initially_deferred_{false};
    bool not_valid_{false};
    db_expression check_{};
};

struct db_table_definition final {
    borrowed_text name_{};
    std::vector<db_schema_column> columns_{};
    std::vector<db_schema_constraint> constraints_{};
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
    std::vector<db_index_key> keys_{};
    bool unique_{false};
    bool if_not_exists_{false};
    bool concurrently_{false};
    db_index_method method_{db_index_method::btree};
    std::vector<borrowed_text> include_{};
    db_expression where_{};
};

struct db_schema_options final {
    db_driver driver_{db_driver::unspecified};
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
    std::vector<db_column_default> defaults_{};
    std::vector<db_column_generated> generated_columns_{};
    std::vector<db_schema_constraint> constraints_{};
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
    std::vector<borrowed_text> segment_by_{};
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
    [[nodiscard]] std::vector<db_migration> compile(std::string_view id) const;
    [[nodiscard]] db_driver driver() const noexcept {
        return driver_;
    }

private:
    friend class db_procedure;
    struct statement_type final {
        std::string sql_;
        db_migration_atomicity atomicity_{db_migration_atomicity::transactional};
        bool procedural_{false};
    };
    void append(std::string sql, db_migration_atomicity atomicity = db_migration_atomicity::transactional, bool procedural = false);
    void require_postgresql() const;
    void append_type(std::string& sql, const db_type_definition& type) const;
    void append_column(std::string& sql, const db_schema_column& column) const;
    void append_constraint(std::string& sql, const db_schema_constraint& constraint) const;
    void append_expression(std::string& sql, db_expression expression) const;
    void append_drop_behavior(std::string& sql, db_drop_behavior behavior) const;
    [[nodiscard]] std::string constraint_name(std::string_view table, std::string_view relation, std::string_view suffix) const;
    template <typename column_type>
    static db_type_definition entity_column_type() {
        db_type_definition type{.data_type_ = column_type::data_type, .length_ = column_type::options.length_, .precision_ = column_type::options.precision_, .scale_ = column_type::options.scale_};
        if constexpr (detail::is_std_vector<typename column_type::value_type>::value) {
            using item_type = typename detail::is_std_vector<typename column_type::value_type>::value_type;
            if constexpr (column_type::options.data_type_ == db_data_type::inferred || column_type::options.data_type_ == db_data_type::array) {
                type.data_type_ = detail::db_entity_type_traits<item_type>::data_type;
            }
            type.array_ = true;
        }
        if constexpr (!column_type::options.enum_name_.view().empty()) {
            type.custom_name_ = column_type::options.enum_name_.view();
            type.data_type_ = db_data_type::inferred;
        }
        return type;
    }

    db_driver driver_;
    std::vector<statement_type> statements_;
};

template <typename entity_type>
void db_schema::create_table(const db_entity_table_options& options) {
    db_table_definition table_value{.name_ = entity_type::table_name(), .columns_ = std::vector<db_schema_column>{}, .constraints_ = std::vector<db_schema_constraint>{}, .if_not_exists_ = options.if_not_exists_, .temporary_ = options.temporary_, .unlogged_ = options.unlogged_};
    table_value.columns_.reserve(std::tuple_size_v<typename entity_type::columns_type>);
    table_value.constraints_.reserve(options.constraints_.size() + 1U + std::tuple_size_v<typename entity_type::relations_type> * 2U);
    table_value.constraints_.assign(options.constraints_.begin(), options.constraints_.end());
    db_query defaults;
    std::vector<borrowed_text> primary;
    primary.reserve(detail::db_primary_key_count<entity_type>());
    detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
        db_schema_column column{.name_ = c_type::name.view(),
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
        } else if constexpr (c_type::options.create_date_ || c_type::options.update_date_) {
            column.default_value_ = defaults.call("now");
        } else if constexpr (c_type::options.version_) {
            column.default_value_ = defaults.value(1);
        }
        if constexpr (c_type::options.primary_key_) {
            primary.push_back(column.name_);
        }
        if constexpr (c_type::options.generated_) {
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
    // Constraints borrow their names, so the reserve keeps these strings in place.
    std::vector<std::string> generated_names;
    generated_names.reserve(1 + std::tuple_size_v<typename entity_type::relations_type> * 2);
    if (!primary.empty()) {
        const auto& pkey_name = generated_names.emplace_back(constraint_name(entity_type::table_name(), {}, "pkey"));
        table_value.constraints_.push_back({.name_ = pkey_name, .kind_ = db_constraint_kind::primary_key, .columns_ = std::move(primary), .referenced_columns_ = std::vector<borrowed_text>{}});
    }
    detail::for_each_descriptor<typename entity_type::relations_type>([&]<typename r_type, std::size_t> {
        if constexpr (!r_type::is_collection && r_type::is_owning) {
            detail::validate_db_relation<entity_type, r_type>();
            using mapping_type = detail::db_relation_mapping<entity_type, r_type>;
            using target_type = typename r_type::target_entity_type;
            const auto& fkey_name = generated_names.emplace_back(constraint_name(entity_type::table_name(), r_type::name.view(), "fkey"));
            db_schema_constraint fk{.name_ = fkey_name, .kind_ = db_constraint_kind::foreign_key, .columns_ = std::vector<borrowed_text>{}, .referenced_table_ = target_type::table_name(), .referenced_columns_ = std::vector<borrowed_text>{}};
            constexpr std::size_t join_col_count = std::tuple_size_v<typename mapping_type::join_columns_type>;
            fk.columns_.reserve(join_col_count);
            fk.referenced_columns_.reserve(join_col_count);
            detail::for_each_descriptor<typename mapping_type::join_columns_type>([&]<typename j_type, std::size_t> { fk.columns_.emplace_back(j_type::local.view()); fk.referenced_columns_.emplace_back(j_type::referenced.view()); });
            table_value.constraints_.push_back(std::move(fk));
            if constexpr (r_type::kind == db_relation_kind::one_to_one) {
                const auto& key_name = generated_names.emplace_back(constraint_name(entity_type::table_name(), r_type::name.view(), "key"));
                table_value.constraints_.push_back({.name_ = key_name, .kind_ = db_constraint_kind::unique, .columns_ = table_value.constraints_.back().columns_, .referenced_columns_ = std::vector<borrowed_text>{}});
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
            db_table_definition table_value{.name_ = m_type::table_name(), .columns_ = std::vector<db_schema_column>{}, .constraints_ = std::vector<db_schema_constraint>{}};
            table_value.columns_.reserve(std::tuple_size_v<typename m_type::source_columns_type> + std::tuple_size_v<typename m_type::target_columns_type>);
            table_value.constraints_.reserve(3U);
            const auto owner_fk_name = constraint_name(m_type::table_name(), {}, "owner_fkey");
            const auto inverse_fk_name = constraint_name(m_type::table_name(), {}, "inverse_fkey");
            const auto pkey_name = constraint_name(m_type::table_name(), {}, "pkey");
            const auto add_side = [&]<typename referenced_entity_type, typename columns_type>(const std::string& fk_name) {
                db_schema_constraint fk{.name_ = fk_name,
                    .kind_ = db_constraint_kind::foreign_key,
                    .columns_ = std::vector<borrowed_text>{},
                    .referenced_table_ = referenced_entity_type::table_name(),
                    .referenced_columns_ = std::vector<borrowed_text>{}};
                constexpr std::size_t side_count = std::tuple_size_v<columns_type>;
                fk.columns_.reserve(side_count);
                fk.referenced_columns_.reserve(side_count);
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
            std::vector<borrowed_text> primary;
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

namespace ruvia {

template <typename entity_type, typename executor_type>
class db_write_query_builder;

struct db_find_order final {
    borrowed_text column_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};

// span 及元素中的文本均为借用；repository 调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_find_options final {
    borrowed_span<borrowed_text> select_{};
    db_predicate where_{};
    borrowed_span<borrowed_text> relations_{};
    borrowed_span<db_find_order> order_{};
    std::optional<query_row_count> skip_{};
    std::optional<query_row_count> take_{};
    std::optional<db_lock_options> lock_{};
    db_cache_setting_type cache_{};
    bool with_deleted_{false};
};

// span 及元素中的文本均为借用；upsert 调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_upsert_options final {
    borrowed_span<borrowed_text> conflict_paths_{};
    borrowed_span<borrowed_text> update_columns_{};
    bool do_nothing_{false};
    bool any_unique_key_{false};
    bool skip_update_if_no_values_changed_{false};
    db_predicate index_predicate_{};
    borrowed_span<db_assignment> update_expressions_{};
    db_predicate update_where_{};
};

namespace detail {

// This plan belongs to a builder or a single cold operation. It owns every
// selected path and result-column name; it never retains an AST or row view.
class db_relation_plan final {
public:
    static constexpr std::size_t root = std::numeric_limits<std::size_t>::max();
    struct node_type final {
        node_type() = default;
        std::size_t parent_{root};
        std::size_t relation_{0};
        std::size_t offset_{0};
        db_join_type join_{db_join_type::left};
        std::string path_;
        std::string alias_;
        std::vector<std::string> columns_;
    };

    db_relation_plan() = default;
    db_relation_plan(const db_relation_plan&) = default;
    db_relation_plan& operator=(const db_relation_plan&) = delete;
    db_relation_plan(db_relation_plan&&) noexcept = default;
    db_relation_plan& operator=(db_relation_plan&&) = delete;

    [[nodiscard]] db_relation_plan clone() const {
        return *this;
    }
    [[nodiscard]] bool empty() const noexcept {
        return nodes_.empty();
    }
    [[nodiscard]] std::size_t column_count() const noexcept {
        return column_count_;
    }
    [[nodiscard]] std::span<const node_type> nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] std::size_t find(std::size_t parent_value, std::size_t relation) const noexcept;

    template <typename entity_type>
    void add(db_query& query, std::string_view root_alias, std::string_view path,
        std::string_view alias = {}, db_join_type join = db_join_type::left, db_expression condition = {}) {
        if constexpr (db_primary_key_count<entity_type>() == 0) {
            throw std::invalid_argument("relation loading requires a primary key on the root entity");
        } else {
            if (alias.find('.') != std::string_view::npos || alias.find('\0') != std::string_view::npos) {
                throw std::invalid_argument("a relation join alias must be one identifier");
            }
            // normalize() reads a leading component as the root alias or a
            // loaded relation's alias; reject paths where it also names a
            // root relation, so the requested relation is never replaced.
            if (const auto dot = path.find('.'); dot != std::string_view::npos) {
                const auto head = path.substr(0, dot);
                bool alias_reading = head == root_alias;
                for (const auto& node : nodes_) {
                    alias_reading |= node.alias_ == head && node.path_ != head;
                }
                if (alias_reading && names_relation<entity_type>(head)) {
                    throw std::invalid_argument("relation path is ambiguous between an alias and a relation name");
                }
            }
            auto normalized = normalize(root_alias, path);
            validate_path<entity_type>(normalized, 0);
            if (!alias.empty() && uses_source_name(query, alias)) {
                bool same = false;
                for (const auto& node : nodes_) {
                    same |= node.path_ == normalized && node.alias_ == alias && node.join_ == join && condition.empty();
                }
                if (!same) {
                    throw std::invalid_argument("relation join alias is already in use");
                }
            }
            if (nodes_.empty()) {
                column_count_ = std::tuple_size_v<typename entity_type::columns_type>;
            }
            add_path<entity_type>(query, root_alias, normalized, root, alias, join, condition);
        }
    }

    template <typename entity_type>
    static void join_relation(db_query& query, std::string_view root_alias, std::string_view relation_path,
        std::string_view alias, db_join_type join, db_expression condition = {}) {
        if (alias.empty() || alias.find('.') != std::string_view::npos || alias.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("a relation join alias must be one identifier");
        }
        auto source_alias = root_alias;
        auto relation_name = relation_path;
        if (const auto dot = relation_path.find('.'); dot != std::string_view::npos) {
            source_alias = relation_path.substr(0, dot);
            relation_name = relation_path.substr(dot + 1);
        }
        if (source_alias.empty() || relation_name.empty() || relation_name.find('.') != std::string_view::npos) {
            throw std::invalid_argument("relation join path requires a direct relation or alias.relation");
        }
        if (source_alias != root_alias) {
            throw std::invalid_argument("unknown database entity relation");
        }
        if (uses_source_name(query, alias)) {
            throw std::invalid_argument("relation join alias is already in use");
        }
        bool found = false;
        for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t> {
            if (relation_name != relation_type::name.view()) {
                return;
            }
            found = true;
            validate_db_relation<entity_type, relation_type>();
            using mapping_type = db_relation_mapping<entity_type, relation_type>;
            if constexpr (mapping_type::through_join_table) {
                std::string junction;
                junction.reserve(alias.size() + 9U);
                junction.append(alias);
                junction.append("_junction");
                if (uses_source_name(query, junction)) {
                    throw std::invalid_argument("relation junction alias is already in use");
                }
                append_relation_joins<entity_type, relation_type>(
                    query, source_alias, alias, junction, join, condition);
            } else {
                append_relation_joins<entity_type, relation_type>(
                    query, source_alias, alias, {}, join, condition);
            }
        });
        if (!found) {
            throw std::invalid_argument("unknown database entity relation");
        }
    }

    template <typename entity_type>
    [[nodiscard]] std::optional<db_query> prepare(const db_query& query,
        std::string_view root_alias, db_driver driver) const {
        constexpr auto keys = db_primary_key_columns<entity_type>();
        return prepare_entity_read(query, keys, root_alias, driver);
    }

private:
    [[nodiscard]] std::string normalize(std::string_view root_alias, std::string_view path) const;
    [[nodiscard]] static bool uses_source_name(const db_query& query, std::string_view name);
    [[nodiscard]] static bool uses_projection_name(const db_query& query, std::string_view name);
    // Returns the rewritten query, or nullopt when the original query can
    // load relations unchanged.
    [[nodiscard]] static std::optional<db_query> prepare_entity_read(const db_query& query,
        std::span<const std::string_view> primary_key, std::string_view root_alias, db_driver driver);

    template <typename entity_type>
    [[nodiscard]] static bool names_relation(std::string_view name) {
        bool found = false;
        for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t> {
            found = found || name == relation_type::name.view();
        });
        return found;
    }

    template <typename entity_type>
    static void validate_path(std::string_view path, std::size_t depth) {
        if (path.empty() || depth >= max_db_query_depth) {
            throw std::invalid_argument("relation path is empty or exceeds query nesting depth");
        }
        const auto dot = path.find('.');
        const auto name = path.substr(0, dot);
        const auto rest = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
        if (name.empty() || (dot != std::string_view::npos && rest.empty())) {
            throw std::invalid_argument("relation path contains an empty component");
        }
        bool found = false;
        for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t> {
            if (name != relation_type::name.view()) {
                return;
            }
            found = true;
            validate_db_relation<entity_type, relation_type>();
            using target_type = typename relation_type::target_entity_type;
            if constexpr (db_primary_key_count<target_type>() == 0) {
                throw std::invalid_argument("relation loading requires a primary key on every selected target");
            } else if (!rest.empty()) {
                validate_path<target_type>(rest, depth + 1);
            }
        });
        if (!found) {
            throw std::invalid_argument("unknown database entity relation");
        }
    }

    [[nodiscard]] std::string unique_alias(const db_query& query, std::string_view reserved);
    [[nodiscard]] std::string unique_projection(const db_query& query);
    static void conjunction(db_query& query, db_expression& predicate, db_expression term);

    template <typename entity_type, typename relation_type>
    static void append_relation_joins(db_query& query, std::string_view source_alias,
        std::string_view target_alias, std::string_view junction_alias, db_join_type join,
        db_expression extra_condition) {
        using mapping_type = db_relation_mapping<entity_type, relation_type>;
        using target_type = typename relation_type::target_entity_type;
        db_expression predicate;
        if constexpr (mapping_type::through_join_table) {
            for_each_descriptor<typename mapping_type::source_columns_type>([&]<typename column_type, std::size_t> {
                conjunction(query, predicate,
                    query.binary(query.column(column_type::referenced.view(), source_alias),
                        db_binary_operator::equal, query.column(column_type::local.view(), junction_alias)));
            });
            query.join(join, mapping_type::table_name(), predicate, junction_alias);
            predicate = {};
            for_each_descriptor<typename mapping_type::target_columns_type>([&]<typename column_type, std::size_t> {
                conjunction(query, predicate,
                    query.binary(query.column(column_type::local.view(), junction_alias),
                        db_binary_operator::equal, query.column(column_type::referenced.view(), target_alias)));
            });
        } else {
            (void)junction_alias;
            for_each_descriptor<typename mapping_type::join_columns_type>([&]<typename column_type, std::size_t> {
                conjunction(query, predicate,
                    query.binary(query.column(column_type::local.view(), source_alias),
                        db_binary_operator::equal, query.column(column_type::referenced.view(), target_alias)));
            });
        }
        if (!extra_condition.empty()) {
            conjunction(query, predicate, query.import_expression(extra_condition));
        }
        query.join(join, target_type::table_name(), predicate, target_alias);
    }

    template <typename entity_type, typename relation_type>
    void append_join(db_query& query, std::string_view source_alias, node_type& node_value,
        std::string_view reserved_alias, db_expression extra_condition) {
        using mapping_type = db_relation_mapping<entity_type, relation_type>;
        using target_type = typename relation_type::target_entity_type;
        if constexpr (mapping_type::through_join_table) {
            auto junction = unique_alias(query, node_value.alias_);
            // A terminal user alias is reserved even while constructing an
            // ancestor's automatic junction alias.
            if (junction == reserved_alias) {
                junction = unique_alias(query, reserved_alias);
            }
            append_relation_joins<entity_type, relation_type>(
                query, source_alias, node_value.alias_, junction, node_value.join_, extra_condition);
        } else {
            (void)reserved_alias;
            append_relation_joins<entity_type, relation_type>(
                query, source_alias, node_value.alias_, {}, node_value.join_, extra_condition);
        }
        node_value.columns_.reserve(std::tuple_size_v<typename target_type::columns_type>);
        for_each_descriptor<typename target_type::columns_type>([&]<typename column_type, std::size_t> {
            auto name = unique_projection(query);
            query.add_select(query.alias(query.column(column_type::name.view(), node_value.alias_), name));
            node_value.columns_.push_back(std::move(name));
        });
    }

    template <typename entity_type>
    void add_path(db_query& query, std::string_view root_alias, std::string_view path,
        std::size_t parent_value, std::string_view requested_alias, db_join_type requested_join, db_expression condition) {
        const auto dot = path.find('.');
        const auto name = path.substr(0, dot);
        const auto rest = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
        for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t i> {
            if (name != relation_type::name.view()) {
                return;
            }
            auto index = find(parent_value, i);
            if (index == root) {
                node_type node;
                node.parent_ = parent_value;
                node.relation_ = i;
                node.offset_ = column_count_;
                node.join_ = rest.empty() ? requested_join : db_join_type::left;
                if (parent_value != root) {
                    node.path_.reserve(nodes_[parent_value].path_.size() + 1U + name.size());
                    node.path_.append(nodes_[parent_value].path_);
                    node.path_.push_back('.');
                }
                node.path_.append(name);
                node.alias_ = rest.empty() && !requested_alias.empty()
                                  ? std::string(requested_alias)
                                  : unique_alias(query, requested_alias);
                const auto source_alias = parent_value == root ? root_alias : std::string_view(nodes_[parent_value].alias_);
                append_join<entity_type, relation_type>(
                    query, source_alias, node, requested_alias, rest.empty() ? condition : db_expression{});
                column_count_ += node.columns_.size();
                index = nodes_.size();
                nodes_.push_back(std::move(node));
            } else if (rest.empty() && ((!requested_alias.empty() && nodes_[index].alias_ != requested_alias) || nodes_[index].join_ != requested_join || !condition.empty())) {
                throw std::invalid_argument("relation path was already loaded with another alias or join kind");
            }
            if (!rest.empty()) {
                add_path<typename relation_type::target_entity_type>(
                    query, root_alias, rest, index, requested_alias, requested_join, condition);
            }
        });
    }

    std::vector<node_type> nodes_;
    std::size_t column_count_{0};
    std::size_t next_alias_{0};
    std::size_t next_projection_{0};
};

struct db_graph_identity final {
    std::size_t index_{0};
    std::size_t scope_{0};
};

struct db_graph_key final {
    std::size_t owner_{0};
    std::size_t relation_{0};
    std::string_view pk_{};

    bool operator==(const db_graph_key&) const noexcept = default;
};

struct db_graph_key_hash final {
    [[nodiscard]] std::size_t operator()(const db_graph_key& key) const noexcept {
        std::size_t seed = std::hash<std::size_t>{}(key.owner_);
        seed ^= std::hash<std::size_t>{}(key.relation_) + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<std::string_view>{}(key.pk_) + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

// Indices, rather than pointers into result vectors, remain valid when a
// collection grows. The identity table is temporary operation storage; the
// resulting entities own their nested values without retaining this table.
class db_relation_decoder final {
public:
    explicit db_relation_decoder(const db_relation_plan& plan)
        : plan_(plan) {}

    template <typename entity_type>
    entity_rows<entity_type> decode(const db_rows& rows) {
        entity_rows<entity_type> result;
        if (rows.empty()) {
            return result;
        }
        resolve_columns<entity_type>(rows.front());
        identities_.reserve(rows.size());
        for (const auto& row : rows) {
            const auto key = make_key<entity_type>(row, 0, 0, db_relation_plan::root);
            if (!key) {
                throw db_conversion_error(db_conversion_error::code_type::unexpected_null, "NULL root primary key in a relation query");
            }
            auto position = identities_.find(*key);
            db_graph_identity identity;
            if (position == identities_.end()) {
                identity = {result.size(), next_scope_++};
                entity_type entity;
                decode_columns(entity, row, 0);
                result.push_back(std::move(entity));
                store_identity<entity_type>(*key, identity);
            } else {
                identity = position->second;
            }
            decode_relations(result[identity.index_], row, db_relation_plan::root, identity.scope_);
        }
        return result;
    }

private:
    template <typename entity_type>
    void resolve_columns(const db_row& row) {
        columns_.resize(plan_.column_count());
        const auto names = row.column_names();
        const auto resolve = [&](std::string_view name) {
            const auto lookup = find_unique_db_result_column(names, name);
            if (lookup.state_ == unique_lookup_status::repeated) {
                throw db_conversion_error(db_conversion_error::code_type::invalid_format, "duplicate entity projection name in a relation query");
            }
            if (lookup.state_ == unique_lookup_status::absent || lookup.index_ >= row.size()) {
                throw db_conversion_error(db_conversion_error::code_type::missing_column, "relation query must select every mapped entity column");
            }
            return lookup.index_;
        };
        for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
            columns_[i] = resolve(column_type::name.view());
        });
        for (const auto& node : plan_.nodes()) {
            for (std::size_t i = 0; i < node.columns_.size(); ++i) {
                columns_[node.offset_ + i] = resolve(node.columns_[i]);
            }
        }
    }

    template <typename entity_type>
    void decode_columns(entity_type& entity, const db_row& row, std::size_t offset) const {
        for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
            decode_entity_field<entity_type, column_type>(entity, row[columns_[offset + i]]);
        });
    }

    template <typename entity_type>
    std::optional<db_graph_key> make_key(const db_row& row, std::size_t offset, std::size_t owner_value, std::size_t relation) {
        if constexpr (db_primary_key_count<entity_type>() == 1) {
            std::optional<std::string_view> pk;
            for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
                if constexpr (column_type::options.primary_key_) {
                    pk = row[columns_[offset + i]].value();
                }
            });
            if (!pk) {
                return std::nullopt;
            }
            return db_graph_key{owner_value, relation, *pk};
        } else {
            key_.clear();
            if (key_.capacity() < 64) {
                key_.reserve(64);
            }
            std::size_t nulls = 0;
            for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
                if constexpr (column_type::options.primary_key_) {
                    const auto value = row[columns_[offset + i]].value();
                    if (!value) {
                        ++nulls;
                        return;
                    }
                    append_db_number(key_, static_cast<std::uint64_t>(value->size()));
                    key_.push_back(':');
                    key_.append(*value);
                }
            });
            if (nulls != 0 && nulls != db_primary_key_count<entity_type>()) {
                throw db_conversion_error(db_conversion_error::code_type::unexpected_null, "partially NULL composite primary key in a relation query");
            }
            if (nulls != 0) {
                return std::nullopt;
            }
            return db_graph_key{owner_value, relation, key_};
        }
    }

    template <typename entity_type>
    void store_identity(db_graph_key key, db_graph_identity identity) {
        if constexpr (db_primary_key_count<entity_type>() > 1) {
            key.pk_ = composite_keys_.emplace_back(key.pk_);
        }
        identities_.emplace(key, identity);
    }

    template <typename entity_type>
    void decode_relations(entity_type& entity, const db_row& row, std::size_t parent_value, std::size_t scope) {
        for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t i> {
            const auto node_index = plan_.find(parent_value, i);
            if (node_index == db_relation_plan::root) {
                return;
            }
            const auto& node_value = plan_.nodes()[node_index];
            using target_type = typename relation_type::target_entity_type;
            const auto key = make_key<target_type>(row, node_value.offset_, scope, node_index);
            if constexpr (relation_type::is_collection) {
                auto& collection_value = db_entity_access<entity_type>::template ensure_relation_collection<relation_type::name>(entity);
                if (!key) {
                    return;
                }
                auto position = identities_.find(*key);
                db_graph_identity identity;
                if (position == identities_.end()) {
                    identity = {collection_value.size(), next_scope_++};
                    target_type child_value;
                    decode_columns(child_value, row, node_value.offset_);
                    collection_value.push_back(std::move(child_value));
                    store_identity<target_type>(*key, identity);
                } else {
                    identity = position->second;
                }
                decode_relations(collection_value[identity.index_], row, node_index, identity.scope_);
            } else {
                if (!key) {
                    if (entity.template is_set<relation_type::name>() && !entity.template is_null<relation_type::name>()) {
                        throw db_conversion_error(db_conversion_error::code_type::unexpected_row_count, "inconsistent to-one rows in a relation query");
                    }
                    db_entity_access<entity_type>::template set_relation_null<relation_type::name>(entity);
                    return;
                }
                auto position = identities_.find(*key);
                db_graph_identity identity;
                if (position == identities_.end()) {
                    if (entity.template is_set<relation_type::name>()) {
                        throw db_conversion_error(db_conversion_error::code_type::unexpected_row_count, "to-one relation returned more than one target");
                    }
                    identity = {0, next_scope_++};
                    auto& child_value = db_entity_access<entity_type>::template emplace_relation<relation_type::name>(entity);
                    decode_columns(child_value, row, node_value.offset_);
                    store_identity<target_type>(*key, identity);
                } else {
                    identity = position->second;
                }
                decode_relations(entity.template get<relation_type::name>(), row, node_index, identity.scope_);
            }
        });
    }

    const db_relation_plan& plan_;
    std::vector<std::size_t> columns_;
    std::unordered_map<db_graph_key, db_graph_identity, db_graph_key_hash> identities_;
    std::deque<std::string> composite_keys_;
    std::string key_;
    std::size_t next_scope_{1};
};

template <typename entity_type>
struct db_map_related_entities final {
    db_relation_plan plan_;
    entity_rows<entity_type> operator()(db_rows&& rows) {
        return db_relation_decoder(plan_).template decode<entity_type>(rows);
    }
};

template <typename entity_type>
struct db_map_one_related_entity final {
    db_relation_plan plan_;
    std::optional<entity_type> operator()(db_rows&& rows) {
        auto entities = db_relation_decoder(plan_).template decode<entity_type>(rows);
        if (entities.empty()) {
            return std::nullopt;
        }
        if (entities.size() != 1) {
            throw db_conversion_error(db_conversion_error::code_type::unexpected_row_count, "single-entity relation query returned multiple roots");
        }
        return std::optional<entity_type>(std::in_place, std::move(entities[0]));
    }
};

// Imports each source into `query` and passes the imported expressions to
// `consume` in one call; small lists stay on the stack.
template <typename source_type, typename import_type, typename consume_type>
void import_db_expressions([[maybe_unused]] db_query& query, std::span<const source_type> sources, import_type&& import_one, consume_type&& consume) {
    static constexpr std::size_t inline_capacity = 16;
    if (sources.size() <= inline_capacity) {
        std::array<db_expression, inline_capacity> imported{};
        for (std::size_t i = 0; i < sources.size(); ++i) {
            imported[i] = import_one(sources[i]);
        }
        consume(std::span<const db_expression>(imported.data(), sources.size()));
        return;
    }
    std::vector<db_expression> imported;
    imported.reserve(sources.size());
    for (const auto& source : sources) {
        imported.push_back(import_one(source));
    }
    consume(std::span<const db_expression>(imported));
}

// Stages borrowed column names as one db_selection list for `consume`.
template <std::size_t inline_capacity, typename name_type, typename consume_type>
decltype(auto) stage_db_selections(std::span<const name_type> columns, consume_type&& consume) {
    if (columns.size() <= inline_capacity) {
        std::array<db_selection, inline_capacity> selections{};
        for (std::size_t i = 0; i < columns.size(); ++i) {
            selections[i] = db_selection{columns[i]};
        }
        return consume(std::span<const db_selection>(selections.data(), columns.size()));
    }
    std::vector<db_selection> selections;
    selections.reserve(columns.size());
    for (const auto& column : columns) {
        selections.emplace_back(column);
    }
    return consume(std::span<const db_selection>(selections));
}

}  // namespace detail

class db_values_builder final {
public:
    explicit db_values_builder(std::span<const std::string_view> columns) {
        if (columns.empty()) {
            throw std::invalid_argument("db_values_builder requires at least one column");
        }
        columns_.reserve(columns.size());
        for (const auto column : columns) {
            columns_.emplace_back(column);
        }
    }
    explicit db_values_builder(std::initializer_list<std::string_view> columns)
        : db_values_builder(std::span<const std::string_view>(columns.begin(), columns.size())) {}

    db_values_builder& row(std::span<const db_expr> values) {
        if (values.size() != columns_.size()) {
            throw std::invalid_argument("db_values_builder row width differs from column count");
        }
        detail::import_db_expressions(
            query_, values, [&](const db_expr& value) { return value.expression(query_); },
            [&](std::span<const db_expression> imported) { query_.values(imported); });
        return *this;
    }
    db_values_builder& row(std::initializer_list<db_expr> values) {
        return row(std::span<const db_expr>(values.begin(), values.size()));
    }

    [[nodiscard]] const db_query& query() const& noexcept RUVIA_LIFETIMEBOUND {
        return query_;
    }
    const db_query& query() const&& = delete;
    [[nodiscard]] const std::vector<std::string>& columns() const& noexcept RUVIA_LIFETIMEBOUND {
        return columns_;
    }
    const std::vector<std::string>& columns() const&& = delete;

private:
    db_query query_;
    std::vector<std::string> columns_;
};

namespace detail {

// Runtime column names resolve against one descriptor table per entity.
struct db_column_info final {
    std::string_view name_;
    bool writable_{false};
    bool nullable_{false};
    bool numeric_{false};
    bool primary_key_{false};
    bool conflict_updatable_{false};
};
template <typename e_type>
inline constexpr auto db_entity_columns = []<typename... columns_type>(std::tuple<columns_type...>*) {
    return std::array<db_column_info, sizeof...(columns_type)>{db_column_info{
        columns_type::name.view(), is_writable_db_column<columns_type>(), columns_type::options.nullable_, is_numeric_db_column<columns_type>(),
        columns_type::options.primary_key_, is_conflict_updatable_db_column<columns_type>()}...};
}(static_cast<typename e_type::columns_type*>(nullptr));
const db_column_info& require_db_column(std::span<const db_column_info> columns, std::string_view name);
void validate_insert_columns(std::span<const db_column_info> entity_columns, std::span<const std::string_view> columns);
// Lifecycle columns are writable by the db_column invariant.
template <typename e_type>
consteval std::string_view delete_date_column() noexcept {
    std::string_view result{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (c_type::options.delete_date_) {
            result = c_type::name.view();
        }
    });
    return result;
}
template <typename e_type>
consteval std::string_view update_date_column() noexcept {
    std::string_view result{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (c_type::options.update_date_) {
            result = c_type::name.view();
        }
    });
    return result;
}
template <typename e_type>
consteval std::string_view version_column() noexcept {
    std::string_view result{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (c_type::options.version_) {
            result = c_type::name.view();
        }
    });
    return result;
}
// The one lifecycle policy for ORM-generated UPDATE and conflict-update
// assignments: update_date_ takes now() and version_ increments by one, unless
// the statement assigns that column itself (`assigned`).
template <typename assigned_type, typename sink_type>
void append_lifecycle_assignments(db_query& query, std::string_view qualifier,
    std::string_view update_date, std::string_view version, assigned_type&& assigned, sink_type&& sink) {
    if (!update_date.empty()) {
        if (!assigned(update_date)) {
            sink(update_date, query.call("now"));
        }
    }
    if (!version.empty()) {
        if (!assigned(version)) {
            sink(version, query.binary(query.column(version, qualifier), db_binary_operator::add, query.value(1)));
        }
    }
}
// Stamps an UPDATE before any explicit assignment; a later SET of the same
// column replaces the stamp, so explicit values win.
template <typename e_type>
void stamp_lifecycle_columns(db_query& query, std::string_view qualifier) {
    append_lifecycle_assignments(
        query, qualifier, update_date_column<e_type>(), version_column<e_type>(), [](std::string_view) noexcept { return false; },
        [&query](std::string_view column, db_expression value) { query.set(column, value); });
}
// Multi-row conflict updates read EXCLUDED, which holds the column default for
// rows that left a column unset; every auto-updated column must be set in all
// rows or in none.
template <typename e_type>
void require_uniform_set_columns(std::span<const e_type> entities, std::span<const db_assignment> overridden) {
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (is_conflict_updatable_db_column<c_type>()) {
            if (std::ranges::any_of(overridden, [](const db_assignment& item) { return item.column_ == c_type::name.view(); })) {
                return;
            }
            const auto set_count = static_cast<std::size_t>(std::ranges::count_if(
                entities, [](const e_type& entity) { return entity.template is_set<c_type::name>(); }));
            if (set_count != 0 && set_count != entities.size()) {
                throw std::invalid_argument("bulk upsert requires identical set columns or explicit update_columns");
            }
        }
    });
}
template <typename joined_type, typename e_type>
[[nodiscard]] db_expression import_join_predicate(db_query& query, const db_predicate& on, std::string_view alias, std::string_view join_alias) {
    if (on.empty()) {
        return {};
    }
    auto imported = on.expression(query, e_type::table_name(), alias);
    if (!join_alias.empty() && joined_type::table_name() != e_type::table_name()) {
        imported = query.import_expression(imported, joined_type::table_name(), join_alias);
    }
    return imported;
}
template <typename e_type>
void select_entity(db_query& query, std::string_view alias) {
    const auto qualifier = alias.empty() ? e_type::table_name() : alias;
    std::array<db_expression, std::tuple_size_v<typename e_type::columns_type>> columns{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t index> {
        columns[index] = query.column(c_type::name.view(), qualifier);
    });
    query.select(columns);
    query.from(e_type::table_name(), alias);
}
template <typename e_type>
void apply_find_options(db_query& query, const db_find_options& options, std::string_view alias = {}) {
    query.cache(options.cache_);
    if (!options.where_.empty()) {
        query.where(options.where_.expression(query, e_type::table_name(), alias));
    }
    for (const auto& order : options.order_) {
        require_db_column(db_entity_columns<e_type>, order.column_);
        query.add_order_by(query.column(order.column_, alias.empty() ? e_type::table_name() : alias), order.direction_, order.nulls_);
    }
    query.offset(options.skip_).limit(options.take_);
    if (options.lock_) {
        query.lock(*options.lock_);
    }
}
struct db_map_count final {
    std::uint64_t operator()(db_rows&& rows) const {
        return db_count_value(rows);
    }
};
struct db_map_exists final {
    bool operator()(db_rows&& rows) const {
        if (rows.size() != 1) {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_row_count, "database exists did not return exactly one row");
        }
        const auto value = rows.front()["exists"].template as<bool>();
        if (!value) {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "database exists column is NULL");
        }
        return *value;
    }
};
template <typename e_type, typename c_type>
db_expression entity_column_value(db_query& query, const e_type& entity) {
    if (!entity.template is_set<c_type::name>()) {
        return query.default_value();
    }
    if (entity.template is_null<c_type::name>()) {
        return query.null_value();
    }
    return query.value(entity_db_value(entity.template get<c_type::name>()));
}

template <typename e_type>
void append_entity_insert_rows(db_query& query, std::span<const e_type> entities,
    std::array<std::string_view, std::tuple_size_v<typename e_type::columns_type>>& columns,
    std::size_t& column_count) {
    if (entities.empty()) {
        throw std::invalid_argument("repository insert requires an entity");
    }
    std::array<bool, std::tuple_size_v<typename e_type::columns_type>> selected{};
    column_count = 0;
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t index> {
        if constexpr (is_writable_db_column<c_type>()) {
            if (std::ranges::any_of(entities, [](const e_type& entity) { return entity.template is_set<c_type::name>(); })) {
                selected[index] = true;
                columns[column_count++] = c_type::name.view();
            }
        }
    });
    std::vector<db_expression> values;
    if (column_count != 0 && entities.size() > values.max_size() / column_count) {
        throw std::length_error("repository insert batch is too large");
    }
    values.reserve(column_count * entities.size());
    for (const auto& entity : entities) {
        for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t index> {
            if constexpr (is_writable_db_column<c_type>()) {
                if (selected[index]) {
                    values.push_back(entity_column_value<e_type, c_type>(query, entity));
                }
            }
        });
    }
    query.insert_rows(std::span<const std::string_view>(columns.data(), column_count), values, entities.size());
}

template <typename e_type>
struct db_map_projection final {
    static_assert(sql_entity<e_type> || sql_projection<e_type>, "SQL results require a SQL entity or a db_projection");
    explicit db_map_projection(std::span<const std::string> selected, bool allow_extra_columns = false, bool wildcard = false)
        : wildcard_(wildcard) {
        if (!wildcard && selected.empty()) {
            selected_columns_.fill(true);
            return;
        }
        if (!allow_extra_columns) {
            for (const auto& name : selected) {
                require_db_column(db_entity_columns<e_type>, name);
            }
        }
        for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t index> {
            selected_columns_[index] = std::ranges::find(selected, c_type::name.view()) != selected.end();
        });
    }
    entity_rows<e_type> operator()(const db_rows& rows) const {
        auto result_value = db_result_access::make_entity_rows<e_type>(rows.size());
        db_entity_row_decoder<e_type> decoder(selected_columns_, wildcard_);
        for (const auto& row : rows) {
            result_value.push_back(decoder.decode(row));
        }
        return result_value;
    }

    std::array<bool, std::tuple_size_v<typename e_type::columns_type>> selected_columns_{};
    bool wildcard_{false};
};
template <typename e_type>
struct db_map_one_projection final {
    db_map_projection<e_type> mapper_;
    std::optional<e_type> operator()(db_rows&& rows) const {
        if (rows.empty()) {
            return std::nullopt;
        }
        db_entity_row_decoder<e_type> decoder(mapper_.selected_columns_, mapper_.wildcard_);
        return decoder.decode(rows.front());
    }
};
struct db_map_raw_rows final {
    db_rows operator()(db_rows&& rows) const noexcept {
        return std::move(rows);
    }
};
template <db_mapped_value scalar_type>
struct db_map_scalar final {
    std::optional<scalar_type> operator()(db_rows&& rows) const {
        if (rows.empty() || !rows.front()[std::size_t{0}].value()) {
            return std::nullopt;
        }
        return ::ruvia::decode_db_field<scalar_type>(rows.front()[std::size_t{0}]);
    }
};
template <db_mapped_value scalar_type>
struct db_map_scalars final {
    std::vector<scalar_type> operator()(db_rows&& rows) const {
        std::vector<scalar_type> result;
        result.reserve(rows.size());
        for (const auto& row : rows) {
            if (!row[std::size_t{0}].value()) {
                throw db_conversion_error(
                    db_conversion_error::code_type::unexpected_null, "database scalar column is NULL");
            }
            result.push_back(::ruvia::decode_db_field<scalar_type>(row[std::size_t{0}]));
        }
        return result;
    }
};
template <typename e_type>
struct db_map_raw_and_entities final {
    db_map_projection<e_type> mapper_;
    std::pair<entity_rows<e_type>, db_rows> operator()(db_rows&& rows) const {
        auto entities = mapper_(rows);
        return {std::move(entities), std::move(rows)};
    }
};
template <typename e_type>
struct db_map_required_one final {
    db_map_projection<e_type> mapper_;
    e_type operator()(db_rows&& rows) const {
        if (rows.size() != 1) {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_row_count, "database save did not return the saved row");
        }
        db_entity_row_decoder<e_type> decoder(mapper_.selected_columns_, mapper_.wildcard_);
        return decoder.decode(rows.front());
    }
};
struct db_projection final {
    db_projection() = default;
    [[nodiscard]] bool empty() const noexcept {
        return columns_.empty() && !wildcard_;
    }
    std::vector<std::string> columns_;
    bool wildcard_{false};
};
// Wildcards stay in the AST; only explicit result names are tracked for mapping.
db_projection apply_projection(db_query& query, std::span<const db_selection> selection,
    std::string_view qualifier, bool returning = false, std::string_view root_table = {},
    const db_projection* previous = nullptr);
[[nodiscard]] std::string format_truncate_table(std::string_view table, db_driver driver);

void apply_entity_upsert(db_query& query, std::span<const db_column_info> entity_columns,
    std::span<db_assignment> updates, std::span<const std::string_view> inserted_columns,
    const db_upsert_options& options, db_driver driver, std::string_view table, std::string_view qualifier,
    std::string_view update_date, std::string_view version);

template <typename entity_type>
void apply_entity_upsert(db_query& query, std::span<const std::string_view> inserted_columns,
    const db_upsert_options& options, db_driver driver, std::string_view qualifier) {
    std::array<db_assignment, std::tuple_size_v<typename entity_type::columns_type>> updates{};
    apply_entity_upsert(query, db_entity_columns<entity_type>, updates, inserted_columns,
        options, driver, entity_type::table_name(), qualifier, update_date_column<entity_type>(), version_column<entity_type>());
}

template <typename executor_type>
using db_repository_executor_type = std::conditional_t<std::same_as<executor_type, db_session>, db_session, executor_type&>;
template <typename executor_type>
using db_repository_input_type = std::conditional_t<std::same_as<executor_type, db_session>, const db_session&, executor_type&>;

}  // namespace detail

template <typename entity_type, typename executor_type>
class db_query_builder final {
public:
    db_query_builder(const db_query_builder&) = delete;
    db_query_builder& operator=(const db_query_builder&) = delete;
    db_query_builder(db_query_builder&&) = default;
    db_query_builder& operator=(db_query_builder&&) = delete;

    db_query_builder& select(std::span<const db_selection> fields_value) {
        require_plain_projection();
        for (const auto& field : fields_value) {
            if (!field.has_expression() && !field.is_wildcard()) {
                detail::require_db_column(detail::db_entity_columns<entity_type>, field.column_);
            }
        }
        selected_ = detail::apply_projection(query_, fields_value, alias_, false, entity_type::table_name());
        return *this;
    }
    db_query_builder& select(std::initializer_list<db_selection> fields_value) {
        return select(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    db_query_builder& select(std::span<const std::string_view> columns) {
        return detail::stage_db_selections<column_capacity>(columns,
            [this](std::span<const db_selection> selections) -> db_query_builder& { return select(selections); });
    }
    db_query_builder& select(const db_expr& expression, borrowed_text alias_name) {
        return select({expression.as(alias_name)});
    }
    db_query_builder& add_select(std::span<const db_selection> fields_value) {
        require_plain_projection();
        if (fields_value.empty()) {
            throw std::invalid_argument("add_select requires at least one field");
        }
        for (const auto& field : fields_value) {
            if (!field.has_expression() && !field.is_wildcard()) {
                detail::require_db_column(detail::db_entity_columns<entity_type>, field.column_);
            }
        }
        detail::db_projection implicit;
        const auto* previous = &selected_;
        if (selected_.empty()) {
            implicit.columns_.reserve(column_capacity);
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
                implicit.columns_.emplace_back(c_type::name.view());
            });
            previous = &implicit;
        }
        selected_ = detail::apply_projection(query_, fields_value, alias_, false, entity_type::table_name(), previous);
        return *this;
    }
    db_query_builder& add_select(std::initializer_list<db_selection> fields_value) {
        return add_select(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    db_query_builder& add_select(std::string_view column) {
        return add_select({db_selection{column}});
    }
    db_query_builder& add_select(const db_expr& expression, borrowed_text alias_name) {
        return add_select({expression.as(alias_name)});
    }
    db_query_builder& from_nothing() {
        require_plain_projection();
        query_.clear_source();
        return *this;
    }
    template <typename joined_type>
    db_query_builder& join(db_join_type type, std::string_view alias, const db_predicate& on = {}) {
        static_assert(sql_entity<joined_type>, "SQL joins require a SQL entity");
        query_.join(type, joined_type::table_name(), detail::import_join_predicate<joined_type, entity_type>(query_, on, alias_, alias), alias);
        return *this;
    }
    template <typename joined_type>
    db_query_builder& join(db_join_type type, std::string_view alias, std::span<const std::string_view> using_columns) {
        static_assert(sql_entity<joined_type>, "SQL joins require a SQL entity");
        query_.join(type, joined_type::table_name(), {}, alias, using_columns);
        return *this;
    }
    template <typename joined_type>
    db_query_builder& join(db_join_type type, std::string_view alias, std::initializer_list<std::string_view> using_columns) {
        return join<joined_type>(type, alias, std::span<const std::string_view>(using_columns.begin(), using_columns.size()));
    }
    template <typename source, typename source_executor_type>
    db_query_builder& join(db_join_type type, const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        source_value.require_plain_projection();
        query_.join(type, source_value.query_, detail::import_join_predicate<source, entity_type>(query_, on, alias_, alias), alias, options);
        return *this;
    }
    db_query_builder& join(db_join_type type, const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join(type, source_value, on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, const db_predicate& on = {}) {
        query_.join(type, name, on.expression(query_, entity_type::table_name(), alias_), alias);
        return *this;
    }
    db_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, std::span<const std::string_view> using_columns) {
        query_.join(type, name, {}, alias, using_columns);
        return *this;
    }
    db_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, std::initializer_list<std::string_view> using_columns) {
        return join_cte(type, name, alias, std::span<const std::string_view>(using_columns.begin(), using_columns.size()));
    }
    db_query_builder& join_function(db_join_type type, db_expression function, std::string_view alias,
        const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join_function(type, query_.import_expression(function), on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_query_builder& left_join(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        detail::db_relation_plan::template join_relation<entity_type>(
            query_, alias_, relation, alias, db_join_type::left, condition.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& inner_join(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        detail::db_relation_plan::template join_relation<entity_type>(
            query_, alias_, relation, alias, db_join_type::inner, condition.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    template <typename joined_type>
    db_query_builder& left_join(std::string_view alias, const db_predicate& on = {}) {
        return join<joined_type>(db_join_type::left, alias, on);
    }
    template <typename joined_type>
    db_query_builder& inner_join(std::string_view alias, const db_predicate& on = {}) {
        return join<joined_type>(db_join_type::inner, alias, on);
    }
    template <typename source, typename source_executor_type>
    db_query_builder& left_join(const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::left, source_value, alias, on, options);
    }
    template <typename source, typename source_executor_type>
    db_query_builder& inner_join(const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::inner, source_value, alias, on, options);
    }
    db_query_builder& left_join(const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::left, source_value, alias, on, options);
    }
    db_query_builder& inner_join(const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::inner, source_value, alias, on, options);
    }
    template <typename source_type>
        requires requires(const source_type& source) {
            { source.query() } -> std::same_as<const db_query&>;
        }
    db_query_builder& with(std::string_view name, const source_type& source, const db_cte_options& options = {}) {
        return with(name, source.query(), options);
    }
    db_query_builder& with(std::string_view name, const db_query& source_value, const db_cte_options& options = {}) {
        if (source_value.has_writes()) {
            require_plain_projection();
        }
        query_.with(name, source_value, options);
        return *this;
    }
    db_query_builder& with(std::string_view name, const db_values_builder& values,
        db_materialization materialization = db_materialization::default_value) {
        query_.with_columns(name, values.query(), materialization, values.columns());
        return *this;
    }
    db_query_builder& from_cte(std::string_view name) {
        require_plain_projection();
        query_.from(name, alias_);
        return *this;
    }
    db_query_builder& from_function(db_expression function, std::string_view alias = {}, const db_source_options& options = {}) {
        require_plain_projection();
        query_.from_function(query_.import_expression(function), alias.empty() ? std::string_view(alias_) : alias, options);
        return *this;
    }
    // Borrows the flat AST, without applying execution-time soft-delete filtering.
    // The builder and its resource must outlive the view; consumers clone on import.
    [[nodiscard]] const db_query& query() const& RUVIA_LIFETIMEBOUND {
        require_plain_projection();
        return query_;
    }
    const db_query& query() const&& = delete;
    db_expression subquery(db_query& expressions) const {
        require_plain_projection();
        return expressions.subquery(query_);
    }
    db_expression exists(db_query& expressions) const {
        require_plain_projection();
        return expressions.exists(query_);
    }
    [[nodiscard]] db_predicate exists() const {
        require_plain_projection();
        return db_predicate::exists(query_);
    }
    [[nodiscard]] db_predicate not_exists() const {
        require_plain_projection();
        return db_predicate::not_exists(query_);
    }
    template <typename source, typename source_executor_type>
    db_query_builder& combine(db_set_operation operation, const db_query_builder<source, source_executor_type>& source_value) {
        require_plain_projection();
        source_value.require_plain_projection();
        query_.combine(operation, source_value.query_);
        return *this;
    }
    db_query_builder& group_by(std::span<const db_expr> expressions) {
        detail::import_db_expressions(query_, expressions, [this](const db_expr& expression) { return import_entity_expression(expression); }, [this](std::span<const db_expression> imported) { query_.group_by(imported); });
        return *this;
    }
    db_query_builder& group_by(std::initializer_list<db_expr> expressions) {
        return group_by(std::span<const db_expr>(expressions.begin(), expressions.size()));
    }
    db_query_builder& group_by(std::span<const std::string_view> columns) {
        detail::import_db_expressions(query_, columns, [this](std::string_view column) { return import_entity_column(column); }, [this](std::span<const db_expression> imported) { query_.group_by(imported); });
        return *this;
    }
    db_query_builder& group_by(std::initializer_list<std::string_view> columns) {
        return group_by(std::span<const std::string_view>(columns.begin(), columns.size()));
    }
    db_query_builder& add_group_by(std::span<const db_expr> expressions) {
        for (const auto& expression : expressions) {
            query_.add_group_by(import_entity_expression(expression));
        }
        return *this;
    }
    db_query_builder& add_group_by(std::initializer_list<db_expr> expressions) {
        return add_group_by(std::span<const db_expr>(expressions.begin(), expressions.size()));
    }
    db_query_builder& add_group_by(std::string_view column) {
        query_.add_group_by(import_entity_column(column));
        return *this;
    }
    db_query_builder& having(const db_predicate& predicate) {
        query_.having(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& and_having(const db_predicate& predicate) {
        query_.and_having(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& or_having(const db_predicate& predicate) {
        query_.or_having(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& order_by(const db_expr& expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        query_.order_by(expression.expression(query_, entity_type::table_name(), alias_), direction, nulls);
        return *this;
    }
    db_query_builder& add_order_by(const db_expr& expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        query_.add_order_by(expression.expression(query_, entity_type::table_name(), alias_), direction, nulls);
        return *this;
    }
    db_query_builder& order_by(const db_order_term& term) {
        return order_by(term.expression_, term.direction_, term.nulls_);
    }
    db_query_builder& order_by(std::span<const db_order_term> terms) {
        for (const auto& term : terms) {
            if (term.expression_.empty()) {
                throw std::invalid_argument("cannot evaluate an empty database expression");
            }
        }
        query_.clear_order();
        for (const auto& term : terms) {
            add_order_by(term);
        }
        return *this;
    }
    db_query_builder& order_by(std::initializer_list<db_order_term> terms) {
        return order_by(std::span<const db_order_term>(terms.begin(), terms.size()));
    }
    db_query_builder& add_order_by(const db_order_term& term) {
        return add_order_by(term.expression_, term.direction_, term.nulls_);
    }
    db_query_builder& add_order_by(std::span<const db_order_term> terms) {
        for (const auto& term : terms) {
            add_order_by(term);
        }
        return *this;
    }
    db_query_builder& add_order_by(std::initializer_list<db_order_term> terms) {
        return add_order_by(std::span<const db_order_term>(terms.begin(), terms.size()));
    }
    db_query_builder& distinct(bool enabled = true) {
        query_.distinct(enabled);
        return *this;
    }
    db_query_builder& distinct_on(std::span<const db_expr> expressions) {
        detail::import_db_expressions(query_, expressions, [this](const db_expr& expression) { return import_entity_expression(expression); }, [this](std::span<const db_expression> imported) { query_.distinct_on(imported); });
        return *this;
    }
    db_query_builder& distinct_on(std::initializer_list<db_expr> expressions) {
        return distinct_on(std::span<const db_expr>(expressions.begin(), expressions.size()));
    }
    db_query_builder& distinct_on(std::span<const std::string_view> columns) {
        detail::import_db_expressions(query_, columns, [this](std::string_view column) { return import_entity_column(column); }, [this](std::span<const db_expression> imported) { query_.distinct_on(imported); });
        return *this;
    }
    db_query_builder& distinct_on(std::initializer_list<std::string_view> columns) {
        return distinct_on(std::span<const std::string_view>(columns.begin(), columns.size()));
    }
    db_query_builder& where(const db_predicate& predicate) {
        query_.where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& and_where(const db_predicate& predicate) {
        query_.and_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& or_where(const db_predicate& predicate) {
        query_.or_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& clear_where() {
        query_.clear_where();
        return *this;
    }
    db_query_builder& order_by(std::string_view column, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        query_.order_by(import_entity_column(column), direction, nulls);
        return *this;
    }
    db_query_builder& add_order_by(std::string_view column, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        query_.add_order_by(import_entity_column(column), direction, nulls);
        return *this;
    }
    db_query_builder& clear_order() {
        query_.clear_order();
        return *this;
    }
    db_query_builder& offset(std::optional<query_row_count> count) {
        query_.offset(count);
        return *this;
    }
    db_query_builder& limit(std::optional<query_row_count> count) {
        query_.limit(count);
        return *this;
    }
    db_query_builder& with_deleted(bool enabled = true) noexcept {
        with_deleted_ = enabled;
        return *this;
    }
    db_query_builder& left_join_and_select(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        return join_and_select(relation, alias, db_join_type::left, condition.expression(query_, entity_type::table_name(), alias_));
    }
    db_query_builder& inner_join_and_select(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        return join_and_select(relation, alias, db_join_type::inner, condition.expression(query_, entity_type::table_name(), alias_));
    }
    db_query_builder& cache(const db_cache_setting_type& setting) {
        query_.cache(setting);
        return *this;
    }
    db_query_builder& cache(std::string_view id, std::optional<std::chrono::milliseconds> duration = {}) {
        query_.cache(id, duration);
        return *this;
    }
    db_query_builder& set_lock(const db_lock_options& options) {
        query_.lock(options);
        return *this;
    }
    db_query_builder& clear_lock() {
        query_.clear_lock();
        return *this;
    }

    template <typename output_type = entity_type>
    [[nodiscard]] task<entity_rows<output_type>> get_many() const {
        auto mapper = projection_mapper<output_type>();
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        if constexpr (std::same_as<output_type, entity_type>) {
            if (hydrates()) {
                auto prepared = prepare_relation_query(target_query);
                return executor_.template query_mapped<entity_rows<entity_type>>(prepared ? *prepared : target_query,
                    detail::db_map_related_entities<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped<entity_rows<output_type>>(target_query, std::move(mapper));
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::optional<output_type>> get_one() const {
        auto mapper = projection_mapper<output_type>();
        auto query = single_row_query();
        if constexpr (std::same_as<output_type, entity_type>) {
            if (hydrates()) {
                auto prepared = prepare_relation_query(query);
                return executor_.template query_mapped<std::optional<entity_type>>(prepared ? *prepared : query,
                    detail::db_map_one_related_entity<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped<std::optional<output_type>>(query, detail::db_map_one_projection<output_type>{std::move(mapper)});
    }
    [[nodiscard]] task<db_rows> get_raw_many() const {
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        return executor_.template query_mapped<db_rows>(target_query, detail::db_map_raw_rows{});
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::pair<entity_rows<output_type>, db_rows>> get_raw_and_entities() const {
        auto mapper = projection_mapper<output_type>(true);
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        return executor_.template query_mapped<std::pair<entity_rows<output_type>, db_rows>>(
            target_query, detail::db_map_raw_and_entities<output_type>{std::move(mapper)});
    }
    // Scalars own all decoded data; get_scalar returns nullopt for no row or
    // SQL NULL, while get_scalars rejects NULL rows. Arrays remain one-dimensional.
    template <db_mapped_value scalar_type>
    [[nodiscard]] task<std::optional<scalar_type>> get_scalar() const {
        auto query = single_row_query();
        return executor_.template query_mapped<std::optional<scalar_type>>(query, detail::db_map_scalar<scalar_type>{});
    }
    template <db_mapped_value scalar_type>
    [[nodiscard]] task<std::vector<scalar_type>> get_scalars() const {
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        return executor_.template query_mapped<std::vector<scalar_type>>(target_query, detail::db_map_scalars<scalar_type>{});
    }
    [[nodiscard]] task<db_stream_result> stream() const
        requires std::same_as<executor_type, db_session>
    {
        executor_.registration_.require_active();
        auto statement = get_query_and_parameters();
        auto& scope = executor_.registration_.scope();
        return make_scoped_operation(scope,
            db_session::query_stream_prepared(executor_.client_, std::move(statement.sql_),
                std::move(statement.params_), executor_.resource_, scope, executor_.io_metrics_, executor_.options_));
    }
    [[nodiscard]] task<std::uint64_t> get_count() const {
        const auto count = count_query();
        return executor_.template query_mapped<std::uint64_t>(count, detail::db_map_count{});
    }
    [[nodiscard]] task<bool> get_exists() const {
        auto source_value = unpaged_source();
        db_query query;
        query.select(query.alias(query.exists(std::move(source_value)), "exists"));
        query.copy_cache(query_, detail::db_cache_shape::exists);
        return executor_.template query_mapped<bool>(query, detail::db_map_exists{});
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::pair<entity_rows<output_type>, std::uint64_t>> get_many_and_count() const {
        if (query_.has_writes()) {
            throw std::invalid_argument("get_many_and_count cannot execute a write CTE twice");
        }
        auto mapper = projection_mapper<output_type>();
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        auto count = count_query();
        if constexpr (std::same_as<output_type, entity_type>) {
            if (hydrates()) {
                auto prepared = prepare_relation_query(target_query);
                return executor_.template query_mapped_and_count<entity_rows<entity_type>>(prepared ? *prepared : target_query, count,
                    detail::db_map_related_entities<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped_and_count<entity_rows<output_type>>(target_query, count, std::move(mapper));
    }
    [[nodiscard]] db_statement get_query_and_parameters() const {
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        auto prepared = prepare_relation_query(target_query);
        return (prepared ? *prepared : target_query).compile(executor_.query_driver());
    }

private:
    static constexpr std::size_t column_capacity = std::tuple_size_v<typename entity_type::columns_type>;
    friend class db_repository<entity_type, executor_type>;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;
    void require_plain_projection() const {
        if (hydrates()) {
            throw std::invalid_argument("explicit projections and subqueries require joins without relation hydration");
        }
    }
    [[nodiscard]] db_expression import_entity_expression(const db_expr& expression) {
        return expression.expression(query_, entity_type::table_name(), alias_);
    }
    [[nodiscard]] db_expression import_entity_column(std::string_view column) {
        detail::require_db_column(detail::db_entity_columns<entity_type>, column);
        return query_.column(column, alias_);
    }
    template <typename output_type>
    detail::db_map_projection<output_type> projection_mapper(bool allow_extra_columns = false) const {
        if constexpr (!std::same_as<output_type, entity_type>) {
            require_plain_projection();
            if (selected_.empty()) {
                throw std::invalid_argument("DTO mapping requires an explicit projection");
            }
        }
        return detail::db_map_projection<output_type>(selected_.columns_, allow_extra_columns, selected_.wildcard_);
    }
    db_query_builder(detail::db_repository_input_type<executor_type> executor, std::string_view alias)
        : executor_(executor),
          alias_(alias.empty() ? detail::unqualified_db_name(entity_type::table_name()) : alias) {
        detail::select_entity<entity_type>(query_, alias_);
    }
    [[nodiscard]] bool hydrates() const noexcept {
        return relations_ && !relations_->empty();
    }
    [[nodiscard]] const db_query& target(std::optional<db_query>& scratch) const {
        scratch = effective_query();
        return scratch ? *scratch : query_;
    }
    [[nodiscard]] db_query single_row_query() const {
        std::optional<db_query> scratch;
        auto query = target(scratch).clone();
        query.limit(1);
        // An id-keyed cache includes the derived result shape.
        query.copy_cache(query_, detail::db_cache_shape::one);
        return query;
    }
    [[nodiscard]] db_query unpaged_source() const {
        std::optional<db_query> scratch;
        auto query = target(scratch).clone();
        query.clear_order().clear_lock().limit(std::nullopt).offset(std::nullopt);
        return query;
    }
    [[nodiscard]] std::optional<db_query> effective_query() const {
        constexpr auto del_col = detail::delete_date_column<entity_type>();
        if constexpr (!del_col.empty()) {
            if (!with_deleted_) {
                auto filtered = query_.clone();
                const auto qualifier = alias_.empty() ? entity_type::table_name() : std::string_view(alias_);
                filtered.and_where(filtered.unary(db_unary_operator::is_null, filtered.column(del_col, qualifier)));
                return filtered;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<db_query> prepare_relation_query(const db_query& target_query) const {
        if (hydrates()) {
            return relations_->template prepare<entity_type>(target_query, alias_, executor_.query_driver());
        }
        return std::nullopt;
    }
    [[nodiscard]] db_query count_query() const {
        auto source_value = unpaged_source();
        if (hydrates()) {
            db_query roots;
            constexpr auto keys = detail::db_primary_key_columns<entity_type>();
            for (const auto key : keys) {
                roots.add_select(roots.column(key, "__ruvia_count_roots"));
            }
            roots.from(std::move(source_value), "__ruvia_count_roots").distinct();
            source_value = std::move(roots);
        }
        db_query count;
        const std::array args{count.star()};
        count.select(count.alias(count.aggregate("count", args), "count")).from(std::move(source_value), "count_source");
        count.copy_cache(query_, detail::db_cache_shape::count);
        return count;
    }
    db_query_builder& join_and_select(std::string_view relation, std::string_view alias, db_join_type join, db_expression condition = {}) {
        if (query_.has_writes()) {
            throw std::invalid_argument("write CTEs require flat result mapping");
        }
        if (!selected_.empty()) {
            throw std::invalid_argument("relation hydration requires a full entity projection");
        }
        if (alias.empty()) {
            throw std::invalid_argument("relation joins require an alias");
        }
        if (!relations_) {
            relations_.emplace();
        }
        relations_->template add<entity_type>(query_, alias_, relation, alias, join, condition);
        return *this;
    }
    detail::db_repository_executor_type<executor_type> executor_;
    db_query query_;
    std::string alias_;
    detail::db_projection selected_;
    std::optional<detail::db_relation_plan> relations_{};
    bool with_deleted_{false};
};

template <typename entity_type, typename executor_type>
class db_write_query_builder final {
public:
    db_write_query_builder(const db_write_query_builder&) = delete;
    db_write_query_builder& operator=(const db_write_query_builder&) = delete;
    db_write_query_builder(db_write_query_builder&&) = default;
    db_write_query_builder& operator=(db_write_query_builder&&) = delete;

    db_write_query_builder& where(const db_predicate& predicate) {
        require_condition(predicate);
        query_.where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_write_query_builder& and_where(const db_predicate& predicate) {
        require_condition(predicate);
        query_.and_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_write_query_builder& or_where(const db_predicate& predicate) {
        require_condition(predicate);
        query_.or_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_write_query_builder& set(std::string_view column, const db_expr& value) {
        if (!detail::require_db_column(detail::db_entity_columns<entity_type>, column).writable_ || value.empty()) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        query_.set(column, value.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    template <detail::db_parameter v_type>
        requires(!std::same_as<std::remove_cvref_t<v_type>, db_expression> &&
                 !std::same_as<std::remove_cvref_t<v_type>, db_expr>)
    db_write_query_builder& set(std::string_view column, v_type&& value) {
        if (!detail::require_db_column(detail::db_entity_columns<entity_type>, column).writable_) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        if constexpr (std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t>) {
            return set_null(column);
        } else {
            query_.set(column, query_.value(detail::make_immediate_db_parameter(std::forward<v_type>(value))));
            return *this;
        }
    }
    db_write_query_builder& set_null(std::string_view column) {
        const auto& info = detail::require_db_column(detail::db_entity_columns<entity_type>, column);
        if (!info.writable_ || !info.nullable_) {
            throw std::invalid_argument("SET NULL requires a writable nullable entity column");
        }
        query_.set(column, query_.null_value());
        return *this;
    }
    db_write_query_builder& set_now(std::string_view column) {
        if (!detail::require_db_column(detail::db_entity_columns<entity_type>, column).writable_) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        query_.set(column, query_.call("now"));
        return *this;
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    db_write_query_builder& increment(std::string_view column, number_type value = number_type{1}) {
        return adjust_number(column, value, db_binary_operator::add);
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    db_write_query_builder& decrement(std::string_view column, number_type value = number_type{1}) {
        return adjust_number(column, value, db_binary_operator::subtract);
    }
    db_write_query_builder& set(const db_assignment& assignment) {
        query_.set(prepare_assignment(assignment));
        return *this;
    }
    db_write_query_builder& set(std::span<const db_assignment> assignments) {
        if (assignments.empty()) {
            throw std::invalid_argument("SET requires at least one writable column");
        }
        std::array<db_assignment, column_capacity> inline_staged{};
        std::vector<db_assignment> heap_staged;
        std::span<db_assignment> staged;
        if (assignments.size() <= inline_staged.size()) {
            staged = std::span(inline_staged).first(assignments.size());
        } else {
            heap_staged.resize(assignments.size());
            staged = heap_staged;
        }
        for (std::size_t index = 0; index < assignments.size(); ++index) {
            staged[index] = prepare_assignment(assignments[index]);
        }
        query_.set(staged);
        return *this;
    }
    db_write_query_builder& set(std::initializer_list<db_assignment> assignments) {
        return set(std::span<const db_assignment>(assignments.begin(), assignments.size()));
    }
    db_write_query_builder& set(const entity_type& changes) {
        std::array<db_assignment, column_capacity> staged{};
        std::size_t count = 0;
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
            if constexpr (detail::is_writable_db_column<c_type>()) {
                if (changes.template is_set<c_type::name>()) {
                    staged[count++] = {c_type::name.view(), db_expr(detail::entity_column_value<entity_type, c_type>(query_, changes), {}, true)};
                }
            }
        });
        if (count == 0 && detail::update_date_column<entity_type>().empty() && detail::version_column<entity_type>().empty()) {
            throw std::invalid_argument("SET requires at least one writable column");
        }
        query_.set(std::span<const db_assignment>(staged.data(), count));
        return *this;
    }
    template <typename source>
    db_write_query_builder& update_from(std::string_view alias = {}) {
        static_assert(sql_entity<source>, "SQL update sources require a SQL entity");
        query_.update_from(source::table_name(), alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& update_from(const db_query_builder<source, source_executor_type>& source_value, std::string_view alias) {
        source_value.require_plain_projection();
        query_.update_from(source_value.query_, alias);
        return *this;
    }
    db_write_query_builder& update_from_cte(std::string_view name, std::string_view alias = {}) {
        query_.update_from(name, alias);
        return *this;
    }
    template <typename source>
    db_write_query_builder& delete_using(std::string_view alias = {}) {
        static_assert(sql_entity<source>, "SQL delete sources require a SQL entity");
        query_.delete_using(source::table_name(), alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& delete_using(const db_query_builder<source, source_executor_type>& source_value, std::string_view alias) {
        source_value.require_plain_projection();
        query_.delete_using(source_value.query_, alias);
        return *this;
    }
    db_write_query_builder& delete_using_cte(std::string_view name, std::string_view alias = {}) {
        query_.delete_using(name, alias);
        return *this;
    }
    template <typename joined_type>
    db_write_query_builder& join(db_join_type type, std::string_view alias, const db_predicate& on = {}) {
        static_assert(sql_entity<joined_type>, "SQL joins require a SQL entity");
        query_.join(type, joined_type::table_name(), detail::import_join_predicate<joined_type, entity_type>(query_, on, alias_, alias), alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& join(db_join_type type, const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        source_value.require_plain_projection();
        query_.join(type, source_value.query_, detail::import_join_predicate<source, entity_type>(query_, on, alias_, alias), alias, options);
        return *this;
    }
    db_write_query_builder& join(db_join_type type, const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join(type, source_value, on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_write_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, const db_predicate& on = {}) {
        query_.join(type, name, on.expression(query_, entity_type::table_name(), alias_), alias);
        return *this;
    }
    db_write_query_builder& join_function(db_join_type type, db_expression function, std::string_view alias,
        const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join_function(type, query_.import_expression(function), on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_write_query_builder& values(const entity_type& entity) {
        return values(std::span<const entity_type>(&entity, 1));
    }
    db_write_query_builder& values(std::initializer_list<entity_type> entities) {
        return values(std::span<const entity_type>(entities.begin(), entities.size()));
    }
    db_write_query_builder& values(std::span<const entity_type> entities) {
        if (!inserting_) {
            throw std::invalid_argument("values requires an insert builder");
        }
        std::array<std::string_view, column_capacity> columns{};
        std::size_t column_count = 0;
        detail::append_entity_insert_rows(query_, entities, columns, column_count);
        record_inserted_columns(std::span<const std::string_view>(columns.data(), column_count));
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& insert_from(std::span<const std::string_view> columns, const db_query_builder<source, source_executor_type>& source_value) {
        if (!inserting_) {
            throw std::invalid_argument("insert_from requires an insert builder");
        }
        source_value.require_plain_projection();
        source_value.query_.require_select_query();
        detail::validate_insert_columns(detail::db_entity_columns<entity_type>, columns);
        const auto count = source_value.selected_.empty() ? std::tuple_size_v<typename source::columns_type> : source_value.selected_.columns_.size();
        if (!source_value.selected_.wildcard_ && count != columns.size()) {
            throw std::invalid_argument("insert_from projection and target column counts differ");
        }
        query_.insert_into(entity_type::table_name(), columns, alias_).insert_from(source_value.query_);
        inserted_column_count_ = 0;
        record_inserted_columns(columns);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& insert_from(std::initializer_list<std::string_view> columns, const db_query_builder<source, source_executor_type>& source_value) {
        return insert_from(std::span<const std::string_view>(columns.begin(), columns.size()), source_value);
    }
    db_write_query_builder& insert_from(std::span<const std::string_view> columns, const db_query& source_value) {
        if (!inserting_) {
            throw std::invalid_argument("insert_from requires an insert builder");
        }
        source_value.require_select_query();
        detail::validate_insert_columns(detail::db_entity_columns<entity_type>, columns);
        query_.insert_into(entity_type::table_name(), columns, alias_).insert_from(source_value);
        inserted_column_count_ = 0;
        record_inserted_columns(columns);
        return *this;
    }
    db_write_query_builder& insert_from(std::initializer_list<std::string_view> columns, const db_query& source_value) {
        return insert_from(std::span<const std::string_view>(columns.begin(), columns.size()), source_value);
    }
    db_write_query_builder& on_conflict(const db_upsert_options& options) {
        if (!inserting_) {
            throw std::invalid_argument("on_conflict requires an insert builder");
        }
        detail::apply_entity_upsert<entity_type>(
            query_, std::span<const std::string_view>(inserted_columns_.data(), inserted_column_count_),
            options, executor_.query_driver(), qualifier());
        return *this;
    }
    template <typename source_type>
        requires requires(const source_type& source) {
            { source.query() } -> std::same_as<const db_query&>;
        }
    db_write_query_builder& with(std::string_view name, const source_type& source, const db_cte_options& options = {}) {
        return with(name, source.query(), options);
    }
    db_write_query_builder& with(std::string_view name, const db_query& source_value, const db_cte_options& options = {}) {
        query_.with(name, source_value, options);
        return *this;
    }
    db_write_query_builder& with(std::string_view name, const db_values_builder& values,
        db_materialization materialization = db_materialization::default_value) {
        query_.with_columns(name, values.query(), materialization, values.columns());
        return *this;
    }
    db_write_query_builder& returning(std::span<const db_selection> fields = {}) {
        if (fields.empty()) {
            std::array<db_expression, column_capacity> expressions{};
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t index> {
                expressions[index] = query_.column(c_type::name.view(), qualifier());
            });
            query_.returning(expressions);
            selected_.columns_.clear();
            selected_.wildcard_ = false;
        } else {
            for (const auto& field : fields) {
                if (!field.has_expression() && !field.is_wildcard()) {
                    detail::require_db_column(detail::db_entity_columns<entity_type>, field.column_);
                }
            }
            selected_ = detail::apply_projection(query_, fields, qualifier(), true, entity_type::table_name());
        }
        return *this;
    }
    db_write_query_builder& returning(std::initializer_list<db_selection> fields_value) {
        return returning(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    db_write_query_builder& returning(std::span<const std::string_view> columns) {
        return detail::stage_db_selections<column_capacity>(columns,
            [this](std::span<const db_selection> selections) -> db_write_query_builder& { return returning(selections); });
    }
    [[nodiscard]] task<db_exec_result> execute() const {
        validate();
        return executor_.execute_query(query_);
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<entity_rows<output_type>> get_many() const {
        require_returning<output_type>("get_many requires RETURNING");
        return executor_.template query_mapped<entity_rows<output_type>>(query_, detail::db_map_projection<output_type>(selected_.columns_, false, selected_.wildcard_));
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::optional<output_type>> get_one() const {
        require_returning<output_type>("get_one requires RETURNING");
        return executor_.template query_mapped<std::optional<output_type>>(query_,
            detail::db_map_one_projection<output_type>{detail::db_map_projection<output_type>(selected_.columns_, false, selected_.wildcard_)});
    }
    [[nodiscard]] task<db_rows> get_raw_many() const {
        require_returning("get_raw_many requires RETURNING");
        return executor_.template query_mapped<db_rows>(query_, detail::db_map_raw_rows{});
    }
    // RETURNING uses the same owning value and NULL contract as read scalars.
    template <db_mapped_value scalar_type>
    [[nodiscard]] task<std::optional<scalar_type>> get_scalar() const {
        require_returning("get_scalar requires RETURNING");
        return executor_.template query_mapped<std::optional<scalar_type>>(query_, detail::db_map_scalar<scalar_type>{});
    }
    template <db_mapped_value scalar_type>
    [[nodiscard]] task<std::vector<scalar_type>> get_scalars() const {
        require_returning("get_scalars requires RETURNING");
        return executor_.template query_mapped<std::vector<scalar_type>>(query_, detail::db_map_scalars<scalar_type>{});
    }
    [[nodiscard]] db_statement get_query_and_parameters() const {
        validate();
        return query_.compile(executor_.query_driver());
    }
    // Borrows the complete validated DML AST, including RETURNING and parameters.
    // The builder and its resource must outlive the view; with() clones on import.
    [[nodiscard]] const db_query& query() const& RUVIA_LIFETIMEBOUND {
        validate();
        return query_;
    }
    const db_query& query() const&& = delete;

private:
    static constexpr std::size_t column_capacity = std::tuple_size_v<typename entity_type::columns_type>;
    friend class db_repository<entity_type, executor_type>;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;
    db_write_query_builder(detail::db_repository_input_type<executor_type> executor, db_query query, bool inserting,
        std::string_view alias = {}, std::span<const std::string_view> inserted_columns = {})
        : executor_(executor),
          query_(std::move(query)),
          inserting_(inserting),
          alias_(alias) {
        record_inserted_columns(inserted_columns);
    }
    std::string_view qualifier() const {
        return alias_.empty() ? entity_type::table_name() : std::string_view(alias_);
    }
    db_assignment prepare_assignment(const db_assignment& assignment) {
        if (!detail::require_db_column(detail::db_entity_columns<entity_type>, assignment.column_).writable_ || assignment.empty()) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        return {assignment.column_, db_expr(assignment.expression(query_, entity_type::table_name(), alias_), {}, true)};
    }
    template <typename number_type>
    db_write_query_builder& adjust_number(std::string_view column, number_type value, db_binary_operator op) {
        const auto& info = detail::require_db_column(detail::db_entity_columns<entity_type>, column);
        if (!info.writable_ || !info.numeric_) {
            throw std::invalid_argument("increment/decrement requires a writable numeric column");
        }
        query_.set(column, query_.binary(query_.column(column, qualifier()), op, query_.value(value)));
        return *this;
    }
    void record_inserted_columns(std::span<const std::string_view> columns) noexcept {
        for (const auto& column : detail::db_entity_columns<entity_type>) {
            if (column.writable_) {
                const auto end = inserted_columns_.begin() + inserted_column_count_;
                if (std::ranges::find(columns, column.name_) != columns.end() &&
                    std::find(inserted_columns_.begin(), end, column.name_) == end) {
                    inserted_columns_[inserted_column_count_++] = column.name_;
                }
            }
        }
    }
    void require_condition(const db_predicate& condition) const {
        if (inserting_ || condition.empty()) {
            throw std::invalid_argument("write WHERE requires an update/delete builder and a nonempty condition");
        }
    }
    void validate() const {
        if (!inserting_ && !query_.has_where()) {
            throw std::invalid_argument("repository update/delete requires a condition");
        }
    }
    template <typename output_type = entity_type>
    void require_returning(const char* message) const {
        validate();
        if (!query_.returns_rows()) {
            throw std::invalid_argument(message);
        }
        if constexpr (!std::same_as<output_type, entity_type>) {
            if (selected_.empty()) {
                throw std::invalid_argument("DTO returning requires an explicit projection");
            }
        }
    }
    detail::db_repository_executor_type<executor_type> executor_;
    db_query query_;
    bool inserting_;
    std::string alias_;
    detail::db_projection selected_;
    std::array<std::string_view, column_capacity> inserted_columns_{};
    std::size_t inserted_column_count_{0};
};

template <typename entity_type, typename executor_type>
class db_repository final {
    static_assert(sql_entity<entity_type>, "SQL repositories require a RUVIA_DB_ENTITY declaration");

public:
    [[nodiscard]] db_query_builder<entity_type, executor_type> query_builder(std::string_view alias = {}) const {
        return db_query_builder<entity_type, executor_type>(executor_, alias);
    }
    // Update builders start with the lifecycle stamps; set() on update_date_
    // or version_ replaces the stamp.
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> update_builder(std::string_view alias = {}) const {
        db_query query;
        query.update(entity_type::table_name(), alias);
        detail::stamp_lifecycle_columns<entity_type>(query, alias.empty() ? entity_type::table_name() : alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), false, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> delete_builder(std::string_view alias = {}) const {
        db_query query;
        query.delete_from(entity_type::table_name(), alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), false, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> insert_builder(std::string_view alias = {}) const {
        db_query query;
        query.insert_into(entity_type::table_name(), {}, alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), true, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> insert_builder(const entity_type& entity, std::string_view alias = {}) const {
        return insert_builder(std::span<const entity_type>(&entity, 1), alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> insert_builder(std::span<const entity_type> entities, std::string_view alias = {}) const {
        auto inserted = insert_query(entities, alias);
        return db_write_query_builder<entity_type, executor_type>(
            executor_, std::move(inserted.query_), true, alias, inserted.columns());
    }
    [[nodiscard]] task<entity_rows<entity_type>> find(const db_find_options& options = {}) const {
        return find_builder(options).get_many();
    }
    template <std::same_as<db_predicate> predicate_type>
    [[nodiscard]] task<entity_rows<entity_type>> find(const predicate_type& where) const {
        return find(db_find_options{.where_ = where});
    }
    [[nodiscard]] task<std::optional<entity_type>> find_one(const db_find_options& options = {}) const {
        return find_builder(options).get_one();
    }
    template <std::same_as<db_predicate> predicate_type>
    [[nodiscard]] task<std::optional<entity_type>> find_one(const predicate_type& where) const {
        return find_one(db_find_options{.where_ = where});
    }
    [[nodiscard]] task<std::pair<entity_rows<entity_type>, std::uint64_t>> find_and_count(const db_find_options& options = {}) const {
        return find_builder(options).get_many_and_count();
    }
    [[nodiscard]] task<std::uint64_t> count(const db_find_options& options = {}) const {
        return find_builder(options).get_count();
    }
    template <std::same_as<db_predicate> predicate_type>
    [[nodiscard]] task<std::uint64_t> count(const predicate_type& where) const {
        return count(db_find_options{.where_ = where});
    }
    [[nodiscard]] task<bool> exists(const db_find_options& options = {}) const {
        return find_builder(options).get_exists();
    }
    template <std::same_as<db_predicate> predicate_type>
    [[nodiscard]] task<bool> exists(const predicate_type& where) const {
        return exists(db_find_options{.where_ = where});
    }
    [[nodiscard]] task<entity_type> save(const entity_type& entity) const {
        auto builder = save_builder(std::span<const entity_type>(&entity, 1));
        builder.returning();
        return executor_.template query_mapped<entity_type>(
            builder.query(), detail::db_map_required_one<entity_type>{detail::db_map_projection<entity_type>({})});
    }
    [[nodiscard]] task<entity_rows<entity_type>> save(std::span<const entity_type> entities) const {
        return save_builder(entities).returning().get_many();
    }
    [[nodiscard]] task<db_exec_result> soft_delete(const db_predicate& predicate) const
        requires(!detail::delete_date_column<entity_type>().empty())
    {
        if (predicate.empty()) {
            throw std::invalid_argument("repository soft_delete requires a condition");
        }
        auto builder = update_builder();
        builder.set_now(detail::delete_date_column<entity_type>());
        builder.where(predicate);
        return builder.execute();
    }
    [[nodiscard]] task<db_exec_result> restore(const db_predicate& predicate) const
        requires(!detail::delete_date_column<entity_type>().empty())
    {
        if (predicate.empty()) {
            throw std::invalid_argument("repository restore requires a condition");
        }
        auto builder = update_builder();
        builder.set_null(detail::delete_date_column<entity_type>());
        builder.where(predicate);
        return builder.execute();
    }
    [[nodiscard]] task<db_exec_result> clear() const {
        auto sql = detail::format_truncate_table(entity_type::table_name(), executor_.query_driver());
        return executor_.execute_prepared(std::move(sql));
    }
    [[nodiscard]] task<db_exec_result> insert(const entity_type& entity) const {
        return insert(std::span<const entity_type>(&entity, 1));
    }
    [[nodiscard]] task<db_exec_result> insert(std::span<const entity_type> entities) const {
        return insert_builder(entities).execute();
    }
    [[nodiscard]] task<db_exec_result> update(const db_predicate& predicate, const entity_type& changes) const {
        return update_builder().set(changes).where(predicate).execute();
    }
    [[nodiscard]] task<db_exec_result> delete_by(const db_predicate& predicate) const {
        return delete_builder().where(predicate).execute();
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] task<db_exec_result> increment(const db_predicate& predicate, std::string_view property_path, number_type value = number_type{1}) const {
        return update_builder().increment(property_path, value).where(predicate).execute();
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] task<db_exec_result> decrement(const db_predicate& predicate, std::string_view property_path, number_type value = number_type{1}) const {
        return update_builder().decrement(property_path, value).where(predicate).execute();
    }
    [[nodiscard]] task<db_exec_result> remove(const entity_type& entity) const {
        db_query query;
        query.delete_from(entity_type::table_name());
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
            if constexpr (c_type::options.primary_key_) {
                if (!entity.template is_set<c_type::name>() || entity.template is_null<c_type::name>()) {
                    throw std::invalid_argument("remove requires all primary key values");
                }
                query.and_where(query.binary(query.column(c_type::name.view()), db_binary_operator::equal, detail::entity_column_value<entity_type, c_type>(query, entity)));
            }
        });
        if (!query.has_where()) {
            throw std::invalid_argument("remove requires an entity primary key");
        }
        return executor_.execute_query(query);
    }
    [[nodiscard]] task<db_exec_result> upsert(const entity_type& entity, const db_upsert_options& options) const {
        return upsert(std::span<const entity_type>(&entity, 1), options);
    }
    [[nodiscard]] task<db_exec_result> upsert(std::span<const entity_type> entities, const db_upsert_options& options) const {
        auto query = upsert_query(entities, options);
        return executor_.execute_query(query);
    }

    [[nodiscard]] task<db_exec_result> update(const db_predicate& predicate, std::span<const db_assignment> changes) const {
        return update_builder().set(changes).where(predicate).execute();
    }
    [[nodiscard]] task<db_exec_result> update(const db_predicate& predicate, std::initializer_list<db_assignment> changes) const {
        return update(predicate, std::span<const db_assignment>(changes.begin(), changes.size()));
    }

private:
    static constexpr std::size_t column_capacity = std::tuple_size_v<typename entity_type::columns_type>;
    friend class db_session;
    friend class db_transaction;
    explicit db_repository(detail::db_repository_input_type<executor_type> executor)
        : executor_(executor) {}
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> save_builder(std::span<const entity_type> entities) const {
        constexpr auto keys = detail::db_primary_key_columns<entity_type>();
        if constexpr (keys.empty()) {
            return insert_builder(entities);
        } else {
            bool all_keys_set = true;
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
                if constexpr (c_type::options.primary_key_) {
                    for (const auto& entity : entities) {
                        all_keys_set &= entity.template is_set<c_type::name>() && !entity.template is_null<c_type::name>();
                    }
                }
            });
            if (!all_keys_set) {
                return insert_builder(entities);
            }
            auto builder = insert_builder(entities);
            const auto driver = executor_.query_driver();
            std::array<borrowed_text, keys.size()> conflict_columns{};
            std::size_t conflict_count = 0;
            if (driver == db_driver::postgresql) {
                for (const auto key : keys) {
                    conflict_columns[conflict_count++] = key;
                }
            }
            detail::require_uniform_set_columns<entity_type>(entities, {});
            builder.on_conflict(db_upsert_options{
                .conflict_paths_ = std::span<const borrowed_text>(conflict_columns.data(), conflict_count),
                .any_unique_key_ = driver != db_driver::postgresql,
            });
            return builder;
        }
    }
    [[nodiscard]] db_query_builder<entity_type, executor_type> find_builder(const db_find_options& options) const {
        auto builder = query_builder();
        builder.with_deleted(options.with_deleted_);
        if (!options.select_.empty()) {
            detail::stage_db_selections<column_capacity>(options.select_.view(),
                [&builder](std::span<const db_selection> selections) -> auto& { return builder.select(selections); });
        }
        detail::apply_find_options<entity_type>(builder.query_, options, builder.alias_);
        if (!options.relations_.empty()) {
            builder.relations_.emplace();
            for (const auto& path : options.relations_) {
                builder.relations_->template add<entity_type>(builder.query_, builder.alias_, path);
            }
        }
        return builder;
    }
    db_query upsert_query(std::span<const entity_type> entities, const db_upsert_options& options) const {
        auto inserted = insert_query(entities);
        if (!options.do_nothing_ && options.update_columns_.empty()) {
            detail::require_uniform_set_columns<entity_type>(entities, options.update_expressions_.view());
        }
        detail::apply_entity_upsert<entity_type>(inserted.query_, inserted.columns(), options, executor_.query_driver(), entity_type::table_name());
        return std::move(inserted.query_);
    }
    struct insert_query_result final {
        db_query query_;
        std::array<std::string_view, column_capacity> columns_{};
        std::size_t count_{0};
        [[nodiscard]] std::span<const std::string_view> columns() const noexcept {
            return std::span<const std::string_view>(columns_.data(), count_);
        }
    };
    insert_query_result insert_query(std::span<const entity_type> entities, std::string_view alias = {}) const {
        insert_query_result result{db_query{}};
        result.query_.insert_into(entity_type::table_name(), {}, alias);
        detail::append_entity_insert_rows(result.query_, entities, result.columns_, result.count_);
        return result;
    }
    detail::db_repository_executor_type<executor_type> executor_;
};

template <typename entity_type>
db_repository<entity_type, db_session> db_session::repository() const {
    registration_.require_active();
    return db_repository<entity_type, db_session>(*this);
}
template <typename entity_type>
db_repository<entity_type, db_transaction> db_transaction::repository() & {
    return db_repository<entity_type, db_transaction>(*this);
}

}  // namespace ruvia
