#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"
#include "ruvia/web/detail/websocket/WebSocketAccess.h"

#include "context_services_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/Db.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbConfigStorage.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/detail/redis/RedisConfigStorage.h"
#include "ruvia/web/detail/redis/RedisTypesAccess.h"
#include "ruvia/web/redis/Redis.h"
#include "ruvia/web/redis/RedisTypes.h"
#endif

#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <asio/io_context.hpp>

namespace {

template <typename Fn>
void checkOperationDoesNotUseRequestArena(
    ruvia::testing::TestContext& ruvia_ctx, ruvia::RequestMemory& memory, Fn&& fn) {
    auto* const before = static_cast<std::byte*>(memory.resource()->allocate(1, 1));
    fn();
    auto* const after = static_cast<std::byte*>(memory.resource()->allocate(1, 1));
    RUVIA_CHECK(after == before + 1);
}

ruvia::Task<std::optional<ruvia::WebSocketMessage>> readWebSocket(void*) {
    co_return std::nullopt;
}

ruvia::Task<void> writeWebSocket(void*, ruvia::WebSocketOpcode, std::string_view) {
    co_return;
}

ruvia::Task<void> closeWebSocket(void*, ruvia::WebSocketCloseOptions) {
    co_return;
}

struct ContextFixture final {
    ContextFixture()
        : worker(),
          requestMemory(worker, std::span<std::byte>(requestBuffer)),
          request(ruvia::detail::HttpRequestAccess::make()),
#ifdef RUVIA_ENABLE_DATABASE
          dbDefinitions{makeDbDefinition("default"), makeDbDefinition("reporting")},
#endif
#ifdef RUVIA_ENABLE_REDIS
          redisDefinitions{makeRedisDefinition("default"), makeRedisDefinition("cache")},
#endif
          httpDefinitions{makeHttpDefinition("default"), makeHttpDefinition("upstream")},
          capabilities(ioContext, ruvia::test::testWorkerHandle(), worker.resource(), definitions(),
              {}) {
        ruvia::detail::HttpRequestAccess::setResource(request, requestMemory.resource());
    }

    [[nodiscard]] ruvia::detail::WorkerCapabilityDefinitions definitions() {
        return {
#ifdef RUVIA_ENABLE_DATABASE
            .databases = dbDefinitions,
#endif
#ifdef RUVIA_ENABLE_REDIS
            .redis = redisDefinitions,
#endif
            .httpClients = httpDefinitions,
        };
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] static ruvia::DbConfig databaseConfig() {
#ifdef RUVIA_ENABLE_MARIADB
        return ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb};
#else
        return ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
#endif
    }
#endif

    [[nodiscard]] static ruvia::HttpClientConfig httpConfig() {
        return ruvia::HttpClientConfig{
            .scheme = ruvia::HttpScheme::kHttp,
            .host = "127.0.0.1",
            .port = 1,
        };
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] ruvia::detail::DbDefinition makeDbDefinition(std::string_view alias) {
        return {std::pmr::string(alias, worker.resource()),
            ruvia::detail::DbConfigStorage(databaseConfig(), worker.resource())};
    }
#endif

#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] ruvia::detail::RedisDefinition makeRedisDefinition(std::string_view alias) {
        return {std::pmr::string(alias, worker.resource()),
            ruvia::detail::RedisConfigStorage({}, worker.resource())};
    }
#endif

    [[nodiscard]] ruvia::detail::HttpClientDefinition makeHttpDefinition(
        std::string_view alias) {
        return {std::pmr::string(alias, worker.resource()),
            ruvia::detail::HttpClientConfigStorage(httpConfig(), worker.resource())};
    }

    asio::io_context ioContext;
    ruvia::WorkerMemory worker;
    std::array<std::byte, 64 * 1024> requestBuffer{};
    ruvia::RequestMemory requestMemory;
    ruvia::HttpRequest request;
    ruvia::StopToken stopToken;
#ifdef RUVIA_ENABLE_DATABASE
    std::array<ruvia::detail::DbDefinition, 2> dbDefinitions;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::array<ruvia::detail::RedisDefinition, 2> redisDefinitions;
