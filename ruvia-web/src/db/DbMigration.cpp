#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbMigrationChecksum.h"
#include "ruvia/web/detail/db/DbMigrationValidation.h"
#include "ruvia/web/detail/db/DbUtils.h"
#include "ruvia/web/db/Db.h"

#include "ruvia/core/detail/io/AsioAwait.h"

#include <asio/bind_executor.hpp>
#include <asio/steady_timer.hpp>

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ruvia {
namespace detail {

// A migration runs on its own io_context, not on a worker, so nothing here is
// driving the connection scanner an App installs. Without one every deadline a
// pool arms is armed and never expires: OperationDeadline only becomes expired
// when something calls expire(), and only the scanner does. The socket wait
// under a stalled server then has nobody to cancel it, and migrate() blocks its
// caller -- normally main(), before the service is listening -- for as long as
// the server stays silent, whatever connectTimeout said.
//
// The scanner lives beside the io_context rather than inside the coroutine so
// its lifetime cannot end while a tick is queued: the registry pointer is
// cleared before the registry dies, and a tick that already ran finds nothing
// to scan instead of a destroyed one.
class DbMigrationDeadlineScanner final {
public:
    explicit DbMigrationDeadlineScanner(asio::io_context& ioContext)
        : timer_(ioContext) {}

    DbMigrationDeadlineScanner(const DbMigrationDeadlineScanner&) = delete;
    DbMigrationDeadlineScanner& operator=(const DbMigrationDeadlineScanner&) = delete;

    // Held by the coroutine for exactly as long as the registry it scans is
    // alive, so an exception unwinding the migration detaches it too.
    class Attachment final {
    public:
        Attachment(DbMigrationDeadlineScanner& scanner, DbRegistry& registry) noexcept
            : scanner_(&scanner) {
            scanner.attach(registry);
        }

        Attachment(const Attachment&) = delete;
        Attachment& operator=(const Attachment&) = delete;

        ~Attachment() {
            scanner_->detach();
        }

    private:
        DbMigrationDeadlineScanner* scanner_;
    };

private:
    // Deadlines are configured in milliseconds, so the tick has to be well
    // under the shortest one anybody would set. This runs only while a
    // migration is in flight.
    static constexpr auto kTick = std::chrono::milliseconds(20);

    void attach(DbRegistry& registry) noexcept {
        registry_ = &registry;
        arm();
    }

    void detach() noexcept {
        registry_ = nullptr;
        timer_.cancel();
    }

    void arm() noexcept {
        timer_.expires_after(kTick);
        timer_.async_wait([this](const std::error_code& error) noexcept {
            if (error || registry_ == nullptr) {
                return;
            }
            registry_->scanDeadlines();
            arm();
        });
    }

    asio::steady_timer timer_;
    DbRegistry* registry_{nullptr};
};

}  // namespace detail

namespace {

[[nodiscard]] detail::DbConfigStorage cloneMigrationConfig(const DbConfig& source, std::pmr::memory_resource* resource) {
    return detail::DbConfigStorage(source, resource);
}

struct DbMigrationOptionsStorage final {
    DbMigrationOptionsStorage(const DbMigrationOptions& source, std::pmr::memory_resource* resource)
        : table(source.table, resource),
          lockTimeout(source.lockTimeout) {}

    std::pmr::string table;
    std::chrono::seconds lockTimeout;
};

void appendQuotedIdentifier(std::pmr::string& sql, std::string_view identifier, DbDriver driver) {
    if (!detail::isValidMigrationTableName(identifier, driver)) {
        throw std::invalid_argument("database migration table has an invalid backend identifier");
    }
    const auto quote = driver == DbDriver::kPostgreSql ? '"' : '`';
    sql.push_back(quote);
    sql.append(identifier);
    sql.push_back(quote);
}

[[nodiscard]] std::pmr::string buildMigrationLockName(const detail::DbConfigStorage& config, std::pmr::memory_resource* resource) {
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

[[nodiscard]] std::pmr::string buildCreateMigrationsTableSql(std::string_view table, DbDriver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 260);
    sql.append("CREATE TABLE IF NOT EXISTS ");
    appendQuotedIdentifier(sql, table, driver);
    if (driver == DbDriver::kPostgreSql) {
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
[[nodiscard]] std::pmr::string buildChecksumColumnProbeSql(DbDriver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql("SELECT 1 FROM information_schema.columns WHERE table_schema = ", resource);
    sql.append(driver == DbDriver::kPostgreSql ? "current_schema() AND table_name = $1" : "DATABASE() AND table_name = ?");
    sql.append(" AND column_name = 'checksum'");
    return sql;
}

[[nodiscard]] std::pmr::string buildAddChecksumColumnSql(std::string_view table, DbDriver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 48);
    sql.append("ALTER TABLE ");
    appendQuotedIdentifier(sql, table, driver);
    sql.append(" ADD COLUMN checksum CHAR(64)");
    return sql;
}

[[nodiscard]] std::pmr::string buildFindMigrationSql(std::string_view table, DbDriver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 50);
    sql.append("SELECT checksum FROM ");
    appendQuotedIdentifier(sql, table, driver);
    sql.append(driver == DbDriver::kPostgreSql ? " WHERE migration_id = $1 LIMIT 1" : " WHERE migration_id = ? LIMIT 1");
    return sql;
}

[[nodiscard]] std::pmr::string buildInsertMigrationSql(std::string_view table, DbDriver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 40);
    sql.append("INSERT INTO ");
    appendQuotedIdentifier(sql, table, driver);
    sql.append(driver == DbDriver::kPostgreSql ? " (migration_id, checksum) VALUES ($1, $2)" : " (migration_id, checksum) VALUES (?, ?)");
    return sql;
}

// A row written before checksums were recorded carries none. Adopting the
// current text as that row's baseline is the only choice available -- whatever
// ran back then is unknowable -- and it means the next edit is caught.
[[nodiscard]] std::pmr::string buildAdoptChecksumSql(std::string_view table, DbDriver driver, std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.reserve(table.size() + 72);
    sql.append("UPDATE ");
    appendQuotedIdentifier(sql, table, driver);
    sql.append(driver == DbDriver::kPostgreSql ? " SET checksum = $1 WHERE migration_id = $2" : " SET checksum = ? WHERE migration_id = ?");
    return sql;
}

void appendMigrationId(std::pmr::vector<std::pmr::string>& ids, std::string_view id) {
    ids.emplace_back();
    ids.back().assign(id.data(), id.size());
}

[[nodiscard]] std::runtime_error migrationDrift(std::string_view id, std::pmr::memory_resource* resource) {
    std::pmr::string message("database migration '", resource);
    message.append(id);
    message.append("' was edited after it was applied; its recorded checksum no longer matches");
    return std::runtime_error(message.c_str());
}

}  // namespace

