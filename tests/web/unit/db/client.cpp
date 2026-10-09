#include <array>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_expressions.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::test::rejecting_memory_resource;
using ruvia::test::tracking_resource;
using ruvia::testing::throws_on;

[[nodiscard]] ruvia::db_config test_db_config() {
#ifdef RUVIA_ENABLE_MARIADB
    return ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#else
    return ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
#endif
}

}  // namespace

RUVIA_TEST(scoped_operation_scope_tracks_cold_owner_operations) {
    ruvia::operation_scope operation_scope;
    auto cold_task = []() -> ruvia::task<void> { co_return; }();
    {
        auto operation = ruvia::make_scoped_operation(operation_scope, std::move(cold_task));
        RUVIA_CHECK(operation_scope.has_pending_operations());
    }
    RUVIA_CHECK(!operation_scope.has_pending_operations());
}

RUVIA_TEST(db_error_carries_category_and_native_diagnostics) {
    const ruvia::db_error timeout(ruvia::db_error::code_type::timeout,
        ruvia::db_driver::mariadb, "timeout", 1205, "HY000");
    const ruvia::db_error unique_violation(ruvia::db_error::code_type::statement_failed,
        ruvia::db_driver::postgresql, "duplicate key", std::nullopt, "23505", "uq_jobs_key");
    const ruvia::db_error cancelled(ruvia::db_error::code_type::cancelled,
        ruvia::db_driver::postgresql, "cancelled");
    const ruvia::db_error closing(ruvia::db_error::code_type::closing,
        ruvia::db_driver::mariadb, "closing");
    RUVIA_CHECK(timeout.code() == ruvia::db_error::code_type::timeout);
    RUVIA_CHECK(timeout.driver() == ruvia::db_driver::mariadb);
    RUVIA_CHECK(timeout.native_code() == 1205);
    RUVIA_CHECK(timeout.sql_state() == "HY000");
    RUVIA_CHECK(unique_violation.driver() == ruvia::db_driver::postgresql);
    RUVIA_CHECK(unique_violation.sql_state() == "23505");
    RUVIA_CHECK(unique_violation.constraint_name() == "uq_jobs_key");
    RUVIA_CHECK(!unique_violation.native_code());
    RUVIA_CHECK(cancelled.code() == ruvia::db_error::code_type::cancelled);
    RUVIA_CHECK(!cancelled.native_code());
    RUVIA_CHECK(!cancelled.sql_state());
    RUVIA_CHECK(!cancelled.constraint_name());
    RUVIA_CHECK(closing.code() == ruvia::db_error::code_type::closing);
}

RUVIA_TEST(db_migrator_validates_before_opening_connection) {
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
    RUVIA_CHECK(rejected);
}

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_TEST(db_migrator_rejects_unrepresentable_postgresql_lock_timeout_before_connecting) {
    auto config = ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
    ruvia::db_migrator_options options;
    options.lock_timeout_ = std::chrono::seconds::max();

    bool rejected = false;
    try {
        (void)ruvia::db_migrator::migrate(config, std::span<const ruvia::db_migration>(), options);
    } catch (const std::invalid_argument& error) {
        rejected =
            std::string_view(error.what()) ==
            "database migration lock timeout cannot be represented as PostgreSQL milliseconds";
    }
    RUVIA_CHECK(rejected);
}
#endif

RUVIA_TEST(db_migrator_copies_public_configuration) {
    tracking_resource target_resource;
    std::optional<ruvia::db_migrator> migrator;
    {
        auto config = test_db_config();
        config.host_ = std::string(80, 'h');
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.username_ = std::string(80, 'u');
        config.password_ = std::string(80, 'p');
        config.database_ = std::string(80, 'd');
        ruvia::db_migrator_options options{
            .table_ = std::string(60, 't'),
            .lock_timeout_ = std::chrono::seconds(30),
            .resource_ = &target_resource,
        };
        migrator.emplace(config, options);
    }
    // Opaque owner + four long DB strings + the long migration table all use
    // the caller's resource rather than their public std::string allocators.
    RUVIA_CHECK(target_resource.allocation_count() >= 6);
    migrator.reset();
}

RUVIA_TEST(db_migrator_validates_complete_configuration_before_allocating) {
    {
        auto config = test_db_config();
        config.host_ = std::string(80, 'h');
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.connect_timeout_ = std::chrono::milliseconds::zero();
        tracking_resource resource;
        const ruvia::db_migrator_options options{
            .table_ = std::string(60, 't'),
            .resource_ = &resource,
        };

        RUVIA_CHECK(throws_on([&] { (void)ruvia::db_migrator(config, options); }));
        RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
    }
    {
        auto config = test_db_config();
        config.host_ = std::string(80, 'h');
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        tracking_resource resource;
        const ruvia::db_migrator_options options{
            .table_ = "invalid-table-name",
            .resource_ = &resource,
        };

        RUVIA_CHECK(throws_on([&] { (void)ruvia::db_migrator(config, options); }));
        RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
    }
}

RUVIA_TEST(db_migrator_validates_migration_list_before_allocating_runtime) {
    const std::array<ruvia::db_migration, 2> migrations{{
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 1"}},
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 2"}},
    }};
    auto config = test_db_config();
    config.host_ = std::string(80, 'h');
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    tracking_resource resource;
    const ruvia::db_migrator_options options{
        .table_ = std::string(60, 't'),
        .resource_ = &resource,
    };

    RUVIA_CHECK(throws_on([&] {
        (void)ruvia::db_migrator::migrate(
            config, std::span<const ruvia::db_migration>(migrations), options);
    }));
    RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
}

namespace {
struct default_resource_guard final {
    explicit default_resource_guard(std::pmr::memory_resource* resource) noexcept
        : previous_(std::pmr::set_default_resource(resource)) {}
    ~default_resource_guard() {
        std::pmr::set_default_resource(previous_);
    }
    std::pmr::memory_resource* previous_;
};
}  // namespace

RUVIA_TEST(db_expression_values_use_explicit_resource_for_owned_parameters) {
    ruvia::test::counting_memory_resource source_resource;
    const std::string value(128, 'v');
    ruvia::db_query source_value(&source_resource);
    source_value.select(source_value.value(value));
    const auto statement = source_value.compile(ruvia::db_driver::postgresql, &source_resource);
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{1});
    if (statement.params().size() != 1) {
        return;
    }

    for (const bool through_expressions : {false, true}) {
        ruvia::test::counting_memory_resource destination_resource;
        ruvia::db_query destination(&destination_resource);
        ruvia::db_expressions expressions(&destination_resource);
        rejecting_memory_resource rejecting_default;
        rejecting_default.reject_allocations();
        bool unexpected_allocation = false;
        try {
            default_resource_guard guard_value(&rejecting_default);
            destination.select(through_expressions
                                   ? destination.import_expression(expressions.value(statement.params()[0]))
                                   : destination.value(statement.params()[0]));
        } catch (const std::bad_alloc&) {
            unexpected_allocation = true;
        }
        RUVIA_CHECK(!unexpected_allocation);
        if (!unexpected_allocation) {
            const auto compiled = destination.compile(ruvia::db_driver::postgresql, &destination_resource);
            RUVIA_CHECK_EQ(compiled.params().size(), std::size_t{1});
        }
    }
}