#endif
    std::array<ruvia::detail::HttpClientDefinition, 2> httpDefinitions;
    ruvia::detail::WorkerCapabilities capabilities;
};

}  // namespace

RUVIA_TEST(context_operation_clients_keep_parameters_out_of_request_arena) {
    ContextFixture fixture;
    auto context = ruvia::detail::ContextAccess::make(
        fixture.requestMemory, fixture.request,
        fixture.capabilities.contextServices(fixture.stopToken));

    RUVIA_CHECK(context.resource() == fixture.requestMemory.resource());
    RUVIA_CHECK(context.operationResource() == fixture.worker.resource());

    std::pmr::string handshake("websocket-handshake", fixture.requestMemory.allocator<char>());
    std::pmr::string retainedOperationValue(2048, 'r', context.operationResource());
    const std::string expectedOperationValue(2048, 'r');
#ifdef RUVIA_ENABLE_DATABASE
    auto database = context.db();
#endif
#ifdef RUVIA_ENABLE_REDIS
    auto redis = context.redis();
#endif
    auto httpClient = context.httpClient();
    auto webSocket = ruvia::detail::WebSocketAccess::make(
        nullptr, &readWebSocket, &writeWebSocket, &closeWebSocket);

    for (int index = 0; index != 2000; ++index) {
        const std::string value(2048, static_cast<char>('a' + index % 26));
        const std::string target = "/" + value;
        std::pmr::string transientOperationValue(value, context.operationResource());
        RUVIA_CHECK_EQ(handshake, std::string_view("websocket-handshake"));
        RUVIA_CHECK_EQ(retainedOperationValue, std::string_view(expectedOperationValue));
#ifdef RUVIA_ENABLE_DATABASE
        checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
            auto operation = database.query("SELECT ?", value);
            static_cast<void>(operation);
        });
        checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
            auto operation = context.db("reporting").execute("UPDATE items SET value=?", value);
            static_cast<void>(operation);
        });
        checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
            auto operation = context.db().queryStream("SELECT ?", value);
            static_cast<void>(operation);
        });
#endif
#ifdef RUVIA_ENABLE_REDIS
        checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
            auto operation = redis.command("SET", "key", value);
            static_cast<void>(operation);
        });
        checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
            auto operation = context.redis("cache").set("key", value);
            static_cast<void>(operation);
        });
#endif
        checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
            ruvia::HttpClientRequestView request{.method = "GET", .target = target};
            auto operation = httpClient.send(request);
            static_cast<void>(operation);
        });
        checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
            ruvia::HttpClientRequestView request{.method = "GET", .target = target};
            auto operation = context.httpClient("upstream").send(request);
            static_cast<void>(operation);
        });

        {
            ruvia::detail::ContextWebSocketBinding binding(context, webSocket);
#ifdef RUVIA_ENABLE_DATABASE
            checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
                auto operation = database.query("SELECT ?", std::string_view(transientOperationValue));
                static_cast<void>(operation);
            });
            checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
                auto operation = context.db().query("SELECT ?", std::string_view(transientOperationValue));
                static_cast<void>(operation);
            });
#endif
#ifdef RUVIA_ENABLE_REDIS
            checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
                auto operation = redis.command("SET", "key", transientOperationValue);
                static_cast<void>(operation);
            });
            checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
                auto operation = context.redis().command("SET", "key", transientOperationValue);
                static_cast<void>(operation);
            });
#endif
            checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
                auto operation = httpClient.send(
                    ruvia::HttpClientRequestView{.method = "GET", .target = target});
                static_cast<void>(operation);
            });
            checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
                auto operation = context.httpClient().send(
                    ruvia::HttpClientRequestView{.method = "GET", .target = target});
                static_cast<void>(operation);
            });
        }
    }

    RUVIA_CHECK_EQ(handshake, std::string_view("websocket-handshake"));
    RUVIA_CHECK_EQ(retainedOperationValue, std::string_view(expectedOperationValue));
}

