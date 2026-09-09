#include <array>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>

#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

class TestWorker final {
public:
    explicit TestWorker(asio::io_context& ioContext)
        : dispatcher_(std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8)),
          handle_(ruvia::detail::WorkerHandleAccess::make(dispatcher_)) {}

    [[nodiscard]] const ruvia::WorkerHandle& handle() const noexcept {
        return handle_;
    }

private:
    std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher_;
    ruvia::WorkerHandle handle_;
};

template <typename MakeOperation>
void verifiesOwnerResourceAndColdDiscard(ruvia::test::CountingMemoryResource& owner,
    ruvia::testing::TestContext& ruvia_ctx, MakeOperation&& makeOperation) {
    const auto baseline = owner.liveAllocations();
    {
        ruvia::detail::ScopedOperationScope scope;
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
        ruvia::detail::ScopedOperationScope scope;
        auto operation = makeOperation(scope);
        RUVIA_CHECK(owner.liveAllocations() > baseline);
    }
    RUVIA_CHECK_EQ(owner.liveAllocations(), baseline);
}

template <typename MakeOperation>
void verifiesClosedScopeRejectsColdOperation(ruvia::testing::TestContext& ruvia_ctx,
    MakeOperation&& makeOperation) {
    ruvia::detail::ScopedOperationScope scope;
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
    asio::io_context ioContext;
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
    asio::io_context ioContext;
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

RUVIA_TEST(client_operation_arguments_use_http_registry_owner_resource) {
    asio::io_context ioContext;
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
