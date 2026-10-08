#include <array>
#include <chrono>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/OperationOptions.h"
#include "ruvia/core/StopToken.h"

#include "client/HttpClientRegistry.h"
#include "db/DbRegistry.h"
#include "memory_resource_fixture.h"
#include "redis/RedisRegistry.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

class TestWorker final {
public:
    explicit TestWorker(asio::io_context& ioContext)
        : attachment_(ruvia::attachEventLoop(ioContext, {.queue_capacity = 8})),
          handle_(attachment_.loop().handle()) {}

    [[nodiscard]] const ruvia::WorkerHandle& handle() const noexcept {
        return handle_;
    }

private:
    ruvia::EventLoopAttachment attachment_;
    ruvia::WorkerHandle handle_;
};

template <typename MakeOperation>
void verifiesOwnerResourceAndColdDiscard(ruvia::test::CountingMemoryResource& owner,
    ruvia::testing::TestContext& ruvia_ctx, MakeOperation&& makeOperation) {
    const auto baseline = owner.liveAllocations();
    {
        ruvia::operation_scope scope;
        auto operation = makeOperation(scope);
        RUVIA_CHECK(owner.liveAllocations() > baseline);
        scope.close();
        RUVIA_CHECK_EQ(owner.liveAllocations(), baseline);
    }
    RUVIA_CHECK_EQ(owner.liveAllocations(), baseline);
}

template <typename MakeOperation>
void verifiesColdDiscard(ruvia::test::CountingMemoryResource& owner,
    ruvia::testing::TestContext& ruvia_ctx, MakeOperation&& makeOperation) {
    const auto baseline = owner.liveAllocations();
    {
        ruvia::operation_scope scope;
        auto operation = makeOperation(scope);
        RUVIA_CHECK(owner.liveAllocations() > baseline);
    }
    RUVIA_CHECK_EQ(owner.liveAllocations(), baseline);
}

template <typename MakeOperation>
void verifiesClosedScopeRejectsColdOperation(ruvia::testing::TestContext& ruvia_ctx,
    MakeOperation&& makeOperation) {
    ruvia::operation_scope scope;
    scope.close();
    bool rejected = false;
    try {
        static_cast<void>(makeOperation(scope));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

#ifdef RUVIA_ENABLE_DATABASE
[[nodiscard]] ruvia::detail::DbDefinition dbDefinition(std::string_view alias,
    const ruvia::DbConfig& config) {
    return {std::pmr::string(alias),
        ruvia::detail::DbConfigStorage(config, std::pmr::get_default_resource())};
}
#endif

#ifdef RUVIA_ENABLE_REDIS
[[nodiscard]] ruvia::detail::RedisDefinition redisDefinition(std::string_view alias) {
    return {std::pmr::string(alias), ruvia::detail::RedisConfigStorage(
                                         ruvia::RedisConfig{}, std::pmr::get_default_resource())};
}
#endif

[[nodiscard]] ruvia::detail::HttpClientDefinition httpDefinition(std::string_view alias) {
    ruvia::HttpClientConfig config;
    config.host = "allocator.test";
    return {std::pmr::string(alias),
        ruvia::detail::HttpClientConfigStorage(config, std::pmr::get_default_resource())};
}

}  // namespace

#ifdef RUVIA_ENABLE_DATABASE
RUVIA_TEST(client_operation_arguments_use_db_registry_owner_resource) {
    auto& ioContext = ruvia::test::newTestIoContext();
    TestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource owner;
#ifdef RUVIA_ENABLE_MARIADB
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb};
#else
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
#endif
    const std::array definitions{
        dbDefinition("default", config), dbDefinition("analytics", config)};
    {
        ruvia::detail::DbRegistry registry(
            ioContext, worker.handle(), &owner, std::span(definitions));
        verifiesOwnerResourceAndColdDiscard(owner, ruvia_ctx, [&](auto& scope) {
            return registry.get(scope).query(std::string(4096, 'd'));
        });
        verifiesColdDiscard(owner, ruvia_ctx, [&](auto& scope) {
            return registry.get("analytics", scope).query(std::string(4096, 'a'));
        });
        verifiesClosedScopeRejectsColdOperation(ruvia_ctx, [&](auto& scope) {
            return registry.get(scope).query("SELECT 1");
        });
    }
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}
#endif

#ifdef RUVIA_ENABLE_REDIS
RUVIA_TEST(client_operation_arguments_use_redis_registry_owner_resource) {
    auto& ioContext = ruvia::test::newTestIoContext();
    TestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource owner;
    const std::array definitions{
        redisDefinition("default"), redisDefinition("cache")};
    {
        ruvia::detail::RedisRegistry registry(
            ioContext, &owner, std::span(definitions), worker.handle());
        verifiesOwnerResourceAndColdDiscard(owner, ruvia_ctx, [&](auto& scope) {
            return registry.get(scope).get(std::string(4096, 'd'));
        });
        verifiesColdDiscard(owner, ruvia_ctx, [&](auto& scope) {
            return registry.get("cache", scope).get(std::string(4096, 'a'));
        });
        verifiesClosedScopeRejectsColdOperation(ruvia_ctx, [&](auto& scope) {
            return registry.get(scope).get("key");
        });
    }
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}
#endif

