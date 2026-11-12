#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"

#include "memory_resource_fixture.h"
#include "test_exceptions.h"

namespace {

using ruvia::test::rejecting_memory_resource;
using ruvia::test::throws_on;
using ruvia::test::tracking_resource;

[[nodiscard]] ruvia::db_config test_db_config() {
#ifdef RUVIA_ENABLE_MARIADB
    return ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#else
    return ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
#endif
}

}  // namespace

TEST(Ruvia, scoped_operation_scope_tracks_cold_owner_operations) {
    ruvia::operation_scope operation_scope;
    auto cold_task = []() -> ruvia::task<void> { co_return; }();
    {
        auto operation = ruvia::make_scoped_operation(operation_scope, std::move(cold_task));
        EXPECT_TRUE(operation_scope.has_pending_operations());
    }
    EXPECT_TRUE(!operation_scope.has_pending_operations());
}

TEST(Ruvia, db_error_carries_category_and_native_diagnostics) {
    const ruvia::db_error timeout(ruvia::db_error::code_type::timeout,
        ruvia::db_driver::mariadb, "timeout", 1205, "HY000");
    const ruvia::db_error unique_violation(ruvia::db_error::code_type::statement_failed,
        ruvia::db_driver::postgresql, "duplicate key", std::nullopt, "23505", "uq_jobs_key");
    const ruvia::db_error cancelled(ruvia::db_error::code_type::cancelled,
        ruvia::db_driver::postgresql, "cancelled");
    const ruvia::db_error closing(ruvia::db_error::code_type::closing,
        ruvia::db_driver::mariadb, "closing");
    EXPECT_TRUE(timeout.code() == ruvia::db_error::code_type::timeout);
    EXPECT_TRUE(timeout.driver() == ruvia::db_driver::mariadb);
    EXPECT_TRUE(timeout.native_code() == 1205);
    EXPECT_TRUE(timeout.sql_state() == "HY000");
    EXPECT_TRUE(unique_violation.driver() == ruvia::db_driver::postgresql);
    EXPECT_TRUE(unique_violation.sql_state() == "23505");
    EXPECT_TRUE(unique_violation.constraint_name() == "uq_jobs_key");
    EXPECT_TRUE(!unique_violation.native_code());
    EXPECT_TRUE(cancelled.code() == ruvia::db_error::code_type::cancelled);
    EXPECT_TRUE(!cancelled.native_code());
    EXPECT_TRUE(!cancelled.sql_state());
    EXPECT_TRUE(!cancelled.constraint_name());
    EXPECT_TRUE(closing.code() == ruvia::db_error::code_type::closing);
}

TEST(Ruvia, db_config_rejects_non_positive_timeouts) {
    auto config = test_db_config();
    for (const auto field : {&ruvia::db_config::connect_timeout_, &ruvia::db_config::read_timeout_,
             &ruvia::db_config::write_timeout_, &ruvia::db_config::query_timeout_,
             &ruvia::db_config::acquire_timeout_, &ruvia::db_config::transaction_timeout_}) {
        for (const auto invalid : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
            config.*field = invalid;
            EXPECT_TRUE(throws_on([&] { ruvia::db_migrator migrator(config); }));
        }
        (config.*field).reset();
    }
}

TEST(Ruvia, db_migrator_rejects_non_positive_lock_timeouts) {
    ruvia::db_migrator_options options;
    for (const auto invalid : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
        options.lock_timeout_ = invalid;
        EXPECT_TRUE(throws_on([&] { ruvia::db_migrator migrator(test_db_config(), options); }));
    }
}

TEST(Ruvia, db_migrator_validates_before_opening_connection) {
    const std::array<ruvia::db_migration, 2> migrations{{
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 1"}},
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 2"}},
    }};
    bool rejected = false;
    try {
        (void)ruvia::db_migrator::migrate(
            test_db_config(), std::span<const ruvia::db_migration>(migrations));
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) ==
                   "database migration ids must be unique, including case";
    }
    EXPECT_TRUE(rejected);
}

