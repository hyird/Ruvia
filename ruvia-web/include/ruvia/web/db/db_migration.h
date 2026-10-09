#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/db/db_types.h"

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
    std::string table_{"ruvia_schema_migrations"};
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
    db_migration_report(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource);

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