RUVIA_TEST(http_registry_direct_options_are_owned_and_expired_handles_keep_priority) {
    auto& ioContext = ruvia::test::newTestIoContext();
    TestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource owner;
    const std::array definitions{httpDefinition("default")};
    {
        ruvia::detail::HttpClientRegistry registry(
            ioContext, worker.handle(), &owner, std::span(definitions));
        ruvia::operation_scope scope;
        ruvia::StopSource stop;
        auto handle = registry.get(scope, {.timeout = std::chrono::seconds(4),
                                              .stopToken = stop.token()});
        auto copy = handle;
        auto derived = copy.withOptions({.timeout = std::chrono::seconds(2)});
        auto expired = derived;
        scope.close();

        bool invalidWins = false;
        try {
            (void)expired.withOptions({.timeout = std::chrono::milliseconds::zero()});
        } catch (const std::invalid_argument&) {
            invalidWins = true;
        } catch (const std::logic_error&) {
        }
        RUVIA_CHECK(invalidWins);

        bool invalidDefaultOptionsRejected = false;
        try {
            (void)registry.get(scope, {.timeout = std::chrono::milliseconds::zero()});
        } catch (const std::invalid_argument&) {
            invalidDefaultOptionsRejected = true;
        }
        RUVIA_CHECK(invalidDefaultOptionsRejected);
        bool expiredOperationRejected = false;
        try {
            (void)expired.send({.target = "/"});
        } catch (const std::logic_error&) {
            expiredOperationRejected = true;
        }
        RUVIA_CHECK(expiredOperationRejected);

        const auto baseline = owner.liveAllocations();
        {
            ruvia::operation_scope coldScope;
            auto cold = registry.get(coldScope, {.timeout = std::chrono::seconds(3)});
            const std::string target = "/" + std::string(4096, 'c');
            auto operation = cold.send({.target = target});
            RUVIA_CHECK(owner.liveAllocations() > baseline);
        }
        RUVIA_CHECK_EQ(owner.liveAllocations(), baseline);
    }
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});

    const std::array onlyNamed{httpDefinition("api")};
    {
        ruvia::detail::HttpClientRegistry missingDefault(
            ioContext, worker.handle(), std::pmr::get_default_resource(), std::span(onlyNamed));
        ruvia::operation_scope scope;
        bool missingWins = false;
        try {
            (void)missingDefault.get(scope, {.timeout = std::chrono::milliseconds::zero()});
        } catch (const ruvia::HttpClientError& error) {
            missingWins = error.code() == ruvia::HttpClientError::Code::kNotConfigured;
        }
        RUVIA_CHECK(missingWins);
    }
    {
        const std::array configured{httpDefinition("default")};
        ruvia::detail::HttpClientRegistry closingRegistry(
            ioContext, worker.handle(), std::pmr::get_default_resource(), std::span(configured));
        closingRegistry.closeNow();
        ruvia::operation_scope scope;
        bool closingWins = false;
        try {
            (void)closingRegistry.get(scope, {.timeout = std::chrono::milliseconds::zero()});
        } catch (const ruvia::HttpClientError& error) {
            closingWins = error.code() == ruvia::HttpClientError::Code::kClosing;
        }
        RUVIA_CHECK(closingWins);
    }
}

RUVIA_TEST(client_operation_arguments_use_http_registry_owner_resource) {
    auto& ioContext = ruvia::test::newTestIoContext();
    TestWorker worker(ioContext);
    ruvia::test::CountingMemoryResource owner;
    const std::array definitions{
        httpDefinition("default"), httpDefinition("api")};
    {
        ruvia::detail::HttpClientRegistry registry(
            ioContext, worker.handle(), &owner, std::span(definitions));
        verifiesOwnerResourceAndColdDiscard(owner, ruvia_ctx, [&](auto& scope) {
            const std::string target = "/" + std::string(4096, 'd');
            const ruvia::HttpClientRequestView requestView{
                .method = "GET", .target = target};
            return registry.get(scope).send(requestView);
        });
        verifiesColdDiscard(owner, ruvia_ctx, [&](auto& scope) {
            const std::string target = "/" + std::string(4096, 'a');
            const ruvia::HttpClientRequestView requestView{
                .method = "GET", .target = target};
            return registry.get("api", scope).send(requestView);
        });
        verifiesClosedScopeRejectsColdOperation(ruvia_ctx, [&](auto& scope) {
            const ruvia::HttpClientRequestView requestView{.method = "GET", .target = "/"};
            return registry.get(scope).send(requestView);
        });
    }
    RUVIA_CHECK_EQ(owner.liveAllocations(), std::size_t{0});
}
