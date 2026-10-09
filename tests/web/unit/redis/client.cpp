#include <array>
#include <chrono>
#include <concepts>
#include <exception>
#include <future>
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
#include <thread>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/web/App.h"
#include "ruvia/web/redis/RedisHandle.h"

#include "memory_resource_fixture.h"
#include "redis/RedisHandleHelpers.h"
#include "redis/RedisRegistry.h"
#include "redis/RedisTypesAccess.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using ruvia::test::RejectingMemoryResource;
using ruvia::test::TrackingResource;

using RedisDefinitions = std::span<const ruvia::detail::RedisDefinition>;

class RedisTestWorker final {
public:
    explicit RedisTestWorker(asio::io_context& ioContext)
        : ioContext_(ioContext),
          attachment_(ruvia::attachEventLoop(ioContext)),
          handle_(attachment_.loop().handle()) {}

    RedisTestWorker(const RedisTestWorker&) = delete;
    RedisTestWorker& operator=(const RedisTestWorker&) = delete;

    [[nodiscard]] const ruvia::WorkerHandle& handle() const noexcept {
        return handle_;
    }

    void run() {
        attachment_.run();
    }

    void stop() noexcept {
        ioContext_.stop();
        attachment_.stop();
    }

private:
    asio::io_context& ioContext_;
    ruvia::EventLoopAttachment attachment_;
    ruvia::WorkerHandle handle_;
};

[[nodiscard]] ruvia::detail::RedisDefinition redisDefinition(std::string_view alias,
    const ruvia::RedisConfig& config = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    return {
        std::pmr::string(alias, resource),
        ruvia::detail::RedisConfigStorage(config, resource),
    };
}

