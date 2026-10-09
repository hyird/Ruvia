#include <array>
#include <chrono>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/stop_token.h"

#include "client/http_client_registry.h"
#include "db/db_registry.h"
#include "memory_resource_fixture.h"
#include "redis/redis_registry.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

class test_worker final {
public:
    explicit test_worker(asio::io_context& io_context)
        : attachment_(ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8})),
          handle_(attachment_.loop().handle()) {}

    [[nodiscard]] const ruvia::worker_handle& handle() const noexcept {
        return handle_;
    }

private:
    ruvia::event_loop_attachment attachment_;
    ruvia::worker_handle handle_;
};

template <typename make_operation_type>
void verifies_owner_resource_and_cold_discard(ruvia::test::counting_memory_resource& owner_value,
    ruvia::testing::test_context& ruvia_ctx, make_operation_type&& make_operation) {
    const auto baseline = owner_value.live_allocations();
    {
        ruvia::operation_scope scope;
        auto operation = make_operation(scope);
        RUVIA_CHECK(owner_value.live_allocations() > baseline);
        scope.close();
        RUVIA_CHECK_EQ(owner_value.live_allocations(), baseline);
    }
    RUVIA_CHECK_EQ(owner_value.live_allocations(), baseline);
}

template <typename make_operation_type>
void verifies_cold_discard(ruvia::test::counting_memory_resource& owner_value,
    ruvia::testing::test_context& ruvia_ctx, make_operation_type&& make_operation) {
    const auto baseline = owner_value.live_allocations();
    {
        ruvia::operation_scope scope;
        auto operation = make_operation(scope);
        RUVIA_CHECK(owner_value.live_allocations() > baseline);
    }
    RUVIA_CHECK_EQ(owner_value.live_allocations(), baseline);
}

