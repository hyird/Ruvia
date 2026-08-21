#pragma once

#include "ruvia/web/db/DbTypes.h"
#include "ruvia/core/memory/PmrResource.h"

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
enum class DbMigrationAtomicity : std::uint8_t {
    kTransactional,
    kUnwrapped,
};

struct DbMigrationOptions final {
    BorrowedText id;
    BorrowedText sql;
    DbMigrationAtomicity atomicity{DbMigrationAtomicity::kTransactional};
};

// Immutable migration descriptor borrowing stable application storage. String
// literals and owning-string lvalues preserve constexpr/zero-allocation use;
// owning-string rvalues are rejected before an async run can retain them.
//
// `sql` is exactly one statement. Neither backend accepts more than one per
// call -- libpq's extended protocol refuses multiple commands and the MariaDB
// connection never enables CLIENT_MULTI_STATEMENTS -- so a schema change that
// needs several statements is several migrations. A trailing ';' is allowed.
//
// `id` identifies an applied migration for the rest of the schema's life. It is
// compared with the migrations table's collation, so ids that differ only in
// letter case are rejected up front rather than resolving differently per
// backend.
class DbMigration final {
public:
    constexpr explicit DbMigration(DbMigrationOptions options) noexcept
        : id_(options.id),
          sql_(options.sql),
          atomicity_(options.atomicity) {}

    [[nodiscard]] constexpr std::string_view id() const noexcept {
        return id_.view();
    }

    [[nodiscard]] constexpr std::string_view sql() const noexcept {
        return sql_.view();
    }

    [[nodiscard]] constexpr DbMigrationAtomicity atomicity() const noexcept {
        return atomicity_;
    }

private:
    BorrowedText id_;
    BorrowedText sql_;
    DbMigrationAtomicity atomicity_;
};

struct DbMigratorOptions final {
    std::string table{"ruvia_schema_migrations"};
    std::chrono::seconds lockTimeout{30};
    std::pmr::memory_resource* resource{nullptr};
};

class DbMigrationReport final {
public:
    DbMigrationReport(const DbMigrationReport&) = delete;
    DbMigrationReport& operator=(const DbMigrationReport&) = delete;
    DbMigrationReport(DbMigrationReport&&) noexcept = default;
    DbMigrationReport& operator=(DbMigrationReport&&) = delete;

    [[nodiscard]] std::span<const std::pmr::string> applied() const& noexcept;
    [[nodiscard]] std::span<const std::pmr::string> applied() const&& = delete;
    [[nodiscard]] std::span<const std::pmr::string> skipped() const& noexcept;
    [[nodiscard]] std::span<const std::pmr::string> skipped() const&& = delete;
    [[nodiscard]] bool changed() const noexcept;

private:
    friend class DbMigrator;
    friend class detail::DbMigrationRunner;

    explicit DbMigrationReport(std::pmr::memory_resource* resource);
    DbMigrationReport(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource);

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
// DbConfig's timeouts apply here as they do on a worker: without connectTimeout
// or queryTimeout a stalled backend blocks startup indefinitely.
class DbMigrator final {
public:
    // The configuration and migration-table name are copied into options.resource;
    // their source PMR storage may be released after construction. The supplied
    // resource itself must outlive this migrator.
    explicit DbMigrator(const DbConfig& config, DbMigratorOptions options = {});

    [[nodiscard]] DbMigrationReport migrate(std::span<const DbMigration> migrations) const;

    [[nodiscard]] static DbMigrationReport migrate(const DbConfig& config, std::span<const DbMigration> migrations, DbMigratorOptions options = {});

private:
    DbConfig config_;
    DbMigratorOptions options_;
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
