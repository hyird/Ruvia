#include <array>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/db/db_types.h"

#include "db/db_registry.h"
#include "db/db_transaction_start.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using ruvia::db_driver;
using ruvia::db_transaction_access_mode;
using ruvia::db_transaction_isolation;
using ruvia::db_transaction_options;
using ruvia::detail::db_transaction_start_plan;
using ruvia::detail::make_db_transaction_start_plan;

struct isolation_case final {
    db_transaction_isolation value_;
    std::string_view postgres_suffix_;
    std::string_view maria_name_;
};

constexpr std::array isolation_cases{
    isolation_case{db_transaction_isolation::default_value, "", ""},
    isolation_case{db_transaction_isolation::read_uncommitted, " ISOLATION LEVEL READ UNCOMMITTED",
        "READ UNCOMMITTED"},
    isolation_case{db_transaction_isolation::read_committed, " ISOLATION LEVEL READ COMMITTED",
        "READ COMMITTED"},
    isolation_case{db_transaction_isolation::repeatable_read, " ISOLATION LEVEL REPEATABLE READ",
        "REPEATABLE READ"},
    isolation_case{db_transaction_isolation::serializable, " ISOLATION LEVEL SERIALIZABLE",
        "SERIALIZABLE"},
};

struct access_case final {
    db_transaction_access_mode value_;
    std::string_view suffix_;
};

constexpr std::array access_cases{
    access_case{db_transaction_access_mode::default_value, ""},
    access_case{db_transaction_access_mode::read_write, " READ WRITE"},
    access_case{db_transaction_access_mode::read_only, " READ ONLY"},
};

[[nodiscard]] std::string expected_postgres_begin(
    std::string_view isolation_suffix, std::string_view access_suffix) {
    std::string result_value{"BEGIN"};
    result_value.append(isolation_suffix);
    result_value.append(access_suffix);
    return result_value;
}

[[nodiscard]] std::string expected_maria_begin(std::string_view access_suffix) {
    std::string result_value{"START TRANSACTION"};
    result_value.append(access_suffix);
    return result_value;
}

}  // namespace

RUVIA_TEST(db_transaction_start_plan_covers_postgresql_options) {
    for (const auto isolation : isolation_cases) {
        for (const auto access : access_cases) {
            const auto plan = make_db_transaction_start_plan(
                db_driver::postgresql, db_transaction_options{isolation.value_, access.value_});
            RUVIA_CHECK_EQ(plan.configure_, std::string_view{});
            RUVIA_CHECK_EQ(plan.begin_, expected_postgres_begin(isolation.postgres_suffix_, access.suffix_));
        }
    }
}

RUVIA_TEST(db_transaction_start_plan_covers_mariadb_options) {
    for (const auto isolation : isolation_cases) {
        for (const auto access : access_cases) {
            const auto plan = make_db_transaction_start_plan(
                db_driver::mariadb, db_transaction_options{isolation.value_, access.value_});
            const auto expected_configure = isolation.maria_name_.empty()
                                                ? std::string{}
                                                : "SET TRANSACTION ISOLATION LEVEL " + std::string(isolation.maria_name_);
            RUVIA_CHECK_EQ(plan.configure_, expected_configure);
            RUVIA_CHECK_EQ(plan.begin_, expected_maria_begin(access.suffix_));
        }
    }
}

RUVIA_TEST(db_transaction_start_plan_snapshots_options_before_deferred_start) {
    db_transaction_options options{
        .isolation_ = db_transaction_isolation::serializable,
        .access_mode_ = db_transaction_access_mode::read_only,
    };
    const db_transaction_start_plan plan = make_db_transaction_start_plan(db_driver::postgresql, options);

    options.isolation_ = db_transaction_isolation::default_value;
    options.access_mode_ = db_transaction_access_mode::default_value;

    RUVIA_CHECK_EQ(plan.configure_, std::string_view{});
    RUVIA_CHECK_EQ(plan.begin_, std::string_view{"BEGIN ISOLATION LEVEL SERIALIZABLE READ ONLY"});
}

RUVIA_TEST(db_transaction_start_plan_rejects_unsupported_driver) {
    bool rejected = false;
    try {
        (void)make_db_transaction_start_plan(
            db_driver::unspecified, db_transaction_options{});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(ruvia::testing::throws_on([] {
        (void)make_db_transaction_start_plan(db_driver::postgresql,
            {.isolation_ = static_cast<db_transaction_isolation>(255)});
    }));
    RUVIA_CHECK(ruvia::testing::throws_on([] {
        (void)make_db_transaction_start_plan(db_driver::mariadb,
            {.access_mode_ = static_cast<db_transaction_access_mode>(255)});
    }));
}

RUVIA_TEST(db_transaction_options_cold_cancelled_and_invalid_operations_release_storage) {
    using namespace ruvia;
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        db_driver::mariadb,
#endif
    };
    for (const auto driver : drivers) {
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().handle();
        test::counting_memory_resource resource;
        detail::db_registry registry(context_value, worker_value, &resource,
            db_config{.driver_ = driver});
        ::ruvia::operation_scope scope;
        auto handle = registry.get(scope);
        stop_source cancellation;
        cancellation.request_stop();
        auto cancelled = handle.with_options({.stop_token_ = cancellation.token()});
        const auto baseline = resource.live_allocations();
        auto exercise = [&]() -> task<void> {
            for (int iteration = 0; iteration < 8; ++iteration) {
                {
                    const auto cold = handle.begin_transaction({.isolation_ = db_transaction_isolation::serializable,
                        .access_mode_ = db_transaction_access_mode::read_only});
                    RUVIA_CHECK(scope.has_pending_operations());
                }
                RUVIA_CHECK(!scope.has_pending_operations());
                RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
                bool observed_cancellation = false;
                bool observed_invalid = false;
                db_transaction_options options{.isolation_ = db_transaction_isolation::repeatable_read,
                    .access_mode_ = db_transaction_access_mode::read_only};
                auto operation = cancelled.begin_transaction(options);
                options.isolation_ = static_cast<db_transaction_isolation>(255);
                try {
                    auto transaction = co_await std::move(operation);
                    RUVIA_CHECK(false);
                } catch (const db_error& error) {
                    observed_cancellation = error.code() == db_error::code_type::cancelled;
                }
                auto invalid = handle.begin_transaction(options);
                options = {};
                try {
                    auto transaction = co_await std::move(invalid);
                    RUVIA_CHECK(false);
                } catch (const std::invalid_argument&) {
                    observed_invalid = true;
                }
                RUVIA_CHECK(observed_cancellation);
                RUVIA_CHECK(observed_invalid);
                RUVIA_CHECK(!scope.has_pending_operations());
                RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
            }
        };
        std::promise<void> completion;
        auto future = completion.get_future();
        asio::co_spawn(context_value, as_awaitable(exercise()),
            [&attachment, &completion](std::exception_ptr error) {
                if (error) {
                    completion.set_exception(error);
                } else {
                    completion.set_value();
                }
                attachment.stop();
            });
        attachment.run();
        future.get();
    }
}