#ifdef RUVIA_ENABLE_MARIADB
TEST(Ruvia, db_migrator_treats_mariadb_double_dash_at_end_of_sql_as_comment) {
    // Nothing listens on port 1, so validation is the only step that can
    // reject the list with std::invalid_argument; a connect failure is not one.
    const auto validation_error = [](std::string_view sql) {
        const std::array<ruvia::db_migration, 1> migrations{{
            ruvia::db_migration{{.id_ = "001", .sql_ = std::pmr::string(sql, ruvia::process_resource())}},
        }};
        auto config = ruvia::db_config{.driver_ = ruvia::db_driver::mariadb, .port_ = std::uint16_t{1}};
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        try {
            (void)ruvia::db_migrator::migrate(config, std::span<const ruvia::db_migration>(migrations));
        } catch (const std::invalid_argument& error) {
            return std::pmr::string(error.what(), ruvia::process_resource());
        } catch (const std::exception&) {
        }
        return std::pmr::string(ruvia::process_resource());
    };
    EXPECT_EQ(validation_error("CREATE TABLE t (a INT);--"), std::pmr::string(ruvia::process_resource()));
    EXPECT_EQ(validation_error("--"), std::pmr::string("database migration SQL must not be empty", ruvia::process_resource()));
    EXPECT_EQ(validation_error("SELECT 1;--x"), std::pmr::string("database migration must contain exactly one SQL statement", ruvia::process_resource()));
}
#endif

#ifdef RUVIA_ENABLE_POSTGRESQL
TEST(Ruvia, db_migrator_rejects_unrepresentable_postgresql_lock_timeout_before_connecting) {
    const auto config = ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
    for (const auto timeout : {
             ruvia::timeout_duration(std::chrono::duration<std::uint64_t, std::milli>{
                 std::numeric_limits<std::uint64_t>::max()}),
             ruvia::timeout_duration(std::chrono::milliseconds{
                 std::int64_t{std::numeric_limits<std::int32_t>::max()} + 1})}) {
        const ruvia::db_migrator_options options{.lock_timeout_ = timeout};
        EXPECT_THROW((void)ruvia::db_migrator::migrate(config, std::span<const ruvia::db_migration>(), options),
            std::invalid_argument);
    }
}
#endif

TEST(Ruvia, db_migrator_copies_public_configuration) {
    tracking_resource target_resource;
    std::optional<ruvia::db_migrator> migrator;
    {
        auto config = test_db_config();
        config.host_ = std::pmr::string(40, 'h', ruvia::process_resource()) + "." + std::pmr::string(40, 'h', ruvia::process_resource());
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.username_ = std::pmr::string(80, 'u', ruvia::process_resource());
        config.password_ = std::pmr::string(80, 'p', ruvia::process_resource());
        config.database_ = std::pmr::string(80, 'd', ruvia::process_resource());
        const std::pmr::string table_name(60, 't', ruvia::process_resource());
        ruvia::db_migrator_options options{
            .table_ = table_name,
            .lock_timeout_ = std::chrono::seconds(30),
            .resource_ = &target_resource,
        };
        migrator.emplace(config, options);
    }
    // Opaque owner + four long DB strings + the long migration table all use
    // the caller's resource rather than the input strings' allocators.
    EXPECT_TRUE(target_resource.allocation_count() >= 6);
    migrator.reset();
}

TEST(Ruvia, db_migrator_validates_complete_configuration_before_allocating) {
    {
        auto config = test_db_config();
        config.host_ = std::pmr::string(40, 'h', ruvia::process_resource()) + "." + std::pmr::string(40, 'h', ruvia::process_resource());
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.connect_timeout_ = std::chrono::milliseconds::zero();
        tracking_resource resource;
        const std::pmr::string table_name(60, 't', ruvia::process_resource());
        const ruvia::db_migrator_options options{
            .table_ = table_name,
            .resource_ = &resource,
        };

        EXPECT_TRUE(throws_on([&] { (void)ruvia::db_migrator(config, options); }));
        EXPECT_EQ(resource.allocation_count(), std::size_t{0});
    }
    {
        auto config = test_db_config();
        config.host_ = std::pmr::string(40, 'h', ruvia::process_resource()) + "." + std::pmr::string(40, 'h', ruvia::process_resource());
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        tracking_resource resource;
        const ruvia::db_migrator_options options{
            .table_ = "invalid-table-name",
            .resource_ = &resource,
        };

        EXPECT_TRUE(throws_on([&] { (void)ruvia::db_migrator(config, options); }));
        EXPECT_EQ(resource.allocation_count(), std::size_t{0});
    }
}

