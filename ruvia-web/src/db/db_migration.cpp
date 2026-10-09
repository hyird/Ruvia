#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>

#include <asio/io_context.hpp>
#include <openssl/evp.h>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_config_validation.h"
#include "db/db_migration_checksum.h"
#include "db/db_migration_validation.h"
#include "db/db_registry.h"
#include "integration/named_capability.h"

namespace ruvia {
namespace {

struct db_migrator_options_storage final {
    db_migrator_options_storage(const db_migrator_options& source_value, std::pmr::memory_resource* resource)
        : table_(source_value.table_, resource),
          lock_timeout_(source_value.lock_timeout_) {}

    db_migrator_options_storage(
        const db_migrator_options_storage& source_value, std::pmr::memory_resource* resource)
        : table_(source_value.table_, resource),
          lock_timeout_(source_value.lock_timeout_) {}

    std::pmr::string table_;
    std::chrono::seconds lock_timeout_;
};

void validate_db_migrator_options(const db_migrator_options& options, db_driver driver) {
    if (!detail::is_valid_migration_table_name(options.table_, driver)) {
        throw std::invalid_argument("database migration table has an invalid backend identifier");
    }
    if (options.lock_timeout_.count() <= 0) {
        throw std::invalid_argument("database migration lock timeout must be greater than zero");
    }
    if (driver == db_driver::postgresql) {
        // PostgreSQL receives this value in milliseconds, and a valid seconds
        // duration can still overflow that representation during conversion.
        (void)detail::postgres_lock_timeout_milliseconds(options.lock_timeout_);
    }
}

void append_quoted_identifier(std::pmr::string& sql, std::string_view identifier, db_driver driver) {
    if (!detail::is_valid_migration_table_name(identifier, driver)) {
        throw std::invalid_argument("database migration table has an invalid backend identifier");
    }
    const auto quote = driver == db_driver::postgresql ? '"' : '`';
    sql.push_back(quote);
    sql.append(identifier);
    sql.push_back(quote);
}

[[nodiscard]] std::pmr::string build_migration_lock_name(
    const detail::db_config_storage& config, std::pmr::memory_resource* resource) {
    constexpr std::string_view prefix = "ruvia:migrations:";
    std::pmr::string name(resource);
    name.reserve(prefix.size() +
                 (!config.database_.empty() ? config.database_.size() : config.host_.size() + 1 + 10));
    name.append(prefix);
    if (!config.database_.empty()) {
        name.append(config.database_);
    } else {
        name.append(config.host_);
        name.push_back(':');
        detail::append_db_number(name, static_cast<std::uint64_t>(config.port_));
    }
    return name;
}

[[nodiscard]] std::pmr::string build_create_migrations_table_sql(
    std::string_view table_value, db_driver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table_value.size() + 260);
    sql.append("CREATE TABLE IF NOT EXISTS ");
    append_quoted_identifier(sql, table_value, driver);
    if (driver == db_driver::postgresql) {
        // timestamptz records the instant; PostgreSQL's plain timestamp would
        // record a wall-clock reading whose zone nobody wrote down.
        sql.append(
            " (migration_id VARCHAR(190) PRIMARY KEY,"
            " applied_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,");
    } else {
        // A binary collation keeps ids that differ only in letter case
        // distinct, matching PostgreSQL. It is still PAD SPACE, so trailing
        // spaces remain invisible to a comparison here; ids carrying them are
        // refused before they reach this table. DATETIME rather than TIMESTAMP:
        // TIMESTAMP is a 32-bit epoch that stops in 2038 and is rewritten
        // across session time zones.
        sql.append(
            " (migration_id VARCHAR(190) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin PRIMARY KEY,"
            " applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,");
    }
    // The CHECK is deliberately unnamed: MySQL scopes CHECK constraint names to
    // the schema, so a fixed name would collide the moment a second migration
    // table is created in one database.
    sql.append(" checksum CHAR(64), CHECK (migration_id <> ''))");
    return sql;
}

// Tables created before migrations were checksummed have no column for one.
// Asking the catalogue is the portable way to find out: ALTER TABLE ... ADD
// COLUMN IF NOT EXISTS is a MariaDB and PostgreSQL extension that MySQL does
// not accept, and running an unguarded ALTER would fail on every later run.
[[nodiscard]] std::pmr::string build_checksum_column_probe_sql(
    db_driver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(
        "SELECT 1 FROM information_schema.columns WHERE table_schema = ", resource);
    sql.append(driver == db_driver::postgresql ? "current_schema() AND table_name = $1"
                                               : "DATABASE() AND table_name = ?");
    sql.append(" AND column_name = 'checksum'");
    return sql;
}

[[nodiscard]] std::pmr::string build_add_checksum_column_sql(
    std::string_view table_value, db_driver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table_value.size() + 48);
    sql.append("ALTER TABLE ");
    append_quoted_identifier(sql, table_value, driver);
    sql.append(" ADD COLUMN checksum CHAR(64)");
    return sql;
}

