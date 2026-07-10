#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbMigrationValidation.h"
#include "ruvia/web/detail/db/DbUtils.h"
#include "ruvia/web/db/Db.h"

#include "ruvia/core/detail/AsioAwait.h"

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <exception>
#include <future>
#include <memory_resource>
#include <stdexcept>
#include <utility>

namespace ruvia {
namespace {

void appendQuotedIdentifier(std::pmr::string& sql, std::string_view identifier) {
    if (!detail::isValidMigrationTableName(identifier)) {
        throw std::invalid_argument("database migration table must be 1-64 letters, digits or underscores");
    }
    sql.push_back('`');
    sql.append(identifier);
    sql.push_back('`');
}

[[nodiscard]] std::pmr::string buildMigrationLockName(
    const DbConfig& config,
    std::pmr::memory_resource* resource) {
    constexpr std::string_view kPrefix = "ruvia:migrations:";
    std::pmr::string name(resource);
    name.reserve(kPrefix.size() + (!config.database.empty() ? config.database.size() : config.host.size() + 1 + 10));
    name.append(kPrefix);
    if (!config.database.empty()) {
        name.append(config.database);
    } else {
        name.append(config.host);
        name.push_back(':');
        detail::appendDbNumber(name, static_cast<std::uint64_t>(config.port));
    }
    return name;
}

[[nodiscard]] std::pmr::string buildCreateMigrationsTableSql(
    std::string_view table,
    std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 260);
    sql.append("CREATE TABLE IF NOT EXISTS ");
    appendQuotedIdentifier(sql, table);
    sql.append(
        " (id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        " migration_id VARCHAR(190) NOT NULL,"
        " applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " UNIQUE KEY `uk_migration_id` (`migration_id`))"
        " ENGINE=InnoDB");
    return sql;
}

[[nodiscard]] std::pmr::string buildFindMigrationSql(
    std::string_view table,
    std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 50);
    sql.append("SELECT migration_id FROM ");
    appendQuotedIdentifier(sql, table);
    sql.append(" WHERE migration_id = ? LIMIT 1");
    return sql;
}

[[nodiscard]] std::pmr::string buildInsertMigrationSql(
    std::string_view table,
    std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 40);
    sql.append("INSERT INTO ");
    appendQuotedIdentifier(sql, table);
    sql.append(" (migration_id) VALUES (?)");
    return sql;
}

void appendMigrationId(std::pmr::vector<std::pmr::string>& ids, std::string_view id) {
    ids.emplace_back();
    ids.back().assign(id.data(), id.size());
}

}  // namespace

class detail::DbMigrationRunner final {
public:
    [[nodiscard]] static Task<void> run(
        asio::io_context& ioContext,
        DbConfig config,
        std::span<const DbMigration> migrations,
        DbMigrationOptions options,
        DbMigrationReport& report,
        std::pmr::memory_resource* resource) {
        auto* resolved = detail::pmrResourceOrDefault(resource);
        detail::validateMigrationList(migrations);
        if (!detail::isValidMigrationTableName(options.table)) {
            throw std::invalid_argument("database migration table must be 1-64 letters, digits or underscores");
        }

        config.poolSize = 1;
        if (config.acquireTimeout.count() == 0) {
            config.acquireTimeout = config.queryTimeout;
        }
        validateDbConfig(config);

        auto lockName = buildMigrationLockName(config, resolved);
        const detail::DbDefinition databases[] = {
            detail::DbDefinition{
                std::pmr::string(
                    detail::kDefaultDbAlias.data(),
                    detail::kDefaultDbAlias.size(),
                    resolved),
                std::move(config)}
        };
        detail::DbRegistry registry(ioContext, resolved, databases);
        co_await registry.connect();
        auto handle = registry.get(resolved);

        const auto lockSeconds = static_cast<std::int64_t>(options.lockTimeout.count());
        std::array<DbValue, 2> lockParams{DbValue{std::string_view(lockName)}, DbValue{lockSeconds}};
        auto lockResult = co_await handle.query(
            "SELECT GET_LOCK(?, ?)",
            std::span<const DbValue>(lockParams));
        if (lockResult.rows().size() != 1 ||
            lockResult.rows()[0].empty() ||
            lockResult.rows()[0][0].text() != "1") {
            registry.closeNow();
            throw std::runtime_error("database migration lock could not be acquired");
        }

        std::exception_ptr failure;
        try {
            (void)co_await handle.execute(buildCreateMigrationsTableSql(options.table, resolved));

            auto findSql = buildFindMigrationSql(options.table, resolved);
            auto insertSql = buildInsertMigrationSql(options.table, resolved);
            for (const auto& migration : migrations) {
                std::array<DbValue, 1> findParams{DbValue{migration.id}};
                auto existing = co_await handle.query(findSql, std::span<const DbValue>(findParams));
                if (!existing.rows().empty()) {
                    appendMigrationId(report.skipped_, migration.id);
                    continue;
                }

                (void)co_await handle.execute(migration.sql);
                std::array<DbValue, 1> insertParams{DbValue{migration.id}};
                (void)co_await handle.execute(insertSql, std::span<const DbValue>(insertParams));
                appendMigrationId(report.applied_, migration.id);
            }
        } catch (...) {
            failure = std::current_exception();
        }

        try {
            std::array<DbValue, 1> releaseParams{DbValue{std::string_view(lockName)}};
            (void)co_await handle.execute(
                "DO RELEASE_LOCK(?)",
                std::span<const DbValue>(releaseParams));
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }

        registry.closeNow();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        co_return;
    }
};

DbMigrator::DbMigrator(
    DbConfig config,
    DbMigrationOptions options,
    std::pmr::memory_resource* resource)
    : config_(std::move(config)),
      options_(std::move(options)),
      resource_(detail::pmrResourceOrDefault(resource)) {}

DbMigrationReport DbMigrator::migrate(std::span<const DbMigration> migrations) const {
    return migrate(config_, migrations, options_, resource_);
}

DbMigrationReport DbMigrator::migrate(
    DbConfig config,
    std::span<const DbMigration> migrations,
    DbMigrationOptions options,
    std::pmr::memory_resource* resource) {
    auto* resolved = detail::pmrResourceOrDefault(resource);
    DbMigrationReport report(resolved);
    asio::io_context ioContext(1);
    auto future = asio::co_spawn(
        ioContext,
        detail::taskAsAwaitable(detail::DbMigrationRunner::run(
            ioContext,
            std::move(config),
            migrations,
            std::move(options),
            report,
            resource)),
        asio::use_future);
    ioContext.run();
    future.get();
    return report;
}

}  // namespace ruvia