#if defined(RUVIA_ENABLE_DATABASE) || defined(RUVIA_ENABLE_REDIS)
RUVIA_TEST(context_operation_results_release_transient_pmr_storage) {
#ifdef RUVIA_ENABLE_DATABASE
    ruvia::test::CountingMemoryResource dbResource;
    {
        auto& resource = dbResource;
        auto makeRows = [&](std::string_view value) {
            auto result = ruvia::detail::DbResultAccess::makeResult(&resource);
            auto& rows = ruvia::detail::DbResultAccess::rows(result);
            auto row = ruvia::detail::DbResultAccess::ownedRow(&resource);
            auto& fields = ruvia::detail::DbResultAccess::ownedFields(row);
            auto& columnNames = ruvia::detail::DbResultAccess::ownedColumnNames(row);
            columnNames.emplace_back("value");
            fields.push_back(ruvia::detail::DbResultAccess::ownedField(value, &resource));
            rows.push_back(std::move(row));
            return result;
        };

        const std::string expected(2048, 'd');
        auto retained = makeRows(expected);
        const auto baseline = resource.liveAllocations();
        RUVIA_CHECK(baseline > 0);
        for (int index = 0; index != 100; ++index) {
            const std::string value(2048, static_cast<char>('a' + index % 26));
            {
                auto transient = makeRows(value);
                RUVIA_CHECK_EQ(transient.front()["value"].value(),
                    std::optional<std::string_view>(value));
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
            RUVIA_CHECK_EQ(retained.front()["value"].value(),
                std::optional<std::string_view>(expected));
        }
    }
    RUVIA_CHECK_EQ(dbResource.liveAllocations(), std::size_t{0});
#endif

#ifdef RUVIA_ENABLE_REDIS
    ruvia::test::CountingMemoryResource redisResource;
    {
        auto& resource = redisResource;
        const std::string expected(2048, 'r');
        auto retained = ruvia::detail::RedisTypesAccess::stringValue(expected, &resource);
        const auto baseline = resource.liveAllocations();
        RUVIA_CHECK(baseline > 0);
        for (int index = 0; index != 100; ++index) {
            const std::string value(2048, static_cast<char>('a' + index % 26));
            {
                auto transient = ruvia::detail::RedisTypesAccess::stringValue(value, &resource);
                RUVIA_CHECK_EQ(transient.string(), std::string_view(value));
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
            RUVIA_CHECK_EQ(retained.string(), std::string_view(expected));
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
        RUVIA_CHECK_EQ(retained.string(), std::string_view(expected));
    }
    RUVIA_CHECK_EQ(redisResource.liveAllocations(), std::size_t{0});
#endif
}
#endif

RUVIA_TEST(context_operation_client_failures_do_not_leave_request_arena_state) {
    ContextFixture fixture;
    auto context = ruvia::detail::ContextAccess::make(
        fixture.requestMemory, fixture.request,
        fixture.capabilities.contextServices(fixture.stopToken));

#ifdef RUVIA_ENABLE_DATABASE
    bool dbRejected = false;
#endif
#ifdef RUVIA_ENABLE_REDIS
    bool redisRejected = false;
#endif
    bool httpRejected = false;
#ifdef RUVIA_ENABLE_DATABASE
    try {
        static_cast<void>(context.db("missing"));
    } catch (const ruvia::DbError&) {
        dbRejected = true;
    }
#endif
#ifdef RUVIA_ENABLE_REDIS
    try {
        static_cast<void>(context.redis("missing"));
    } catch (const ruvia::RedisError&) {
        redisRejected = true;
    }
#endif
    try {
        static_cast<void>(context.httpClient("missing"));
    } catch (const ruvia::HttpClientError&) {
        httpRejected = true;
    }

#ifdef RUVIA_ENABLE_DATABASE
    RUVIA_CHECK(dbRejected);
#endif
#ifdef RUVIA_ENABLE_REDIS
    RUVIA_CHECK(redisRejected);
#endif
    RUVIA_CHECK(httpRejected);

    checkOperationDoesNotUseRequestArena(ruvia_ctx, fixture.requestMemory, [&] {
        auto operation = context.httpClient().send(
            ruvia::HttpClientRequestView{.method = "GET", .target = "/health"});
        static_cast<void>(operation);
    });
}