std::pmr::string detail::migrationChecksum(std::string_view sql, std::pmr::memory_resource* resource) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    if (EVP_Digest(sql.data(), sql.size(), digest.data(), &digestSize, EVP_sha256(), nullptr) != 1 || digestSize * 2 != kMigrationChecksumSize) {
        throw std::runtime_error("database migration checksum could not be computed");
    }

    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::pmr::string hex(detail::pmrResourceOrDefault(resource));
    hex.reserve(kMigrationChecksumSize);
    for (unsigned int i = 0; i < digestSize; ++i) {
        hex.push_back(kHexDigits[digest[i] >> 4]);
        hex.push_back(kHexDigits[digest[i] & 0x0F]);
    }
    return hex;
}

class detail::DbMigrationRunner final {
public:
    [[nodiscard]] static Task<DbMigrationReport> run(asio::io_context& ioContext, detail::DbMigrationDeadlineScanner& scanner, detail::DbConfigStorage config, std::span<const DbMigration> migrations, DbMigrationOptionsStorage options, std::pmr::memory_resource* resource) {
        auto* resolved = detail::pmrResourceOrDefault(resource);
        detail::validateMigrationList(migrations, config.driver);
        if (!detail::isValidMigrationTableName(options.table, config.driver)) {
            throw std::invalid_argument("database migration table has an invalid backend identifier");
        }
        if (options.lockTimeout.count() <= 0) {
            throw std::invalid_argument("database migration lock timeout must be greater than zero");
        }
        const auto driver = config.driver;
        if (driver == DbDriver::kPostgreSql) {
            // Validate before opening a connection.  PostgreSQL receives this
            // value in milliseconds, and a valid seconds duration can still
            // overflow that representation during conversion.
            (void)detail::postgresLockTimeoutMilliseconds(options.lockTimeout);
        }

        if (!config.acquireTimeout.has_value()) {
            config.acquireTimeout = config.queryTimeout;
        }
        validateDbConfig(config);
        DbMigrationReport report(resolved);

        auto lockName = buildMigrationLockName(config, resolved);
        const detail::DbDefinition databases[] = {detail::DbDefinition{std::pmr::string(detail::kDefaultDbAlias.data(), detail::kDefaultDbAlias.size(), resolved), std::move(config)}};
        detail::DbRegistry registry(ioContext, resolved, databases);
        const detail::DbMigrationDeadlineScanner::Attachment scanning(scanner, registry);
        co_await registry.connect();
        detail::ScopedOperationScope operationScope;
        auto handle = registry.get(resolved, operationScope);

        co_await acquireLock(handle, driver, lockName, options.lockTimeout, resolved);

        std::exception_ptr failure;
        try {
            co_await applyMigrations(handle, driver, migrations, options, report, resolved);
        } catch (...) {
            failure = std::current_exception();
        }

        // Release explicitly only after a clean run. A failed statement has
        // already closed the connection, and both lock flavours are held by the
        // session, so the lock is gone with it; issuing the release now would
        // silently reconnect and run it on a session that never held the lock
        // -- a wasted round trip that PostgreSQL answers with a "you don't own
        // a lock" warning. closeNow() below covers the unreleased case.
        if (failure == nullptr) {
            try {
                co_await releaseLock(handle, driver, lockName);
            } catch (...) {
                failure = std::current_exception();
            }
        }

        registry.closeNow();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        co_return std::move(report);
    }

private:
    // Serializes concurrent deployers. Both locks are held by the session, so
    // losing the connection releases them -- including the migration's own
    // failure path, which closes it.
    [[nodiscard]] static Task<void> acquireLock(DbHandle& handle, DbDriver driver, std::string_view lockName, std::chrono::seconds lockTimeout, std::pmr::memory_resource* resource) {
        if (driver == DbDriver::kMariaDb) {
            const auto lockSeconds = static_cast<std::int64_t>(lockTimeout.count());
            std::array<DbValue, 2> lockParams{DbValue{lockName}, DbValue{lockSeconds}};
            auto lockResult = co_await handle.query("SELECT GET_LOCK(?, ?)", std::span<const DbValue>(lockParams));
            // GET_LOCK answers 0 on timeout and NULL on error rather than
            // failing the statement, so the wait has to be read out of the row.
            if (lockResult.size() != 1 || lockResult[0].empty() || lockResult[0][0].as<bool>() != true) {
                throw std::runtime_error("database migration lock could not be acquired");
            }
            co_return;
        }

        // PostgreSQL's advisory lock waits without a bound of its own; the
        // session's lock_timeout is what ends that wait.
        std::pmr::string timeoutSql("SET lock_timeout TO '", resource);
        detail::appendDbNumber(timeoutSql, detail::postgresLockTimeoutMilliseconds(lockTimeout));
        timeoutSql.append("ms'");
        (void)co_await handle.execute(timeoutSql);
        std::array<DbValue, 1> lockParams{DbValue{lockName}};
        (void)co_await handle.query("SELECT pg_advisory_lock(hashtextextended($1, 0))", std::span<const DbValue>(lockParams));
    }

