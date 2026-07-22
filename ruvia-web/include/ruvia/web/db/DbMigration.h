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
class DbMigration final {
public:
    constexpr DbMigration(
        std::string_view id,
        std::string_view sql) noexcept
        : id_(id), sql_(sql) {}

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

    [[nodiscard]] std::span<const std::pmr::string> applied() const & noexcept;
    [[nodiscard]] std::span<const std::pmr::string> applied() const && = delete;
    [[nodiscard]] std::span<const std::pmr::string> skipped() const & noexcept;
    [[nodiscard]] std::span<const std::pmr::string> skipped() const && = delete;
    [[nodiscard]] bool changed() const noexcept;

private:
    friend class DbMigrator;
    friend class detail::DbMigrationRunner;

    explicit DbMigrationReport(std::pmr::memory_resource* resource);
    DbMigrationReport(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource);

    std::pmr::vector<std::pmr::string> applied_;
    std::pmr::vector<std::pmr::string> skipped_;
};

class DbMigrator final {
public:
    explicit DbMigrator(
        DbConfig config,
        DbMigrationOptions options = {},
        std::pmr::memory_resource* resource = nullptr);

    [[nodiscard]] DbMigrationReport migrate(std::span<const DbMigration> migrations) const;

    [[nodiscard]] static DbMigrationReport migrate(
        DbConfig config,
        std::span<const DbMigration> migrations,
        DbMigrationOptions options = {},
        std::pmr::memory_resource* resource = nullptr);

private:
    DbConfig config_;
    DbMigrationOptions options_;
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
