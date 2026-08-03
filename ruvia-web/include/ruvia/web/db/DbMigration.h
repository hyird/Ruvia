#pragma once

#include "ruvia/web/db/DbTypes.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/detail/util/BorrowedView.h"

#include <chrono>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia {

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
    constexpr DbMigration(std::string_view id, std::string_view sql) noexcept
        : id_(id),
          sql_(sql) {}

    template <detail::HttpTemporaryOwningCharString String>
    DbMigration(String&&, std::string_view) = delete;

    template <detail::HttpTemporaryOwningCharString String>
    DbMigration(std::string_view, String&&) = delete;

    [[nodiscard]] constexpr std::string_view id() const noexcept {
        return id_;
    }

    [[nodiscard]] constexpr std::string_view sql() const noexcept {
        return sql_;
    }

private:
    std::string_view id_;
    std::string_view sql_;
};

struct DbMigrationOptions final {
    std::pmr::string table{"ruvia_schema_migrations"};
    std::chrono::seconds lockTimeout{30};
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
// Each migration and the row that records it are separate statements: a crash
// between them leaves the change applied and unrecorded, and the next run
// retries it. Migrations that can be re-applied safely -- CREATE TABLE IF NOT
// EXISTS and friends -- survive that; ones that cannot need a guard of their
// own.
//
// DbConfig's timeouts apply here as they do on a worker: without connectTimeout
// or queryTimeout a stalled backend blocks startup indefinitely.
class DbMigrator final {
public:
    explicit DbMigrator(DbConfig config, DbMigrationOptions options = {}, std::pmr::memory_resource* resource = nullptr);

    [[nodiscard]] DbMigrationReport migrate(std::span<const DbMigration> migrations) const;

    [[nodiscard]] static DbMigrationReport migrate(DbConfig config, std::span<const DbMigration> migrations, DbMigrationOptions options = {}, std::pmr::memory_resource* resource = nullptr);

private:
    DbConfig config_;
    DbMigrationOptions options_;
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