    [[nodiscard]] static Task<void> releaseLock(DbHandle& handle, DbDriver driver, std::string_view lockName) {
        std::array<DbValue, 1> releaseParams{DbValue{lockName}};
        if (driver == DbDriver::kMariaDb) {
            (void)co_await handle.execute("DO RELEASE_LOCK(?)", std::span<const DbValue>(releaseParams));
        } else {
            (void)co_await handle.query("SELECT pg_advisory_unlock(hashtextextended($1, 0))", std::span<const DbValue>(releaseParams));
        }
    }

    [[nodiscard]] static Task<void> applyMigrations(DbHandle& handle, DbDriver driver, std::span<const DbMigration> migrations, const DbMigrationOptionsStorage& options, DbMigrationReport& report, std::pmr::memory_resource* resource) {
        (void)co_await handle.execute(buildCreateMigrationsTableSql(options.table, driver, resource));

        std::array<DbValue, 1> tableParams{DbValue{std::string_view(options.table)}};
        auto checksumColumn = co_await handle.query(buildChecksumColumnProbeSql(driver, resource), std::span<const DbValue>(tableParams));
        if (checksumColumn.empty()) {
            (void)co_await handle.execute(buildAddChecksumColumnSql(options.table, driver, resource));
        }

        auto findSql = buildFindMigrationSql(options.table, driver, resource);
        auto insertSql = buildInsertMigrationSql(options.table, driver, resource);
        auto adoptSql = buildAdoptChecksumSql(options.table, driver, resource);
        for (const auto& migration : migrations) {
            const auto checksum = detail::migrationChecksum(migration.sql(), resource);
            std::array<DbValue, 1> findParams{DbValue{migration.id()}};
            auto existing = co_await handle.query(findSql, std::span<const DbValue>(findParams));
            if (!existing.empty()) {
                const auto& recorded = existing[0][0];
                const auto value = recorded.value();
                if (!value || value->empty()) {
                    std::array<DbValue, 2> adoptParams{DbValue{std::string_view(checksum)}, DbValue{migration.id()}};
                    (void)co_await handle.execute(adoptSql, std::span<const DbValue>(adoptParams));
                } else if (*value != std::string_view(checksum)) {
                    throw migrationDrift(migration.id(), resource);
                }
                appendMigrationId(report.skipped_, migration.id());
                continue;
            }

            std::array<DbValue, 2> insertParams{DbValue{migration.id()}, DbValue{std::string_view(checksum)}};
            // On PostgreSQL the statement and the row recording it commit
            // together, so an interruption between them cannot leave the schema
            // changed and unrecorded. MariaDB commits DDL implicitly, so there
            // is no transaction to put them in and the two-statement window
            // stands.
            if (driver == DbDriver::kPostgreSql && migration.atomicity() == DbMigrationAtomicity::kTransactional) {
                auto transaction = co_await handle.beginTransaction();
                (void)co_await transaction.execute(migration.sql());
                (void)co_await transaction.execute(insertSql, std::span<const DbValue>(insertParams));
                co_await transaction.commit();
            } else {
                (void)co_await handle.execute(migration.sql());
                (void)co_await handle.execute(insertSql, std::span<const DbValue>(insertParams));
            }
            appendMigrationId(report.applied_, migration.id());
        }
    }
};

