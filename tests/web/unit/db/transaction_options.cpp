#include <array>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbTransactionStart.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::DbDriver;
using ruvia::DbTransactionAccessMode;
using ruvia::DbTransactionIsolation;
using ruvia::DbTransactionOptions;
using ruvia::detail::DbTransactionStartPlan;
using ruvia::detail::makeDbTransactionStartPlan;

struct IsolationCase final {
    DbTransactionIsolation value;
    std::string_view postgresSuffix;
    std::string_view mariaName;
};

constexpr std::array isolationCases{
    IsolationCase{DbTransactionIsolation::kDefault, "", ""},
    IsolationCase{DbTransactionIsolation::kReadUncommitted, " ISOLATION LEVEL READ UNCOMMITTED",
        "READ UNCOMMITTED"},
    IsolationCase{DbTransactionIsolation::kReadCommitted, " ISOLATION LEVEL READ COMMITTED",
        "READ COMMITTED"},
    IsolationCase{DbTransactionIsolation::kRepeatableRead, " ISOLATION LEVEL REPEATABLE READ",
        "REPEATABLE READ"},
    IsolationCase{DbTransactionIsolation::kSerializable, " ISOLATION LEVEL SERIALIZABLE",
        "SERIALIZABLE"},
};

struct AccessCase final {
    DbTransactionAccessMode value;
    std::string_view suffix;
};

constexpr std::array accessCases{
    AccessCase{DbTransactionAccessMode::kDefault, ""},
    AccessCase{DbTransactionAccessMode::kReadWrite, " READ WRITE"},
    AccessCase{DbTransactionAccessMode::kReadOnly, " READ ONLY"},
};

[[nodiscard]] std::string expectedPostgresBegin(
    std::string_view isolationSuffix, std::string_view accessSuffix) {
    std::string result{"BEGIN"};
    result.append(isolationSuffix);
    result.append(accessSuffix);
    return result;
}

[[nodiscard]] std::string expectedMariaBegin(std::string_view accessSuffix) {
    std::string result{"START TRANSACTION"};
    result.append(accessSuffix);
    return result;
}

}  // namespace

RUVIA_TEST(db_transaction_start_plan_covers_postgresql_options) {
    for (const auto isolation : isolationCases) {
        for (const auto access : accessCases) {
            const auto plan = makeDbTransactionStartPlan(
                DbDriver::kPostgreSql, DbTransactionOptions{isolation.value, access.value});
            RUVIA_CHECK_EQ(plan.configure, std::string_view{});
            RUVIA_CHECK_EQ(plan.begin, expectedPostgresBegin(isolation.postgresSuffix, access.suffix));
        }
    }
}

RUVIA_TEST(db_transaction_start_plan_covers_mariadb_options) {
    for (const auto isolation : isolationCases) {
        for (const auto access : accessCases) {
            const auto plan = makeDbTransactionStartPlan(
                DbDriver::kMariaDb, DbTransactionOptions{isolation.value, access.value});
            const auto expectedConfigure = isolation.mariaName.empty()
                                               ? std::string{}
                                               : "SET TRANSACTION ISOLATION LEVEL " + std::string(isolation.mariaName);
            RUVIA_CHECK_EQ(plan.configure, expectedConfigure);
            RUVIA_CHECK_EQ(plan.begin, expectedMariaBegin(access.suffix));
        }
    }
}

RUVIA_TEST(db_transaction_start_plan_snapshots_options_before_deferred_start) {
    DbTransactionOptions options{
        .isolation = DbTransactionIsolation::kSerializable,
        .accessMode = DbTransactionAccessMode::kReadOnly,
    };
    const DbTransactionStartPlan plan = makeDbTransactionStartPlan(DbDriver::kPostgreSql, options);

    options.isolation = DbTransactionIsolation::kDefault;
    options.accessMode = DbTransactionAccessMode::kDefault;

    RUVIA_CHECK_EQ(plan.configure, std::string_view{});
    RUVIA_CHECK_EQ(plan.begin, std::string_view{"BEGIN ISOLATION LEVEL SERIALIZABLE READ ONLY"});
}

RUVIA_TEST(db_transaction_start_plan_rejects_unsupported_driver) {
    bool rejected = false;
    try {
        (void)makeDbTransactionStartPlan(
            DbDriver::kUnspecified, DbTransactionOptions{});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(ruvia::testing::throwsOn([] {
        (void)makeDbTransactionStartPlan(DbDriver::kPostgreSql,
            {.isolation = static_cast<DbTransactionIsolation>(255)});
    }));
    RUVIA_CHECK(ruvia::testing::throwsOn([] {
        (void)makeDbTransactionStartPlan(DbDriver::kMariaDb,
            {.accessMode = static_cast<DbTransactionAccessMode>(255)});
    }));
}

RUVIA_TEST(db_transaction_options_cold_cancelled_and_invalid_operations_release_storage) {
    using namespace ruvia;
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        DbDriver::kPostgreSql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        DbDriver::kMariaDb,
#endif
    };
    for (const auto driver : drivers) {
        asio::io_context context;
        auto dispatcher = std::make_shared<detail::WorkerDispatcher>(context, 8);
        auto worker = detail::WorkerHandleAccess::make(dispatcher);
        test::CountingMemoryResource resource;
        detail::DbRegistry registry(context, worker, &resource, DbConfig{.driver = driver});
        detail::ScopedOperationScope scope;
        auto handle = registry.get(scope);
        StopSource cancellation;
        cancellation.requestStop();
        auto cancelled = handle.withOptions({.stopToken = cancellation.token()});
        const auto baseline = resource.liveAllocations();
        for (int iteration = 0; iteration < 8; ++iteration) {
            {
                const auto cold = handle.beginTransaction({.isolation = DbTransactionIsolation::kSerializable,
                    .accessMode = DbTransactionAccessMode::kReadOnly});
                RUVIA_CHECK(scope.hasPendingOperations());
            }
            RUVIA_CHECK(!scope.hasPendingOperations());
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
            bool observedCancellation = false;
            bool observedInvalid = false;
            auto exercise = [&]() -> Task<void> {
                DbTransactionOptions options{.isolation = DbTransactionIsolation::kRepeatableRead,
                    .accessMode = DbTransactionAccessMode::kReadOnly};
                auto operation = cancelled.beginTransaction(options);
                options.isolation = static_cast<DbTransactionIsolation>(255);
                try {
                    auto transaction = co_await std::move(operation);
                    RUVIA_CHECK(false);
                } catch (const DbError& error) {
                    observedCancellation = error.code() == DbError::Code::kCancelled;
                }
                auto invalid = handle.beginTransaction(options);
                options = {};
                try {
                    auto transaction = co_await std::move(invalid);
                    RUVIA_CHECK(false);
                } catch (const std::invalid_argument&) {
                    observedInvalid = true;
                }
            };
            {
                auto future = asio::co_spawn(context, detail::taskAsAwaitable(exercise()), asio::use_future);
                context.run();
                future.get();
            }
            RUVIA_CHECK(observedCancellation);
            RUVIA_CHECK(observedInvalid);
            RUVIA_CHECK(!scope.hasPendingOperations());
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
            context.restart();
        }
    }
}
