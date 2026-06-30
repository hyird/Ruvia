#pragma once

#include "ruvia/db/DbTypes.h"
#include "ruvia/memory/PmrResource.h"

#include <chrono>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia {

struct DbMigration final {
    std::string_view id;
    std::string_view sql;
};

struct DbMigrationOptions final {
    std::pmr::string table{"ruvia_schema_migrations"};
    std::chrono::seconds lockTimeout{30};
};

class DbMigrationReport final {
public:
    explicit DbMigrationReport(std::pmr::memory_resource* resource = nullptr);

    DbMigrationReport(const DbMigrationReport&) = delete;
    DbMigrationReport& operator=(const DbMigrationReport&) = delete;
    DbMigrationReport(DbMigrationReport&&) noexcept = default;
    DbMigrationReport& operator=(DbMigrationReport&&) noexcept = default;

    [[nodiscard]] std::span<const std::pmr::string> applied() const noexcept;
    [[nodiscard]] std::span<const std::pmr::string> skipped() const noexcept;
    [[nodiscard]] bool changed() const noexcept;

private:
    friend class DbMigrator;
    friend class detail::DbMigrationRunner;

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