[[nodiscard]] std::pmr::string build_find_migration_sql(
    std::string_view table_value, db_driver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table_value.size() + 50);
    sql.append("SELECT checksum FROM ");
    append_quoted_identifier(sql, table_value, driver);
    sql.append(driver == db_driver::postgresql ? " WHERE migration_id = $1 LIMIT 1"
                                               : " WHERE migration_id = ? LIMIT 1");
    return sql;
}

[[nodiscard]] std::pmr::string build_insert_migration_sql(
    std::string_view table_value, db_driver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table_value.size() + 40);
    sql.append("INSERT INTO ");
    append_quoted_identifier(sql, table_value, driver);
    sql.append(driver == db_driver::postgresql ? " (migration_id, checksum) VALUES ($1, $2)"
                                               : " (migration_id, checksum) VALUES (?, ?)");
    return sql;
}

// A row written before checksums were recorded carries none. Adopting the
// current text as that row's baseline is the only choice available -- whatever
// ran back then is unknowable -- and it means the next edit is caught.
[[nodiscard]] std::pmr::string build_adopt_checksum_sql(
    std::string_view table_value, db_driver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table_value.size() + 72);
    sql.append("UPDATE ");
    append_quoted_identifier(sql, table_value, driver);
    sql.append(driver == db_driver::postgresql ? " SET checksum = $1 WHERE migration_id = $2"
                                               : " SET checksum = ? WHERE migration_id = ?");
    return sql;
}

void append_migration_id(std::pmr::vector<std::pmr::string>& ids, std::string_view id) {
    ids.emplace_back();
    ids.back().assign(id.data(), id.size());
}

[[nodiscard]] std::runtime_error migration_drift(
    std::string_view id, std::pmr::memory_resource* resource) {
    std::pmr::string message("database migration '", resource);
    message.append(id);
    message.append("' was edited after it was applied; its recorded checksum no longer matches");
    return std::runtime_error(message.c_str());
}

}  // namespace

std::pmr::string detail::migration_checksum(
    std::string_view sql, std::pmr::memory_resource* resource) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    std::size_t digest_size = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, sql.data(), sql.size(), digest.data(), &digest_size) !=
            1 ||
        digest_size * 2 != migration_checksum_size) {
        throw std::runtime_error("database migration checksum could not be computed");
    }

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::pmr::string hex(detail::pmr_resource_or_default(resource));
    hex.reserve(migration_checksum_size);
    for (std::size_t i = 0; i < digest_size; ++i) {
        hex.push_back(hex_digits[digest[i] >> 4]);
        hex.push_back(hex_digits[digest[i] & 0x0F]);
    }
    return hex;
}