template <typename make_operation_type>
void verifies_closed_scope_rejects_cold_operation(ruvia::testing::test_context& ruvia_ctx,
    make_operation_type&& make_operation) {
    ruvia::operation_scope scope;
    scope.close();
    bool rejected = false;
    try {
        static_cast<void>(make_operation(scope));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

#ifdef RUVIA_ENABLE_DATABASE
[[nodiscard]] ruvia::detail::db_definition db_definition(std::string_view alias,
    const ruvia::db_config& config) {
    return {std::pmr::string(alias),
        ruvia::detail::db_config_storage(config, std::pmr::get_default_resource())};
}
#endif

#ifdef RUVIA_ENABLE_REDIS
[[nodiscard]] ruvia::detail::redis_definition_type redis_definition(std::string_view alias) {
    return {std::pmr::string(alias), ruvia::detail::redis_config_storage(
                                         ruvia::redis_config{}, std::pmr::get_default_resource())};
}
#endif

[[nodiscard]] ruvia::detail::http_client_definition_type http_definition(std::string_view alias) {
    ruvia::http_client_config config;
    config.host_ = "allocator.test";
    return {std::pmr::string(alias),
        ruvia::detail::http_client_config_storage(config, std::pmr::get_default_resource())};
}

}  // namespace

#ifdef RUVIA_ENABLE_DATABASE
RUVIA_TEST(client_operation_arguments_use_db_registry_owner_resource) {
    auto& io_context = ruvia::test::new_test_io_context();
    test_worker worker(io_context);
    ruvia::test::counting_memory_resource owner;
#ifdef RUVIA_ENABLE_MARIADB
    const auto config = ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#else
    const auto config = ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
#endif
    const std::array definitions{
        db_definition("default", config), db_definition("analytics", config)};
    {
        ruvia::detail::db_registry registry(
            io_context, worker.handle(), &owner, std::span(definitions));
        verifies_owner_resource_and_cold_discard(owner, ruvia_ctx, [&](auto& scope) {
            return registry.get(scope).query(std::string(4096, 'd'));
        });
        verifies_cold_discard(owner, ruvia_ctx, [&](auto& scope) {
            return registry.get("analytics", scope).query(std::string(4096, 'a'));
        });
        verifies_closed_scope_rejects_cold_operation(ruvia_ctx, [&](auto& scope) {
            return registry.get(scope).query("SELECT 1");
        });
    }
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}
#endif

#ifdef RUVIA_ENABLE_REDIS
RUVIA_TEST(client_operation_arguments_use_redis_registry_owner_resource) {
    auto& io_context = ruvia::test::new_test_io_context();
    test_worker worker(io_context);
    ruvia::test::counting_memory_resource owner;
    const std::array definitions{
        redis_definition("default"), redis_definition("cache")};
    {
        ruvia::detail::redis_registry registry(
            io_context, &owner, std::span(definitions), worker.handle());
        verifies_owner_resource_and_cold_discard(owner, ruvia_ctx, [&](auto& scope) {
            return registry.get(scope).get(std::string(4096, 'd'));
        });
        verifies_cold_discard(owner, ruvia_ctx, [&](auto& scope) {
            return registry.get("cache", scope).get(std::string(4096, 'a'));
        });
        verifies_closed_scope_rejects_cold_operation(ruvia_ctx, [&](auto& scope) {
            return registry.get(scope).get("key");
        });
    }
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}
#endif

RUVIA_TEST(http_registry_direct_options_are_owned_and_expired_handles_keep_priority) {
    auto& io_context = ruvia::test::new_test_io_context();
    test_worker worker(io_context);
    ruvia::test::counting_memory_resource owner;
    const std::array definitions{http_definition("default")};
    {
        ruvia::detail::http_client_registry registry(
            io_context, worker.handle(), &owner, std::span(definitions));
        ruvia::operation_scope scope;
        ruvia::stop_source stop;
        auto handle = registry.get(scope, {.timeout_ = std::chrono::seconds(4),
                                              .stop_token_ = stop.token()});
        auto copy = handle;
        auto derived = copy.with_options({.timeout_ = std::chrono::seconds(2)});
        auto expired = derived;
        scope.close();

        bool invalid_wins = false;
        try {
            (void)expired.with_options({.timeout_ = std::chrono::milliseconds::zero()});
        } catch (const std::invalid_argument&) {
            invalid_wins = true;
        } catch (const std::logic_error&) {
        }
        RUVIA_CHECK(invalid_wins);

        bool invalid_default_options_rejected = false;
        try {
            (void)registry.get(scope, {.timeout_ = std::chrono::milliseconds::zero()});
        } catch (const std::invalid_argument&) {
            invalid_default_options_rejected = true;
        }
        RUVIA_CHECK(invalid_default_options_rejected);
        bool expired_operation_rejected = false;
        try {
            (void)expired.send({.target_ = "/"});
        } catch (const std::logic_error&) {
            expired_operation_rejected = true;
        }
        RUVIA_CHECK(expired_operation_rejected);

        const auto baseline = owner.live_allocations();
        {
            ruvia::operation_scope cold_scope;
            auto cold = registry.get(cold_scope, {.timeout_ = std::chrono::seconds(3)});
            const std::string target = "/" + std::string(4096, 'c');
            auto operation = cold.send({.target_ = target});
            RUVIA_CHECK(owner.live_allocations() > baseline);
        }
        RUVIA_CHECK_EQ(owner.live_allocations(), baseline);
    }
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});

    const std::array only_named{http_definition("api")};
    {
        ruvia::detail::http_client_registry missing_default(
            io_context, worker.handle(), std::pmr::get_default_resource(), std::span(only_named));
        ruvia::operation_scope scope;
        bool missing_wins = false;
        try {
            (void)missing_default.get(scope, {.timeout_ = std::chrono::milliseconds::zero()});
        } catch (const ruvia::http_client_error& error) {
            missing_wins = error.code() == ruvia::http_client_error::code_type::not_configured;
        }
        RUVIA_CHECK(missing_wins);
    }
    {
        const std::array configured{http_definition("default")};
        ruvia::detail::http_client_registry closing_registry(
            io_context, worker.handle(), std::pmr::get_default_resource(), std::span(configured));
        closing_registry.close_now();
        ruvia::operation_scope scope;
        bool closing_wins = false;
        try {
            (void)closing_registry.get(scope, {.timeout_ = std::chrono::milliseconds::zero()});
        } catch (const ruvia::http_client_error& error) {
            closing_wins = error.code() == ruvia::http_client_error::code_type::closing;
        }
        RUVIA_CHECK(closing_wins);
    }
}

RUVIA_TEST(client_operation_arguments_use_http_registry_owner_resource) {
    auto& io_context = ruvia::test::new_test_io_context();
    test_worker worker(io_context);
    ruvia::test::counting_memory_resource owner;
    const std::array definitions{
        http_definition("default"), http_definition("api")};
    {
        ruvia::detail::http_client_registry registry(
            io_context, worker.handle(), &owner, std::span(definitions));
        verifies_owner_resource_and_cold_discard(owner, ruvia_ctx, [&](auto& scope) {
            const std::string target = "/" + std::string(4096, 'd');
            const ruvia::http_client_request_view request_view{
                .method_ = "GET", .target_ = target};
            return registry.get(scope).send(request_view);
        });
        verifies_cold_discard(owner, ruvia_ctx, [&](auto& scope) {
            const std::string target = "/" + std::string(4096, 'a');
            const ruvia::http_client_request_view request_view{
                .method_ = "GET", .target_ = target};
            return registry.get("api", scope).send(request_view);
        });
        verifies_closed_scope_rejects_cold_operation(ruvia_ctx, [&](auto& scope) {
            const ruvia::http_client_request_view request_view{.method_ = "GET", .target_ = "/"};
            return registry.get(scope).send(request_view);
        });
    }
    RUVIA_CHECK_EQ(owner.live_allocations(), std::size_t{0});
}