class StalledRedisCommandServer final {
public:
    StalledRedisCommandServer()
        : ioContext_(ruvia::test::newTestIoContext()),
          acceptor_(ioContext_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          port_(acceptor_.local_endpoint().port()),
          commandReadFuture_(commandRead_.get_future()),
          thread_([this] { run(); }) {}

    ~StalledRedisCommandServer() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    StalledRedisCommandServer(const StalledRedisCommandServer&) = delete;
    StalledRedisCommandServer& operator=(const StalledRedisCommandServer&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

    void waitUntilCommandRead() {
        commandReadFuture_.get();
    }

private:
    void run() noexcept {
        try {
            asio::ip::tcp::socket socket(ioContext_);
            acceptor_.accept(socket);

            constexpr std::string_view ping = "*1\r\n$4\r\nPING\r\n";
            std::array<char, ping.size()> command{};
            std::error_code error;
            (void)asio::read(socket, asio::buffer(command), error);
            if (error) {
                throw std::system_error(error);
            }
            commandRead_.set_value();

            std::array<char, 1> ignoredByte{};
            (void)socket.read_some(asio::buffer(ignoredByte), error);
        } catch (...) {
            try {
                commandRead_.set_exception(std::current_exception());
            } catch (...) {
            }
        }
    }

    asio::io_context& ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::promise<void> commandRead_;
    std::future<void> commandReadFuture_;
    std::thread thread_;
};

template <typename Fn>
bool throwsInvalidArgument(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(
    redis_blocking_commands_ignore_the_ordinary_pool_timeout_and_require_a_cancellation_bound) {
    auto& ioContext = ruvia::test::newTestIoContext();
    RedisTestWorker worker(ioContext);
    ruvia::RedisConfig config;
    config.commandTimeout = std::chrono::milliseconds(1);
    const std::array definitions{redisDefinition("default", config)};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope generalScope;
    auto redis = registry.get(generalScope);
    const std::array<std::string_view, 1> keys{"queue"};
    const std::array streams{ruvia::RedisStreamReadView{.stream = "events", .id = ">"}};

    bool finitePopAccepted = true;
    bool finiteStreamAccepted = true;
    bool finiteRawAccepted = true;
    bool statefulRejected = false;
    bool clientStateRejected = false;
    bool helloRejected = false;
    bool askingRejected = false;
    try {
        (void)redis.blpop(keys, ruvia::RedisBlockWait::forDuration(std::chrono::seconds(1)));
    } catch (...) {
        finitePopAccepted = false;
    }
    try {
        (void)redis.xreadGroup("workers", "consumer", streams,
            {.block = ruvia::RedisBlockWait::forDuration(std::chrono::milliseconds(10))});
    } catch (...) {
        finiteStreamAccepted = false;
    }
    try {
        (void)redis.withOptions({.timeout = std::chrono::seconds(1)})
            .command("BLPOP", "queue", "1");
    } catch (...) {
        finiteRawAccepted = false;
    }
    try {
        (void)redis.command("SELECT", "1");
    } catch (const std::invalid_argument&) {
        statefulRejected = true;
    }
    try {
        (void)redis.command("CLIENT", "REPLY", "OFF");
    } catch (const std::invalid_argument&) {
        clientStateRejected = true;
    }
    try {
        (void)redis.command("HELLO", "3");
    } catch (const std::invalid_argument&) {
        helloRejected = true;
    }
    try {
        (void)redis.command("ASKING");
    } catch (const std::invalid_argument&) {
        askingRejected = true;
    }

    bool infiniteStreamRejected = false;
    bool infinitePopRejected = false;
    bool unboundedRawRejected = false;
    try {
        (void)redis.xreadGroup(
            "workers", "consumer", streams, {.block = ruvia::RedisBlockWait::indefinitely()});
    } catch (const std::invalid_argument&) {
        infiniteStreamRejected = true;
    }
    try {
        (void)redis.blpop(keys, ruvia::RedisBlockWait::indefinitely());
    } catch (const std::invalid_argument&) {
        infinitePopRejected = true;
    }
    try {
        (void)redis.command("BLPOP", "queue", "0");
    } catch (const std::invalid_argument&) {
        unboundedRawRejected = true;
    }

    RUVIA_CHECK(finitePopAccepted);
    RUVIA_CHECK(finiteStreamAccepted);
    RUVIA_CHECK(finiteRawAccepted);
    RUVIA_CHECK(statefulRejected);
    RUVIA_CHECK(clientStateRejected);
    RUVIA_CHECK(helloRejected);
    RUVIA_CHECK(askingRejected);
    RUVIA_CHECK(infiniteStreamRejected);
    RUVIA_CHECK(infinitePopRejected);
    RUVIA_CHECK(unboundedRawRejected);
}

RUVIA_TEST(redis_registry_derives_default_pool_from_owned_entry_index) {
    auto& ioContext = ruvia::test::newTestIoContext();
    RedisTestWorker worker(ioContext);
    const std::array<ruvia::detail::RedisDefinition, 2> definitions{{
        redisDefinition("cache"),
        redisDefinition("default"),
    }};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope operationScope;

    bool defaultResolved = true;
    bool aliasResolved = true;
    try {
        (void)registry.get(operationScope);
    } catch (...) {
        defaultResolved = false;
    }
    try {
        (void)registry.get("cache", operationScope);
    } catch (...) {
        aliasResolved = false;
    }
    RUVIA_CHECK(defaultResolved);
    RUVIA_CHECK(aliasResolved);
}

RUVIA_TEST(redis_registry_rejects_an_invalid_worker) {
    auto& ioContext = ruvia::test::newTestIoContext();
    const RedisDefinitions definitions;
    const ruvia::WorkerHandle worker;

    RUVIA_CHECK(throwsInvalidArgument([&] {
        ruvia::detail::RedisRegistry registry(
            ioContext, std::pmr::get_default_resource(), definitions, worker);
    }));
}

RUVIA_TEST(redis_registry_owns_nested_pmr_configuration) {
    TrackingResource sourceResource;
    std::pmr::unsynchronized_pool_resource targetResource;
    auto& ioContext = ruvia::test::newTestIoContext();
    RedisTestWorker worker(ioContext);
    std::optional<ruvia::detail::RedisDefinition> definition;
    ruvia::RedisConfig config{
        .host = std::string(80, 'h'),
        .port = 6379,
        .username = std::string(80, 'u'),
        .password = std::string(80, 'p'),
        .database = 0,
        .poolSizePerWorker = 1,
    };
    definition.emplace(redisDefinition("default", config, &sourceResource));

    std::optional<ruvia::detail::RedisRegistry> registry;
    registry.emplace(ioContext, &targetResource,
        std::span<const ruvia::detail::RedisDefinition>(&*definition, 1), worker.handle());
    definition.reset();
    sourceResource.release();
    registry.reset();

    RUVIA_CHECK(!sourceResource.deallocatedAfterRelease());
}

RUVIA_TEST(redis_request_capabilities_reject_after_parent_scope_closes) {
    auto& ioContext = ruvia::test::newTestIoContext();
    RedisTestWorker worker(ioContext);
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope operationScope;
    auto handle = registry.get(operationScope);
    auto copiedHandle = handle;
    auto configuredHandle = handle.withOptions({.timeout = std::chrono::seconds(3)});
    auto copiedConfiguredHandle = configuredHandle;
    auto derivedConfiguredHandle = copiedConfiguredHandle.withOptions(
        {.timeout = std::chrono::seconds(1)});
    auto pipeline = handle.pipeline();
    pipeline.get("key");
    auto movedPipeline = std::move(pipeline);
    auto transaction = handle.transaction();
    transaction.get("key");
    auto movedTransaction = std::move(transaction);

    operationScope.close();
    bool handleRejected = false;
    bool copyRejected = false;
    bool builderRejected = false;
    bool transactionRejected = false;
    const auto optionFailureOrder = [](const auto& capability) {
        bool validationWrongOrder = false;
        bool lifetimeRejected = false;
        try {
            (void)capability.withOptions({.timeout = std::chrono::milliseconds::zero()});
        } catch (const std::invalid_argument&) {
            validationWrongOrder = true;
        } catch (const std::logic_error&) {
            lifetimeRejected = true;
        }
        return std::pair{validationWrongOrder, lifetimeRejected};
    };
    const auto handleOptionFailure = optionFailureOrder(handle);
    const auto configuredOptionFailure = optionFailureOrder(configuredHandle);
    const auto copiedConfiguredOptionFailure = optionFailureOrder(copiedConfiguredHandle);
    const auto derivedConfiguredOptionFailure = optionFailureOrder(derivedConfiguredHandle);
    const bool expiredOptionsRejectedBeforeValidation =
        !handleOptionFailure.first && handleOptionFailure.second &&
        !configuredOptionFailure.first && configuredOptionFailure.second &&
        !copiedConfiguredOptionFailure.first && copiedConfiguredOptionFailure.second &&
        !derivedConfiguredOptionFailure.first && derivedConfiguredOptionFailure.second;
    try {
        (void)handle.ping();
    } catch (const std::logic_error&) {
        handleRejected = true;
    }
    try {
        (void)copiedHandle.ping();
    } catch (const std::logic_error&) {
        copyRejected = true;
    }
    try {
        movedPipeline.get("other");
    } catch (const std::logic_error&) {
        builderRejected = true;
    }
    try {
        movedTransaction.get("other");
    } catch (const std::logic_error&) {
        transactionRejected = true;
    }
    RUVIA_CHECK(expiredOptionsRejectedBeforeValidation);
    RUVIA_CHECK(handleRejected);
    RUVIA_CHECK(copyRejected);
    RUVIA_CHECK(builderRejected);
    RUVIA_CHECK(transactionRejected);
}

RUVIA_TEST(redis_batch_builders_own_cold_payload_and_reject_reuse) {
    auto& io_context = ruvia::test::newTestIoContext();
    RedisTestWorker worker(io_context);
    ruvia::test::CountingMemoryResource memory;
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(io_context, &memory, definitions, worker.handle());
    ruvia::operation_scope scope;
    auto handle = registry.get(scope);
    const auto baseline = memory.liveAllocations();
    const auto throws_logic_error = [](auto&& operation) {
        try {
            operation();
        } catch (const std::logic_error&) {
            return true;
        }
        return false;
    };
    {
        auto pipeline = handle.pipeline();
        pipeline.set(std::string(128, 'k'), std::string(128, 'v'));
        std::optional moved(std::move(pipeline));
        RUVIA_CHECK(throws_logic_error([&] { pipeline.get("moved"); }));
        auto cold = std::move(*moved).exec();
        RUVIA_CHECK(throws_logic_error([&] { moved->incrBy("used", 1); }));
        RUVIA_CHECK(throws_logic_error([&] { (void)std::move(*moved).exec(); }));
        moved.reset();
        RUVIA_CHECK(memory.liveAllocations() > baseline);
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
    {
        auto transaction = handle.transaction();
        transaction.watch(std::string(128, 'w')).unwatch().set(std::string(128, 'k'), std::string(128, 'v'));
        std::optional moved(std::move(transaction));
        RUVIA_CHECK(throws_logic_error([&] { transaction.watch("moved"); }));
        auto cold = std::move(*moved).exec();
        RUVIA_CHECK(throws_logic_error([&] { moved->unwatch(); }));
        RUVIA_CHECK(throws_logic_error([&] { moved->zadd("used", 1, "member"); }));
        moved.reset();
        RUVIA_CHECK(memory.liveAllocations() > baseline);
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
    {
        auto pipeline = handle.pipeline();
        auto transaction = handle.transaction();
        for (auto word : {"WATCH", "UNWATCH", "MULTI", "EXEC", "BLPOP"}) {
            RUVIA_CHECK(throwsInvalidArgument([&] { pipeline.command(word, "key"); }));
            RUVIA_CHECK(throwsInvalidArgument([&] { transaction.command(word, "key"); }));
        }
        for (auto score : {std::numeric_limits<double>::infinity(),
                 -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            RUVIA_CHECK(throwsInvalidArgument([&] { pipeline.zadd("key", score, "member"); }));
            RUVIA_CHECK(throwsInvalidArgument([&] { transaction.zadd("key", score, "member"); }));
        }
        pipeline.set(std::string(128, 'k'), std::string(128, 'v'));
        transaction.watch(std::string(128, 'w')).set(std::string(128, 'k'), std::string(128, 'v'));
        scope.close();
        RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
        RUVIA_CHECK(throws_logic_error([&] {
            pipeline.zadd("expired", std::numeric_limits<double>::quiet_NaN(), "member");
        }));
        RUVIA_CHECK(throws_logic_error([&] { (void)std::move(transaction).exec(); }));
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
}

RUVIA_TEST(redis_batch_typed_commands_preserve_order_arguments_and_resource) {
    auto& io_context = ruvia::test::newTestIoContext();
    RedisTestWorker worker(io_context);
    ruvia::test::CountingMemoryResource memory;
    const ruvia::detail::RedisConfigStorage config(ruvia::RedisConfig{}, &memory);
    ruvia::detail::RedisPool pool(io_context, config, config.commandTimeout, 1, worker.handle(), &memory);
    const auto baseline = memory.liveAllocations();
    {
        ruvia::detail::redis_command_batch batch(pool, {}, &memory);
        const auto minimum = std::numeric_limits<std::int64_t>::min();
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        batch.get("k");
        batch.set("k", "v");
        batch.get_del("k");
        batch.append("k", "v");
        batch.strlen("k");
        batch.del("k");
        batch.unlink("k");
        batch.exists("k");
        batch.touch("k");
        batch.type("k");
        batch.rename("k", "n");
        batch.rename_nx("k", "n");
        batch.incr("k");
        batch.incr_by("k", minimum);
        batch.decr("k");
        batch.decr_by("k", maximum);
        batch.hget("k", "f");
        batch.hset("k", "f", "v");
        batch.hdel("k", "f");
        batch.hexists("k", "f");
        batch.hlen("k");
        batch.hget_all("k");
        batch.lpush("k", "v");
        batch.rpush("k", "v");
        batch.lpop("k");
        batch.rpop("k");
        batch.llen("k");
        batch.lrange("k", minimum, maximum);
        batch.sadd("k", "m");
        batch.srem("k", "m");
        batch.smembers("k");
        batch.scard("k");
        batch.zadd("k", 1.5, "m");
        batch.zrem("k", "m");
        batch.zrange("k", minimum, maximum);
        batch.zscore("k", "m");
        batch.zcard("k");
        const std::string binary_key("k\0ey", 4);
        const std::string binary_value("v\0alue", 6);
        auto source_key = binary_key;
        auto source_value = binary_value;
        batch.set(source_key, source_value);
        source_key.assign(128, 'x');
        source_value.assign(128, 'y');
        auto moved = std::move(batch);
        auto payload = moved.consume();
        RUVIA_CHECK_EQ(payload.commands.size(), std::size_t{38});
        RUVIA_CHECK(payload.commands.get_allocator().resource() == &memory);
        const std::initializer_list<std::initializer_list<std::string_view>> expected{
            {"GET", "k"},
            {"SET", "k", "v"},
            {"GETDEL", "k"},
            {"APPEND", "k", "v"},
            {"STRLEN", "k"},
            {"DEL", "k"},
            {"UNLINK", "k"},
            {"EXISTS", "k"},
            {"TOUCH", "k"},
            {"TYPE", "k"},
            {"RENAME", "k", "n"},
            {"RENAMENX", "k", "n"},
            {"INCR", "k"},
            {"INCRBY", "k", "-9223372036854775808"},
            {"DECR", "k"},
            {"DECRBY", "k", "9223372036854775807"},
            {"HGET", "k", "f"},
            {"HSET", "k", "f", "v"},
            {"HDEL", "k", "f"},
            {"HEXISTS", "k", "f"},
            {"HLEN", "k"},
            {"HGETALL", "k"},
            {"LPUSH", "k", "v"},
            {"RPUSH", "k", "v"},
            {"LPOP", "k"},
            {"RPOP", "k"},
            {"LLEN", "k"},
            {"LRANGE", "k", "-9223372036854775808", "9223372036854775807"},
            {"SADD", "k", "m"},
            {"SREM", "k", "m"},
            {"SMEMBERS", "k"},
            {"SCARD", "k"},
            {"ZADD", "k", "1.5", "m"},
            {"ZREM", "k", "m"},
            {"ZRANGE", "k", "-9223372036854775808", "9223372036854775807"},
            {"ZSCORE", "k", "m"},
            {"ZCARD", "k"},
        };
        std::size_t command_index = 0;
        for (auto arguments : expected) {
            const auto& command = payload.commands[command_index++];
            RUVIA_CHECK_EQ(command.args.size(), arguments.size());
            std::size_t argument_index = 0;
            for (auto argument : arguments) {
                const auto& actual = command.args[argument_index++];
                RUVIA_CHECK(actual == argument);
                RUVIA_CHECK(actual.get_allocator().resource() == &memory);
            }
        }
        RUVIA_CHECK(payload.commands.back().args[1] == std::string_view(binary_key));
        RUVIA_CHECK(payload.commands.back().args[2] == std::string_view(binary_value));
        moved.expire();
        RUVIA_CHECK(payload.commands.back().args[2] == std::string_view(binary_value));
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
}

RUVIA_TEST(redis_set_expiration_cannot_represent_conflicting_modes) {
    const auto expiring = ruvia::RedisSetExpiration::expiresAfter(std::chrono::milliseconds(1500));
    RUVIA_CHECK(expiring.duration() != nullptr);
    RUVIA_CHECK_EQ(expiring.duration()->count(), std::chrono::milliseconds::rep{1500});
    RUVIA_CHECK(!expiring.keepsExisting());

    const auto keep = ruvia::RedisSetExpiration::keepExisting();
    RUVIA_CHECK(keep.duration() == nullptr);
    RUVIA_CHECK(keep.keepsExisting());

    bool zeroRejected = false;
    try {
        (void)ruvia::RedisSetExpiration::expiresAfter(std::chrono::milliseconds(0));
    } catch (const std::invalid_argument&) {
        zeroRejected = true;
    }
    RUVIA_CHECK(zeroRejected);

    bool negativeRejected = false;
    try {
        (void)ruvia::RedisSetExpiration::expiresAfter(std::chrono::milliseconds(-1));
    } catch (const std::invalid_argument&) {
        negativeRejected = true;
    }
    RUVIA_CHECK(negativeRejected);
}

RUVIA_TEST(redis_expire_rejects_non_positive_ttl_before_io) {
    auto& ioContext = ruvia::test::newTestIoContext();
    RedisTestWorker worker(ioContext);
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope operationScope;
    auto redis = registry.get(operationScope);

    bool zeroRejected = false;
    try {
        (void)redis.expire("key", std::chrono::seconds(0));
    } catch (const std::invalid_argument&) {
        zeroRejected = true;
    }
    RUVIA_CHECK(zeroRejected);

    bool negativeRejected = false;
    try {
        (void)redis.expire("key", std::chrono::seconds(-1));
    } catch (const std::invalid_argument&) {
        negativeRejected = true;
    }
    RUVIA_CHECK(negativeRejected);
}

RUVIA_TEST(redis_multi_key_commands_reject_empty_key_spans_before_io) {
    auto& ioContext = ruvia::test::newTestIoContext();
    RedisTestWorker worker(ioContext);
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::operation_scope operationScope;
    auto redis = registry.get(operationScope);
    const std::span<const std::string_view> noKeys;

    RUVIA_CHECK(throwsInvalidArgument([&] { (void)redis.mget(noKeys); }));
    RUVIA_CHECK(throwsInvalidArgument([&] { (void)redis.sinter(noKeys); }));
    RUVIA_CHECK(throwsInvalidArgument([&] { (void)redis.sunion(noKeys); }));
    RUVIA_CHECK(throwsInvalidArgument([&] { (void)redis.sdiff(noKeys); }));
}

RUVIA_TEST(redis_transaction_errors_preserve_server_diagnostics) {
    const auto reply = ruvia::detail::RedisTypesAccess::errorValue(
        "EXECABORT Transaction discarded because of previous errors.",
        std::pmr::get_default_resource());
    bool preserved = false;
    try {
        ruvia::detail::throwIfRedisTransactionReplyError(reply, 3);
    } catch (const ruvia::RedisError& error) {
        preserved = error.code() == ruvia::RedisError::Code::kCommandError &&
                    std::string_view(error.what()).contains("reply 3") &&
                    std::string_view(error.what()).contains("EXECABORT");
    }
    RUVIA_CHECK(preserved);
}

RUVIA_TEST(redis_active_command_reports_pool_closing_instead_of_io_error) {
    StalledRedisCommandServer server;
    auto& ioContext = ruvia::test::newTestIoContext();
    RedisTestWorker worker(ioContext);
    auto config = ruvia::RedisConfig{};
    config.host = "127.0.0.1";
    config.tls.mode = ruvia::client_tls_mode::disabled;
    config.port = server.port();
    config.commandTimeout = std::nullopt;
    auto* const resource = std::pmr::get_default_resource();
    const auto storedConfig = ruvia::detail::RedisConfigStorage(config, resource);
    ruvia::detail::RedisPool pool(
        ioContext, storedConfig, storedConfig.commandTimeout, 1, worker.handle(), resource);

    auto exercise = [&]() -> ruvia::Task<ruvia::RedisError::Code> {
        std::pmr::vector<std::pmr::string> args(resource);
        args.emplace_back("PING");
        try {
            (void)co_await pool.executeOwned(std::move(args), resource);
        } catch (const ruvia::RedisError& error) {
            co_return error.code();
        }
        co_return ruvia::RedisError::Code::kProtocolError;
    };

    std::promise<ruvia::RedisError::Code> completion;
    auto result = completion.get_future();
    asio::co_spawn(ioContext, ruvia::asAwaitable(exercise()),
        [&worker, &completion](std::exception_ptr error, ruvia::RedisError::Code code) {
            if (error) {
                completion.set_exception(std::move(error));
            } else {
                completion.set_value(code);
            }
            worker.stop();
        });
    std::jthread runner([&worker] { worker.run(); });
    server.waitUntilCommandRead();
    asio::post(ioContext, [&pool] { pool.closeNow(); });

    runner.join();
    const auto code = result.get();
    RUVIA_CHECK(code == ruvia::RedisError::Code::kClosing);
}

RUVIA_TEST(redis_operation_arguments_are_reclaimed_after_cancellation_and_failure) {
    auto& ioContext = ruvia::test::newTestIoContext();
    RedisTestWorker worker(ioContext);
    const std::array definitions{redisDefinition("default")};
    ruvia::test::CountingMemoryResource operationMemory;
    ruvia::detail::RedisRegistry registry(
        ioContext, &operationMemory, definitions, worker.handle());
    const auto registryLiveAllocations = operationMemory.liveAllocations();
    ruvia::operation_scope operationScope;
    auto redis = registry.get(operationScope);
    ruvia::StopSource cancellation;
    cancellation.requestStop();
    auto cancelled = redis.withOptions({.stopToken = cancellation.token()});
    const std::string key(2048, 'k');

    auto exercise = [&]() -> ruvia::Task<void> {
        for (int index = 0; index != 128; ++index) {
            bool rejected = false;
            try {
                (void)co_await cancelled.get(key);
            } catch (const ruvia::RedisError& error) {
                rejected = error.code() == ruvia::RedisError::Code::kCancelled;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(operationMemory.liveAllocations(), registryLiveAllocations);
        }
        registry.closeNow();
        for (int index = 0; index != 128; ++index) {
            bool rejected = false;
            try {
                (void)co_await redis.get(key);
            } catch (const ruvia::RedisError& error) {
                rejected = error.code() == ruvia::RedisError::Code::kClosing;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(operationMemory.liveAllocations(), registryLiveAllocations);
        }
    };
    std::promise<void> completion;
    auto result = completion.get_future();
    asio::co_spawn(ioContext, ruvia::asAwaitable(exercise()),
        [&worker, &completion](std::exception_ptr error) {
            if (error) {
                completion.set_exception(std::move(error));
            } else {
                completion.set_value();
            }
            worker.stop();
        });
    worker.run();
    result.get();

    RUVIA_CHECK(operationMemory.allocationCount() > 0);
    RUVIA_CHECK_EQ(operationMemory.liveAllocations(), registryLiveAllocations);
}

RUVIA_TEST(redis_value_move_assignment_propagates_allocator_failure) {
    RejectingMemoryResource rejecting;
    const auto longValue =
        std::string_view("redis value large enough to exceed any small-string buffer");

    auto destination = ruvia::detail::RedisTypesAccess::keyValue({}, {}, &rejecting);
    auto source = ruvia::detail::RedisTypesAccess::keyValue(
        longValue, longValue, std::pmr::get_default_resource());
    rejecting.rejectAllocations();
    bool allocationFailure = false;
    try {
        destination = std::move(source);
    } catch (const std::bad_alloc&) {
        allocationFailure = true;
    }
    RUVIA_CHECK(allocationFailure);

    rejecting.rejectAllocations(false);
    auto destinationValue = ruvia::detail::RedisTypesAccess::nullValue(&rejecting);
    auto sourceValue =
        ruvia::detail::RedisTypesAccess::stringValue(longValue, std::pmr::get_default_resource());
    rejecting.rejectAllocations();
    allocationFailure = false;
    try {
        destinationValue = std::move(sourceValue);
    } catch (const std::bad_alloc&) {
        allocationFailure = true;
    }
    RUVIA_CHECK(allocationFailure);
}

RUVIA_TEST(redis_value_array_move_assignment_uses_destination_resource) {
    ruvia::test::CountingMemoryResource source_resource;
    ruvia::test::CountingMemoryResource destination_resource;
    const std::string value(128, 'v');
    {
        auto destination = ruvia::detail::RedisTypesAccess::nullValue(&destination_resource);
        {
            std::pmr::vector<ruvia::RedisValue> values(&source_resource);
            values.push_back(ruvia::detail::RedisTypesAccess::stringValue(value, &source_resource));
            auto source = ruvia::detail::RedisTypesAccess::arrayValue(std::move(values), &source_resource);
            destination = std::move(source);
        }
        RUVIA_CHECK_EQ(source_resource.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(destination.array().size(), std::size_t{1});
        if (destination.array().size() == 1) {
            RUVIA_CHECK_EQ(destination.array().front().string(), std::string_view(value));
        }
    }
    RUVIA_CHECK_EQ(destination_resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(source_resource.liveAllocations(), std::size_t{0});
}

namespace {
template <typename Factory, typename Populate, typename Check>
void check_redis_resource_transfer(ruvia::testing::TestContext& ruvia_ctx,
    Factory factory, Populate populate, Check check) {
    for (const bool copy : {false, true}) {
        ruvia::test::CountingMemoryResource source_resource;
        ruvia::test::CountingMemoryResource destination_resource;
        {
            auto destination = factory(&destination_resource);
            {
                auto source = factory(&source_resource);
                populate(source, &source_resource);
                if (copy) {
                    destination = source;
                } else {
                    destination = std::move(source);
                }
            }
            RUVIA_CHECK_EQ(source_resource.liveAllocations(), std::size_t{0});
            check(destination);
        }
        RUVIA_CHECK_EQ(source_resource.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(destination_resource.liveAllocations(), std::size_t{0});
    }
}
}  // namespace

RUVIA_TEST(redis_typed_result_transfers_use_destination_resource) {
    using Access = ruvia::detail::RedisTypesAccess;
    const std::string key(128, 'k');
    const std::string value(128, 'v');
    check_redis_resource_transfer(ruvia_ctx, [](auto* resource) { return Access::hashScanResult(resource); }, [&](auto& result, auto* resource) { Access::entries(result).push_back(Access::keyValue(key, value, resource)); }, [&](const auto& result) {
            RUVIA_CHECK_EQ(result.entries().size(), std::size_t{1});
            if (result.entries().size() == 1) {
                RUVIA_CHECK_EQ(result.entries().front().key(), std::string_view(key));
                RUVIA_CHECK_EQ(result.entries().front().value(), std::string_view(value));
            } });
    check_redis_resource_transfer(ruvia_ctx, [](auto* resource) { return Access::zScanResult(resource); }, [&](auto& result, auto* resource) { Access::entries(result).push_back(Access::scoredValue(value, 2.5, resource)); }, [&](const auto& result) {
            RUVIA_CHECK_EQ(result.entries().size(), std::size_t{1});
            if (result.entries().size() == 1) {
                RUVIA_CHECK_EQ(result.entries().front().value(), std::string_view(value));
                RUVIA_CHECK_EQ(result.entries().front().score(), 2.5);
            } });
    check_redis_resource_transfer(ruvia_ctx, [](auto* resource) { return Access::xreadGroupResult(resource); }, [&](auto& result, auto* resource) {
            auto stream = Access::streamReadResult(key, resource);
            auto entry = Access::streamEntry(value, resource);
            Access::fields(entry).push_back(Access::keyValue(key, value, resource));
            Access::entries(stream).push_back(std::move(entry));
            Access::streams(result).push_back(std::move(stream)); }, [&](const auto& result) {
            RUVIA_CHECK_EQ(result.streams().size(), std::size_t{1});
            if (result.streams().size() != 1) {
                return;
            }
            const auto& stream = result.streams().front();
            RUVIA_CHECK_EQ(stream.stream(), std::string_view(key));
            RUVIA_CHECK_EQ(stream.entries().size(), std::size_t{1});
            if (stream.entries().size() != 1) {
                return;
            }
            const auto& entry = stream.entries().front();
            RUVIA_CHECK_EQ(entry.id(), std::string_view(value));
            RUVIA_CHECK_EQ(entry.fields().size(), std::size_t{1});
            if (entry.fields().size() == 1) {
                RUVIA_CHECK_EQ(entry.fields().front().key(), std::string_view(key));
                RUVIA_CHECK_EQ(entry.fields().front().value(), std::string_view(value));
            } });
}
