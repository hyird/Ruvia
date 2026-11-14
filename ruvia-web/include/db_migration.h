#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "db_session.h"
#include "event_loop.h"
#include "operation_options.h"
#include "task.h"
#include "timeout_duration.h"

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
    explicit db_migration(db_migration_options options) :
        id_(std::move(options.id_)),
        sql_(std::move(options.sql_)),
        atomicity_(options.atomicity_) {}

    db_migration(const db_migration& other) :
        id_(other.id_),
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
    [[nodiscard]] task<db_migration_report> migrate(std::span<const db_migration> migrations, operation_options operation = {}) const&;
    task<db_migration_report> migrate(std::span<const db_migration>, operation_options = {}) const&& = delete;

private:
    class storage_type;
    struct storage_deleter_type final {
        void operator()(storage_type* storage) const noexcept;
    };
    using storage_owner_type = std::unique_ptr<storage_type, storage_deleter_type>;

    [[nodiscard]] static storage_owner_type make_storage(event_loop loop, const db_config& config, const db_migrator_options& options);

    storage_owner_type storage_;
};

}  // namespace ruvia