DbMigrator::DbMigrator(const DbConfig& config, const DbMigrationOptions& options, std::pmr::memory_resource* resource)
    : config_(config),
      options_(options),
      resource_(detail::pmrResourceOrDefault(resource)) {}

DbMigrationReport DbMigrator::migrate(std::span<const DbMigration> migrations) const {
    return migrate(config_, migrations, options_, resource_);
}

DbMigrationReport DbMigrator::migrate(const DbConfig& config, std::span<const DbMigration> migrations, const DbMigrationOptions& options, std::pmr::memory_resource* resource) {
    auto* resolved = detail::pmrResourceOrDefault(resource);
    auto ownedConfig = cloneMigrationConfig(config, resolved);
    auto ownedOptions = DbMigrationOptionsStorage(options, resolved);
    asio::io_context ioContext(1);
    // Outlives the coroutine that attaches to it and is destroyed before the
    // io_context, so no tick can observe either after it is gone.
    detail::DbMigrationDeadlineScanner scanner(ioContext);
    std::optional<DbMigrationReport> report;
    std::exception_ptr exception;
    detail::asyncStartTask(detail::DbMigrationRunner::run(ioContext, scanner, std::move(ownedConfig), migrations, std::move(ownedOptions), resolved), asio::bind_executor(ioContext.get_executor(), [&report, &exception](detail::TaskCompletionResult<DbMigrationReport> completion) {
        if (const auto* failure = completion.failure()) {
            exception = failure->exception();
        } else {
            report.emplace(std::move(*completion.success()).takeValue());
        }
    }));
    ioContext.run();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (!report.has_value()) {
        throw std::logic_error("database migration produced no report or exception");
    }
    return std::move(*report);
}

}  // namespace ruvia
