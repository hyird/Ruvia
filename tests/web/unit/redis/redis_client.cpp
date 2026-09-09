#include <array>
#include <chrono>
#include <concepts>
#include <future>
#include <initializer_list>
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
#include <asio/use_future.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/App.h"
#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisTypesAccess.h"
#include "ruvia/web/redis/RedisHandle.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::test::RejectingMemoryResource;
using ruvia::test::TrackingResource;

using RedisDefinitions = std::span<const ruvia::detail::RedisDefinition>;

class RedisTestWorker final {
public:
    explicit RedisTestWorker(asio::io_context& ioContext)
        : dispatcher_(std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 64)),
          handle_(ruvia::detail::WorkerHandleAccess::make(dispatcher_)) {}

    RedisTestWorker(const RedisTestWorker&) = delete;
    RedisTestWorker& operator=(const RedisTestWorker&) = delete;

    [[nodiscard]] const ruvia::WorkerHandle& handle() const noexcept {
        return handle_;
    }

    void run() {
        dispatcher_->runContext();
    }

private:
    std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher_;
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
        : acceptor_(ioContext_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
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

    asio::io_context ioContext_;
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

// Command arguments passed as ordinary arguments rather than a prepared span.

// Alternating name/value commands need complete pairs.

// Variadic commands synchronously clone owning-string temporaries.

constexpr ruvia::RedisScanOptions kLiteralRedisScanOptions{
    .match = "session:*",
};

}  // namespace

RUVIA_TEST(
    redis_blocking_commands_ignore_the_ordinary_pool_timeout_and_require_a_cancellation_bound) {
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    ruvia::RedisConfig config;
    config.commandTimeout = std::chrono::milliseconds(1);
    const std::array definitions{redisDefinition("default", config)};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::detail::ScopedOperationScope generalScope;
    auto redis = registry.get(std::pmr::get_default_resource(), generalScope);
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
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    const std::array<ruvia::detail::RedisDefinition, 2> definitions{{
        redisDefinition("cache"),
        redisDefinition("default"),
    }};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::detail::ScopedOperationScope operationScope;

    bool defaultResolved = true;
    bool aliasResolved = true;
    try {
        (void)registry.get(std::pmr::get_default_resource(), operationScope);
    } catch (...) {
        defaultResolved = false;
    }
    try {
        (void)registry.get("cache", std::pmr::get_default_resource(), operationScope);
    } catch (...) {
        aliasResolved = false;
    }
    RUVIA_CHECK(defaultResolved);
    RUVIA_CHECK(aliasResolved);
}

RUVIA_TEST(redis_registry_rejects_an_invalid_worker) {
    asio::io_context ioContext;
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
    asio::io_context ioContext;
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
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::detail::ScopedOperationScope operationScope;
    auto handle = registry.get(std::pmr::get_default_resource(), operationScope);
    auto copiedHandle = handle;
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
    RUVIA_CHECK(handleRejected);
    RUVIA_CHECK(copyRejected);
    RUVIA_CHECK(builderRejected);
    RUVIA_CHECK(transactionRejected);
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
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::detail::ScopedOperationScope operationScope;
    auto redis = registry.get(std::pmr::get_default_resource(), operationScope);

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
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::detail::ScopedOperationScope operationScope;
    auto redis = registry.get(std::pmr::get_default_resource(), operationScope);
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
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    auto config = ruvia::RedisConfig{};
    config.host = "127.0.0.1";
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

    auto result =
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    std::jthread runner([&worker] { worker.run(); });
    server.waitUntilCommandRead();
    asio::post(ioContext, [&pool] { pool.closeNow(); });

    RUVIA_CHECK(result.get() == ruvia::RedisError::Code::kClosing);
    runner.join();
}

RUVIA_TEST(redis_operation_arguments_are_reclaimed_after_cancellation_and_failure) {
    asio::io_context ioContext;
    RedisTestWorker worker(ioContext);
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions, worker.handle());
    ruvia::test::CountingMemoryResource operationMemory;
    ruvia::detail::ScopedOperationScope operationScope;
    auto redis = registry.get(&operationMemory, operationScope);
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
            RUVIA_CHECK_EQ(operationMemory.liveAllocations(), std::size_t{0});
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
            RUVIA_CHECK_EQ(operationMemory.liveAllocations(), std::size_t{0});
        }
    };
    auto result =
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exercise()), asio::use_future);
    worker.run();
    result.get();

    RUVIA_CHECK(operationMemory.allocationCount() > 0);
    RUVIA_CHECK_EQ(operationMemory.allocationCount(), operationMemory.deallocationCount());
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
