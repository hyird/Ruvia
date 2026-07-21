#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbMigrationValidation.h"
#include "ruvia/web/detail/db/DbUtils.h"
#include "ruvia/web/db/Db.h"

#include "ruvia/core/detail/AsioAwait.h"

#include <asio/bind_executor.hpp>

#include <array>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ruvia {
namespace {

void appendQuotedIdentifier(
    std::pmr::string& sql,
    std::string_view identifier,
    DbDriver driver) {
    if (!detail::isValidMigrationTableName(identifier, driver)) {
        throw std::invalid_argument(
            "database migration table has an invalid backend identifier");
    }
    const auto quote = driver == DbDriver::kPostgreSql ? '"' : '`';
    sql.push_back(quote);
    sql.append(identifier);
    sql.push_back(quote);
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
    DbDriver driver,
    std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 260);
    sql.append("CREATE TABLE IF NOT EXISTS ");
    appendQuotedIdentifier(sql, table, driver);
    sql.append(
        " (migration_id VARCHAR(190) PRIMARY KEY,"
        " applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " CONSTRAINT ");
    appendQuotedIdentifier(sql, "ruvia_migration_id_nonempty", driver);
    sql.append(" CHECK (migration_id <> ''))");
    return sql;
}

[[nodiscard]] std::pmr::string buildFindMigrationSql(
    std::string_view table,
    DbDriver driver,
    std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 50);
    sql.append("SELECT migration_id FROM ");
    appendQuotedIdentifier(sql, table, driver);
    sql.append(driver == DbDriver::kPostgreSql
        ? " WHERE migration_id = $1 LIMIT 1"
        : " WHERE migration_id = ? LIMIT 1");
    return sql;
}

[[nodiscard]] std::pmr::string buildInsertMigrationSql(
    std::string_view table,
    DbDriver driver,
    std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 40);
    sql.append("INSERT INTO ");
    appendQuotedIdentifier(sql, table, driver);
    sql.append(driver == DbDriver::kPostgreSql
        ? " (migration_id) VALUES ($1)"
        : " (migration_id) VALUES (?)");
    return sql;
}

void appendMigrationId(std::pmr::vector<std::pmr::string>& ids, std::string_view id) {
    ids.emplace_back();
    ids.back().assign(id.data(), id.size());
}

}  // namespace

class detail::DbMigrationRunner final {
public:
    [[nodiscard]] static Task<DbMigrationReport> run(
        asio::io_context& ioContext,
        DbConfig config,
        std::span<const DbMigration> migrations,
        DbMigrationOptions options,
        std::pmr::memory_resource* resource) {
        auto* resolved = detail::pmrResourceOrDefault(resource);
        detail::validateMigrationList(migrations);
        if (!detail::isValidMigrationTableName(options.table, config.driver)) {
            throw std::invalid_argument(
                "database migration table has an invalid backend identifier");
        }
        if (options.lockTimeout.count() <= 0) {
            throw std::invalid_argument(
                "database migration lock timeout must be greater than zero");
        }

        if (!config.acquireTimeout.has_value()) {
            config.acquireTimeout = config.queryTimeout;
        }
        validateDbConfig(config);
        const auto driver = config.driver;
        DbMigrationReport report(resolved);

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
        detail::ScopedOperationScope operationScope;
        auto handle = registry.get(resolved, operationScope);

        if (driver == DbDriver::kMariaDb) {
            const auto lockSeconds =
                static_cast<std::int64_t>(options.lockTimeout.count());
            std::array<DbValue, 2> lockParams{
                DbValue{std::string_view(lockName)}, DbValue{lockSeconds}};
            auto lockResult = co_await handle.query(
                "SELECT GET_LOCK(?, ?)",
                std::span<const DbValue>(lockParams));
            if (lockResult.rows().size() != 1 ||
                lockResult.rows()[0].empty() ||
                lockResult.rows()[0][0].text() != "1") {
                registry.closeNow();
                throw std::runtime_error(
                    "database migration lock could not be acquired");
            }
        } else {
            std::pmr::string timeoutSql("SET lock_timeout TO '", resolved);
            detail::appendDbNumber(
                timeoutSql,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        options.lockTimeout).count()));
            timeoutSql.append("ms'");
            (void)co_await handle.execute(timeoutSql);
            std::array<DbValue, 1> lockParams{
                DbValue{std::string_view(lockName)}};
            (void)co_await handle.query(
                "SELECT pg_advisory_lock(hashtextextended($1, 0))",
                std::span<const DbValue>(lockParams));
        }

        std::exception_ptr failure;
        try {
            (void)co_await handle.execute(buildCreateMigrationsTableSql(
                options.table, driver, resolved));

            auto findSql = buildFindMigrationSql(
                options.table, driver, resolved);
            auto insertSql = buildInsertMigrationSql(
                options.table, driver, resolved);
            for (const auto& migration : migrations) {
                std::array<DbValue, 1> findParams{DbValue{migration.id()}};
                auto existing = co_await handle.query(findSql, std::span<const DbValue>(findParams));
                if (!existing.rows().empty()) {
                    appendMigrationId(report.skipped_, migration.id());
                    continue;
                }

                (void)co_await handle.execute(migration.sql());
                std::array<DbValue, 1> insertParams{DbValue{migration.id()}};
                (void)co_await handle.execute(insertSql, std::span<const DbValue>(insertParams));
                appendMigrationId(report.applied_, migration.id());
            }
        } catch (...) {
            failure = std::current_exception();
        }

        try {
            std::array<DbValue, 1> releaseParams{
                DbValue{std::string_view(lockName)}};
            if (driver == DbDriver::kMariaDb) {
                (void)co_await handle.execute(
                    "DO RELEASE_LOCK(?)",
                    std::span<const DbValue>(releaseParams));
            } else {
                (void)co_await handle.execute(
                    "SELECT pg_advisory_unlock(hashtextextended($1, 0))",
                    std::span<const DbValue>(releaseParams));
            }
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }

        registry.closeNow();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        co_return std::move(report);
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
    asio::io_context ioContext(1);
    std::optional<DbMigrationReport> report;
    std::exception_ptr exception;
    detail::asyncStartTask(
        detail::DbMigrationRunner::run(
            ioContext,
            std::move(config),
            migrations,
            std::move(options),
            resource),
        asio::bind_executor(
            ioContext.get_executor(),
            [&report, &exception](
                detail::TaskCompletionResult<DbMigrationReport> completion) {
                if (const auto* failure = completion.failure()) {
                    exception = failure->exception();
                } else {
                    report.emplace(
                        std::move(*completion.success()).takeValue());
                }
            }));
    ioContext.run();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (!report.has_value()) {
        throw std::logic_error(
            "database migration produced no report or exception");
    }
    return std::move(*report);
}

}  // namespace ruvia