class detail::db_migration_runner final {
public:
    [[nodiscard]] static task<db_migration_report> run(asio::io_context& io_context,
        const worker_handle& worker_value, db_config_storage config, std::span<const db_migration> migrations,
        db_migrator_options_storage options, std::pmr::memory_resource* resource) {
        auto* resolved = detail::pmr_resource_or_default(resource);
        const auto driver = config.driver_;

        if (!config.acquire_timeout_.has_value()) {
            config.acquire_timeout_ = config.query_timeout_;
        }
        db_migration_report report(resolved);

        auto lock_name = build_migration_lock_name(config, resolved);
        const detail::db_definition databases[] = {
            detail::db_definition{std::pmr::string(detail::default_capability_alias.data(),
                                      detail::default_capability_alias.size(), resolved),
                std::move(config)}};
        detail::db_registry registry(io_context, worker_value, resolved, databases);
        co_await registry.connect();
        ::ruvia::operation_scope operation_scope;
        auto handle = registry.get(operation_scope);

        co_await acquire_lock(handle, driver, lock_name, options.lock_timeout_, resolved);

        std::exception_ptr failure;
        try {
            co_await apply_migrations(handle, driver, migrations, options, report, resolved);
        } catch (...) {
            failure = std::current_exception();
        }

        // Release explicitly only after a clean run. A failed statement has
        // already closed the connection, and both lock flavours are held by the
        // session, so the lock is gone with it; issuing the release now would
        // silently reconnect and run it on a session that never held the lock
        // -- a wasted round trip that PostgreSQL answers with a "you don't own
        // a lock" warning. close_now() below covers the unreleased case.
        if (failure == nullptr) {
            try {
                co_await release_lock(handle, driver, lock_name);
            } catch (...) {
                failure = std::current_exception();
            }
        }

        registry.close_now();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        co_return std::move(report);
    }

private:
    // Serializes concurrent deployers. Both locks are held by the session, so
    // losing the connection releases them -- including the migration's own
    // failure path, which closes it.
    [[nodiscard]] static task<void> acquire_lock(db_handle& handle, db_driver driver,
        std::string_view lock_name, std::chrono::seconds lock_timeout,
        std::pmr::memory_resource* resource) {
        if (driver == db_driver::mariadb) {
            const auto lock_seconds = static_cast<std::int64_t>(lock_timeout.count());
            std::array<db_value, 2> lock_params{db_value{lock_name}, db_value{lock_seconds}};
            auto lock_result = co_await handle.query(
                "SELECT GET_LOCK(?, ?)", std::span<const db_value>(lock_params));
            // GET_LOCK answers 0 on timeout and NULL on error rather than
            // failing the statement, so the wait has to be read out of the row.
            if (lock_result.size() != 1 || lock_result[0].empty() ||
                lock_result[0][0].as<bool>() != true) {
                throw std::runtime_error("database migration lock could not be acquired");
            }
            co_return;
        }

        // PostgreSQL's advisory lock waits without a bound of its own; the
        // session's lock_timeout is what ends that wait.
        std::pmr::string timeout_sql("SET lock_timeout TO '", resource);
        detail::append_db_number(timeout_sql, detail::postgres_lock_timeout_milliseconds(lock_timeout));
        timeout_sql.append("ms'");
        (void)co_await handle.execute(timeout_sql);
        std::array<db_value, 1> lock_params{db_value{lock_name}};
        (void)co_await handle.query("SELECT pg_advisory_lock(hashtextextended($1, 0))",
            std::span<const db_value>(lock_params));
    }

    [[nodiscard]] static task<void> release_lock(
        db_handle& handle, db_driver driver, std::string_view lock_name) {
        std::array<db_value, 1> release_params{db_value{lock_name}};
        if (driver == db_driver::mariadb) {
            (void)co_await handle.execute(
                "DO RELEASE_LOCK(?)", std::span<const db_value>(release_params));
        } else {
            (void)co_await handle.query("SELECT pg_advisory_unlock(hashtextextended($1, 0))",
                std::span<const db_value>(release_params));
        }
    }

    [[nodiscard]] static task<void> apply_migrations(db_handle& handle, db_driver driver,
        std::span<const db_migration> migrations, const db_migrator_options_storage& options,
        db_migration_report& report, std::pmr::memory_resource* resource) {
        (void)co_await handle.execute(
            build_create_migrations_table_sql(options.table_, driver, resource));

        std::array<db_value, 1> table_params{db_value{std::string_view(options.table_)}};
        auto checksum_column = co_await handle.query(
            build_checksum_column_probe_sql(driver, resource), std::span<const db_value>(table_params));
        if (checksum_column.empty()) {
            (void)co_await handle.execute(
                build_add_checksum_column_sql(options.table_, driver, resource));
        }

        auto find_sql = build_find_migration_sql(options.table_, driver, resource);
        auto insert_sql = build_insert_migration_sql(options.table_, driver, resource);
        auto adopt_sql = build_adopt_checksum_sql(options.table_, driver, resource);
        for (const auto& migration : migrations) {
            const auto checksum = detail::migration_checksum(migration.sql(), resource);
            std::array<db_value, 1> find_params{db_value{migration.id()}};
            auto existing = co_await handle.query(find_sql, std::span<const db_value>(find_params));
            if (!existing.empty()) {
                const auto& recorded = existing[0][0];
                const auto value = recorded.value();
                if (!value || value->empty()) {
                    std::array<db_value, 2> adopt_params{
                        db_value{std::string_view(checksum)}, db_value{migration.id()}};
                    (void)co_await handle.execute(adopt_sql, std::span<const db_value>(adopt_params));
                } else if (*value != std::string_view(checksum)) {
                    throw migration_drift(migration.id(), resource);
                }
                append_migration_id(report.skipped_, migration.id());
                continue;
            }

            std::array<db_value, 2> insert_params{
                db_value{migration.id()}, db_value{std::string_view(checksum)}};
            // On PostgreSQL the statement and the row recording it commit
            // together, so an interruption between them cannot leave the schema
            // changed and unrecorded. MariaDB commits DDL implicitly, so there
            // is no transaction to put them in and the two-statement window
            // stands.
            if (driver == db_driver::postgresql &&
                migration.atomicity() == db_migration_atomicity::transactional) {
                auto transaction = co_await handle.begin_transaction();
                (void)co_await transaction.execute(migration.sql());
                (void)co_await transaction.execute(
                    insert_sql, std::span<const db_value>(insert_params));
                co_await transaction.commit();
            } else {
                (void)co_await handle.execute(migration.sql());
                (void)co_await handle.execute(insert_sql, std::span<const db_value>(insert_params));
            }
            append_migration_id(report.applied_, migration.id());
        }
    }
};