TEST(Ruvia, db_migrator_rejects_nul_bytes_in_credentials) {
    using namespace std::string_literals;
    for (const auto field : {&ruvia::db_config::username_, &ruvia::db_config::password_, &ruvia::db_config::database_}) {
        auto config = test_db_config();
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.*field = "app\0suffix"s;
        EXPECT_TRUE(throws_on([&] { (void)ruvia::db_migrator(config, {}); }));
    }
}

TEST(Ruvia, database_config_rejects_non_host_dns_inputs) {
    for (const std::string_view host : {std::string_view("bad_name"), std::string_view("1.2.3.999"),
             std::string_view("host..example"), std::string_view("-host.example")}) {
        auto config = test_db_config();
        config.host_ = host;
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        EXPECT_TRUE(throws_on([&] { (void)ruvia::db_migrator(config, {}); }));
    }
}

TEST(Ruvia, db_migrator_validates_migration_list_before_allocating_runtime) {
    const std::array<ruvia::db_migration, 2> migrations{{
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 1"}},
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 2"}},
    }};
    auto config = test_db_config();
    config.host_ = std::pmr::string(80, 'h', ruvia::process_resource());
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    tracking_resource resource;
    const std::pmr::string table_name(60, 't', ruvia::process_resource());
    const ruvia::db_migrator_options options{
        .table_ = table_name,
        .resource_ = &resource,
    };

    EXPECT_TRUE(throws_on([&] {
        (void)ruvia::db_migrator::migrate(
            config, std::span<const ruvia::db_migration>(migrations), options);
    }));
    EXPECT_EQ(resource.allocation_count(), std::size_t{0});
}

TEST(Ruvia, db_expression_values_use_explicit_resource_for_owned_parameters) {
    ruvia::test::counting_memory_resource source_resource;
    const std::pmr::string value(128, 'v', ruvia::process_resource());
    ruvia::db_query source_value(&source_resource);
    source_value.select(source_value.value(value));
    const auto statement = source_value.compile(ruvia::db_driver::postgresql, &source_resource);
    EXPECT_EQ(statement.params().size(), std::size_t{1});
    if (statement.params().size() != 1) {
        return;
    }

    for (const bool through_expressions : {false, true}) {
        ruvia::test::counting_memory_resource destination_resource;
        ruvia::db_query destination(&destination_resource);
        ruvia::db_query expressions(&destination_resource);
        rejecting_memory_resource rejecting_default;
        rejecting_default.reject_allocations();
        bool unexpected_allocation = false;
        try {
            ruvia::test::default_resource_guard guard_value(rejecting_default);
            destination.select(through_expressions
                                   ? destination.import_expression(expressions.value(statement.params()[0]))
                                   : destination.value(statement.params()[0]));
        } catch (const std::bad_alloc&) {
            unexpected_allocation = true;
        }
        EXPECT_TRUE(!unexpected_allocation);
        if (!unexpected_allocation) {
            const auto compiled = destination.compile(ruvia::db_driver::postgresql, &destination_resource);
            EXPECT_EQ(compiled.params().size(), std::size_t{1});
        }
    }
}

TEST(Ruvia, db_value_copy_inherits_owned_text_resource_and_rolls_back_failure) {
    ruvia::test::counting_memory_resource allocations;
    ruvia::test::equivalent_tracking_resource resource(allocations);
    rejecting_memory_resource unrelated;
    unrelated.reject_allocations();
    {
        std::optional<ruvia::db_value> copied;
        {
            const std::pmr::string text(4096, 'v', ruvia::process_resource());
            ruvia::db_query query(&resource);
            query.select(query.value(text));
            const auto statement = query.compile(ruvia::db_driver::postgresql, &resource);
            ASSERT_EQ(statement.params().size(), std::size_t{1});
            const auto retained = allocations.live_allocations();
            resource.reject_allocations_after(0);
            EXPECT_THROW((void)ruvia::db_value(statement.params()[0]), std::bad_alloc);
            resource.allow_allocations();
            EXPECT_EQ(allocations.live_allocations(), retained);
            {
                ruvia::test::default_resource_guard guard(unrelated);
                copied.emplace(statement.params()[0]);
            }
        }
        // The owning copy still uses the original resource after its statement dies.
        resource.reject_allocations_after(0);
        EXPECT_THROW((void)ruvia::db_value(*copied), std::bad_alloc);
        resource.allow_allocations();
        {
            ruvia::test::default_resource_guard guard(unrelated);
            const ruvia::db_value second(*copied);
        }
    }
    EXPECT_EQ(allocations.live_allocations(), std::size_t{0});
}