class db_migrator::storage_type final {
public:
    storage_type(detail::validated_db_config_view config_source_value, const db_migrator_options& options_source,
        std::pmr::memory_resource* storage_resource)
        : resource_(storage_resource),
          config_(config_source_value, storage_resource),
          options_(options_source, storage_resource) {}

    std::pmr::memory_resource* resource_;
    detail::db_config_storage config_;
    db_migrator_options_storage options_;
};

void db_migrator::storage_deleter_type::operator()(storage_type* storage) const noexcept {
    detail::destroy_pmr_object(storage, resource_);
}

namespace {

task<db_migration_report> stop_migration_loop_when_done(
    event_loop_attachment& attachment, task<db_migration_report> operation) {
    try {
        auto report = co_await std::move(operation);
        attachment.stop();
        co_return report;
    } catch (...) {
        attachment.stop();
        throw;
    }
}

[[nodiscard]] db_migration_report run_migrations(detail::db_config_storage config,
    std::span<const db_migration> migrations, db_migrator_options_storage options,
    std::pmr::memory_resource* resource) {
    asio::io_context io_context(1);
    auto attachment = attach_event_loop(io_context);
    const auto loop = attachment.loop();
    const auto worker_value = loop.handle();
    auto result_value = loop.start(stop_migration_loop_when_done(
        attachment, detail::db_migration_runner::run(io_context, worker_value, std::move(config), migrations,
                        std::move(options), resource)));
    io_context.run();
    return result_value.get();
}

}  // namespace

db_migrator::storage_owner_type db_migrator::make_storage(
    const db_config& config, const db_migrator_options& options) {
    auto* resource = detail::pmr_resource_or_default(options.resource_);
    const auto validated_config = detail::validated_db_config(config);
    validate_db_migrator_options(options, validated_config.get().driver_);
    return storage_owner_type(
        detail::construct_pmr_object<storage_type>(resource, validated_config, options, resource),
        storage_deleter_type{resource});
}

db_migrator::db_migrator(const db_config& config, const db_migrator_options& options)
    : storage_(make_storage(config, options)) {}

db_migrator::~db_migrator() = default;

db_migrator::db_migrator(db_migrator&&) noexcept = default;

db_migrator& db_migrator::operator=(db_migrator&&) noexcept = default;

db_migration_report db_migrator::migrate(std::span<const db_migration> migrations) const {
    if (storage_ == nullptr) {
        throw std::logic_error("database migrator has been moved from");
    }
    detail::validate_migration_list(migrations, storage_->config_.driver_);
    return run_migrations(detail::db_config_storage(storage_->config_, storage_->resource_), migrations,
        db_migrator_options_storage(storage_->options_, storage_->resource_), storage_->resource_);
}

db_migration_report db_migrator::migrate(const db_config& config,
    std::span<const db_migration> migrations, const db_migrator_options& options) {
    auto* resource = detail::pmr_resource_or_default(options.resource_);
    const auto validated_config = detail::validated_db_config(config);
    validate_db_migrator_options(options, validated_config.get().driver_);
    detail::validate_migration_list(migrations, validated_config.get().driver_);
    return run_migrations(detail::db_config_storage(validated_config, resource), migrations,
        db_migrator_options_storage(options, resource), resource);
}

}  // namespace ruvia
