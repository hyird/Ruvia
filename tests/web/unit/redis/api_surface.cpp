#include "test_harness.h"

#include <array>
#include <chrono>
#include <concepts>
#include <future>
#include <initializer_list>
#include <memory_resource>
#include <new>
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
#include "ruvia/web/App.h"
#include "ruvia/web/redis/RedisHandle.h"
#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisTypesAccess.h"

namespace {

[[nodiscard]] ruvia::detail::RedisDefinition redisDefinition(std::string_view alias, const ruvia::RedisConfig& config = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    return {
        std::pmr::string(alias, resource),
        ruvia::detail::RedisConfigStorage(config, resource),
    };
}

class RejectingMemoryResource final : public std::pmr::memory_resource {
public:
    void rejectAllocations(bool value = true) noexcept {
        rejecting_ = value;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (rejecting_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool rejecting_{false};
};

class TrackingResource final : public std::pmr::memory_resource {
public:
    void release() noexcept {
        released_ = true;
    }

    [[nodiscard]] bool deallocatedAfterRelease() const noexcept {
        return deallocatedAfterRelease_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        deallocatedAfterRelease_ = deallocatedAfterRelease_ || released_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool released_{false};
    bool deallocatedAfterRelease_{false};
};

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

static_assert(std::is_move_assignable_v<ruvia::RedisKeyValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisKeyValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisScoredValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisScoredValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisValue>);

template <typename T>
concept ExposesAnyRvalueRedisOwnedView = requires(T&& value) { std::move(value).duration(); } || requires(T&& value) { std::move(value).key(); } || requires(T&& value) { std::move(value).value(); } || requires(T&& value) { std::move(value).values(); } || requires(T&& value) { std::move(value).entries(); } || requires(T&& value) { std::move(value).fields(); } || requires(T&& value) { std::move(value).streams(); } || requires(T&& value) { std::move(value).stream(); } || requires(T&& value) { std::move(value).id(); } || requires(T&& value) { std::move(value).message(); } || requires(T&& value) { std::move(value).string(); } || requires(T&& value) { std::move(value).error(); } || requires(T&& value) { std::move(value).array(); };

template <typename T>
concept HasRedisErrorMessageAlias = requires(const T& error) { error.message(); };

static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisSetExpiration>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisKeyValue>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisScoredValue>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisHashScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisZScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisStreamEntry>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisStreamReadResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisXReadGroupResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisError>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisValue>);
static_assert(std::is_final_v<ruvia::RedisError>);
static_assert(std::derived_from<ruvia::RedisError, std::runtime_error>);
static_assert(!HasRedisErrorMessageAlias<ruvia::RedisError>);

template <typename T>
concept HasRedisHandleSpanArgs = requires(const T& handle, std::span<const std::string_view> keys, std::span<const std::pair<std::string_view, std::string_view>> pairs) {
    handle.command(keys);
    handle.mget(keys);
    handle.mset(pairs);
    handle.hset(std::string_view{}, pairs);
    handle.hmget(std::string_view{}, keys);
    handle.sinter(keys);
    handle.sunion(keys);
    handle.sdiff(keys);
    handle.scriptExists(keys);
};

template <typename T>
concept HasRedisHandleInitializerListArgs = requires(const T& handle, std::initializer_list<std::string_view> keys, std::initializer_list<std::pair<std::string_view, std::string_view>> pairs) {
    handle.command(keys);
    handle.mget(keys);
    handle.mset(pairs);
    handle.hset(std::string_view{}, pairs);
    handle.hmget(std::string_view{}, keys);
    handle.sinter(keys);
    handle.sunion(keys);
    handle.sdiff(keys);
    handle.scriptExists(keys);
};

template <typename T>
concept HasRedisPerCallCommandOptions = requires(
    const T& handle,
    std::span<const std::string_view> args) {
    handle.command(args, ruvia::OperationOptions{});
} || requires(const T& handle) {
    handle.command(ruvia::OperationOptions{}, "PING");
};

template <typename T>
concept HasRedisBlockingPerCallOptions = requires(
    const T& handle,
    std::span<const std::string_view> keys) {
    handle.blpop(keys, ruvia::RedisBlockWait::indefinitely(), ruvia::OperationOptions{});
    handle.brpop(keys, ruvia::RedisBlockWait::indefinitely(), ruvia::OperationOptions{});
};

template <typename T>
concept HasRedisXReadGroupOperationMember = requires(T& options) { options.operation; };

template <typename T>
concept HasRedisPipelineSpanCommand = requires(T& pipeline, std::span<const std::string_view> args) { pipeline.command(args); };

template <typename T>
concept HasRedisPipelineInitializerListCommand = requires(T& pipeline, std::initializer_list<std::string_view> args) { pipeline.command(args); };

template <typename T>
concept HasRedisTransactionSpanCommand = requires(T& transaction, std::span<const std::string_view> args) { transaction.command(args); };

template <typename T>
concept HasRedisTransactionInitializerListCommand = requires(T& transaction, std::initializer_list<std::string_view> args) { transaction.command(args); };

template <typename T>
concept HasRedisTransactionDiscard = requires(T& transaction) { transaction.discard(); };

template <typename T>
concept HasLvalueRedisExec = requires(T& batch) { batch.exec(); };

template <typename T>
concept HasRvalueRedisExec = requires(T& batch) { std::move(batch).exec(); };

template <typename T>
concept HasRvalueRedisExecOptions = requires(T& batch) { std::move(batch).exec(ruvia::OperationOptions{}); };

template <typename T>
concept HasLegacyRedisSetOptionBooleans = requires(T& options) {
    options.ttl;
    options.nx;
    options.xx;
    options.get;
    options.keepTtl;
};

template <typename T>
concept HasRedisSetReturnPreviousBoolean = requires(T& options) { options.returnPrevious = true; };

template <typename T>
concept HasRedisSetPreviousValuePolicy = requires(T& options) { options.previousValue = ruvia::RedisSetPreviousValuePolicy::kReturn; };

template <typename T>
concept HasRedisXReadGroupNoAckBoolean = requires(T& options) { options.noAck = true; };

template <typename T>
concept HasRedisXReadGroupAcknowledgementPolicy = requires(T& options) { options.acknowledgement = ruvia::RedisXReadGroupAcknowledgementPolicy::kNoAck; };

template <typename T>
concept HasRedisTcpNoDelayBoolean = requires(T& config) { config.tcpNoDelay = true; };

template <typename T>
concept HasRedisKeepAliveBoolean = requires(T& config) { config.keepAlive = true; };

template <typename T>
concept HasRedisTcpSocketPolicies = requires(T& config) {
    { config.tcpNoDelay } -> std::same_as<ruvia::TcpNoDelayPolicy&>;
    { config.tcpKeepAlive } -> std::same_as<ruvia::TcpKeepAlivePolicy&>;
};

template <typename T>
concept HasRedisRegistrationConfig = requires(T& app, ruvia::RedisConfig config) {
    { app.redis(ruvia::RedisRegistrationConfig{.config = config}) } ->
        std::same_as<ruvia::App&>;
};

template <typename T>
concept HasRedisRegistrationPositional = requires(T& app, ruvia::RedisConfig config) {
    app.redis(config);
} || requires(T& app, ruvia::RedisConfig config) {
    app.redis(std::string_view{}, config);
};

template <typename Match>
concept AcceptsRedisScanMatch = requires(Match&& match) { ruvia::RedisScanOptions{.match = std::forward<Match>(match)}; };

template <typename Match>
concept AssignsRedisScanMatch = requires(ruvia::RedisScanOptions& options, Match&& match) { options.match = std::forward<Match>(match); };

// Command arguments passed as ordinary arguments rather than a prepared span.
template <typename T>
concept HasRedisHandleVariadicArgs = requires(const T& handle, std::string_view key) {
    handle.command("TYPE", key);
    handle.mget(key, key);
    handle.mset(key, key, key, key);
    handle.hset(key, key, key, key, key);
    handle.hmget(key, key, key);
    handle.sinter(key, key);
    handle.sunion(key, key);
    handle.sdiff(key, key);
    handle.scriptExists(key);
};

// Alternating name/value commands need complete pairs.
template <typename T>
concept HasRedisHandleOddPairArgs = requires(const T& handle, std::string_view key) { handle.mset(key, key, key); };

// Variadic commands synchronously clone owning-string temporaries.
template <typename T>
concept HasRedisHandleOwningTemporaryArgs = requires(const T& handle, std::string_view key) { handle.mget(key, std::string("owned")); };

template <typename T>
concept HasRedisHandleOwningLvalueArgs = requires(const T& handle, std::string_view key, std::string owned) { handle.mget(key, owned); };

template <typename T>
concept HasRedisPipelineVariadicCommand = requires(T& pipeline, std::string_view key) { pipeline.command("TYPE", key); };

template <typename T>
concept HasRedisPipelineOwningTemporaryArgs = requires(T& pipeline) { pipeline.command("GET", std::string("owned")); };

template <typename T>
concept HasRedisTransactionVariadicCommand = requires(T& transaction, std::string_view key) { transaction.command("TYPE", key); };

template <typename T>
concept HasRedisTransactionOwningTemporaryArgs = requires(T& transaction) { transaction.command("GET", std::string("owned")); };

template <typename T>
concept HasRedisTransactionVariadicWatch = requires(T& transaction, std::string_view key) { transaction.watch(key, key); };

template <typename T>
concept HasLegacyRedisSetCommands = requires(const T& handle) {
    handle.setEx("key", std::chrono::seconds(1), "value");
} || requires(const T& handle) {
    handle.setNx("key", "value");
} || requires(const T& handle) {
    handle.getSet("key", "value");
};

template <typename T>
concept HasLegacyRedisBatchGetSet = requires(T& batch) { batch.getSet("key", "value"); };

static_assert(HasRedisHandleSpanArgs<ruvia::RedisHandle>);
static_assert(!HasRedisHandleInitializerListArgs<ruvia::RedisHandle>);
static_assert(HasRedisHandleVariadicArgs<ruvia::RedisHandle>);
static_assert(!HasRedisHandleOddPairArgs<ruvia::RedisHandle>);
static_assert(HasRedisHandleOwningTemporaryArgs<ruvia::RedisHandle>);
static_assert(HasRedisHandleOwningLvalueArgs<ruvia::RedisHandle>);
static_assert(HasRedisPipelineSpanCommand<ruvia::RedisPipeline>);
static_assert(!HasRedisPipelineInitializerListCommand<ruvia::RedisPipeline>);
static_assert(HasRedisPipelineVariadicCommand<ruvia::RedisPipeline>);
static_assert(HasRedisPipelineOwningTemporaryArgs<ruvia::RedisPipeline>);
static_assert(HasRedisTransactionSpanCommand<ruvia::RedisTransaction>);
static_assert(!HasRedisTransactionInitializerListCommand<ruvia::RedisTransaction>);
static_assert(HasRedisTransactionVariadicCommand<ruvia::RedisTransaction>);
static_assert(HasRedisTransactionOwningTemporaryArgs<ruvia::RedisTransaction>);
static_assert(HasRedisTransactionVariadicWatch<ruvia::RedisTransaction>);
static_assert(!HasRedisTransactionDiscard<ruvia::RedisTransaction>);
static_assert(!HasLegacyRedisSetCommands<ruvia::RedisHandle>);
static_assert(!HasLegacyRedisBatchGetSet<ruvia::RedisPipeline>);
static_assert(!HasLegacyRedisBatchGetSet<ruvia::RedisTransaction>);
static_assert(!HasLvalueRedisExec<ruvia::RedisPipeline>);
static_assert(HasRvalueRedisExec<ruvia::RedisPipeline>);
static_assert(!HasRvalueRedisExecOptions<ruvia::RedisPipeline>);
static_assert(!HasLvalueRedisExec<ruvia::RedisTransaction>);
static_assert(HasRvalueRedisExec<ruvia::RedisTransaction>);
static_assert(!HasRvalueRedisExecOptions<ruvia::RedisTransaction>);
static_assert(std::move_constructible<ruvia::RedisPipeline>);
static_assert(!std::assignable_from<ruvia::RedisPipeline&, ruvia::RedisPipeline&&>);
static_assert(std::move_constructible<ruvia::RedisTransaction>);
static_assert(!std::assignable_from<ruvia::RedisTransaction&, ruvia::RedisTransaction&&>);
static_assert(!HasLegacyRedisSetOptionBooleans<ruvia::RedisSetOptions>);
static_assert(!HasRedisSetReturnPreviousBoolean<ruvia::RedisSetOptions>);
static_assert(HasRedisSetPreviousValuePolicy<ruvia::RedisSetOptions>);
static_assert(std::same_as<decltype(std::declval<ruvia::RedisSetOptions>().condition), std::optional<ruvia::RedisSetCondition>>);
static_assert(std::same_as<decltype(std::declval<ruvia::RedisSetOptions>().expiration), std::optional<ruvia::RedisSetExpiration>>);
static_assert(std::same_as<decltype(std::declval<ruvia::RedisSetOptions>().previousValue), ruvia::RedisSetPreviousValuePolicy>);
static_assert(std::same_as<decltype(std::declval<const ruvia::RedisSetResult&>().applied()), bool>);
static_assert(std::same_as<decltype(std::declval<const ruvia::RedisSetResult&>().previous()), const std::optional<std::pmr::string>&>);
static_assert(!std::default_initializable<ruvia::RedisSetExpiration>);
static_assert(std::same_as<decltype(ruvia::RedisScanOptions{}.cursor), std::optional<ruvia::RedisScanCursor>>);
static_assert(!std::default_initializable<ruvia::RedisScanCursor>);
static_assert(std::same_as<decltype(std::declval<const ruvia::RedisTtl&>().remaining()), std::optional<std::chrono::milliseconds>>);
static_assert(std::same_as<decltype(ruvia::RedisScanOptions{}.count), std::optional<std::uint64_t>>);
static_assert(std::is_aggregate_v<ruvia::RedisScanOptions>);
static_assert(std::is_aggregate_v<ruvia::OperationOptions>);
static_assert(std::is_aggregate_v<ruvia::RedisXReadGroupOptions>);
static_assert(!HasRedisXReadGroupNoAckBoolean<ruvia::RedisXReadGroupOptions>);
static_assert(HasRedisXReadGroupAcknowledgementPolicy<ruvia::RedisXReadGroupOptions>);
static_assert(std::same_as<decltype(std::declval<ruvia::RedisXReadGroupOptions>().acknowledgement), ruvia::RedisXReadGroupAcknowledgementPolicy>);
static_assert(!HasRedisXReadGroupOperationMember<ruvia::RedisXReadGroupOptions>);
static_assert(!HasRedisPerCallCommandOptions<ruvia::RedisHandle>);
static_assert(!HasRedisBlockingPerCallOptions<ruvia::RedisHandle>);
static_assert(std::same_as<decltype(ruvia::RedisConfig{}.blockingPoolSizePerWorker), std::size_t>);
static_assert(ruvia::RedisConfig{}.connectTimeout.has_value());
static_assert(ruvia::RedisConfig{}.commandTimeout.has_value());
static_assert(ruvia::RedisConfig{}.acquireTimeout.has_value());
static_assert(ruvia::RedisConfig{}.connectTimeout == std::chrono::seconds(5));
static_assert(ruvia::RedisConfig{}.commandTimeout == std::chrono::seconds(30));
static_assert(ruvia::RedisConfig{}.acquireTimeout == std::chrono::seconds(5));
static_assert(HasRedisTcpSocketPolicies<ruvia::RedisConfig>);
static_assert(ruvia::RedisConfig{}.tcpNoDelay == ruvia::TcpNoDelayPolicy::kEnable);
static_assert(ruvia::RedisConfig{}.tcpKeepAlive == ruvia::TcpKeepAlivePolicy::kSystemDefault);
static_assert(!HasRedisTcpNoDelayBoolean<ruvia::RedisConfig>);
static_assert(!HasRedisKeepAliveBoolean<ruvia::RedisConfig>);
static_assert(std::is_aggregate_v<ruvia::RedisRegistrationConfig>);
static_assert(std::same_as<decltype(ruvia::RedisRegistrationConfig{.config = ruvia::RedisConfig{}}.alias), std::string>);
static_assert(HasRedisRegistrationConfig<ruvia::App>);
static_assert(!HasRedisRegistrationPositional<ruvia::App>);
constexpr ruvia::RedisScanOptions kLiteralRedisScanOptions{
    .match = "session:*",
};
static_assert(kLiteralRedisScanOptions.match.view() == "session:*");
static_assert(!AcceptsRedisScanMatch<std::string>);
static_assert(!AcceptsRedisScanMatch<const std::string>);
static_assert(!AcceptsRedisScanMatch<std::pmr::string>);
static_assert(AcceptsRedisScanMatch<std::string&>);
static_assert(AcceptsRedisScanMatch<std::pmr::string&>);
static_assert(AcceptsRedisScanMatch<std::string_view>);
static_assert(!AssignsRedisScanMatch<std::string>);
static_assert(!AssignsRedisScanMatch<const std::string>);
static_assert(!AssignsRedisScanMatch<std::pmr::string>);
static_assert(AssignsRedisScanMatch<std::string&>);
static_assert(AssignsRedisScanMatch<std::pmr::string&>);
static_assert(AssignsRedisScanMatch<std::string_view>);

}  // namespace

RUVIA_TEST(redis_api_surface_uses_span_args_without_initializer_list_overloads) {
    RUVIA_CHECK(true);
}

RUVIA_TEST(redis_blocking_commands_ignore_the_ordinary_pool_timeout_and_require_a_cancellation_bound) {
    asio::io_context ioContext;
    ruvia::RedisConfig config;
    config.commandTimeout = std::chrono::milliseconds(1);
    const std::array definitions{redisDefinition("default", config)};
    ruvia::detail::RedisRegistry registry(ioContext, std::pmr::get_default_resource(), definitions);
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
        (void)redis.xreadGroup("workers", "consumer", streams, {.block = ruvia::RedisBlockWait::forDuration(std::chrono::milliseconds(10))});
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
        (void)redis.xreadGroup("workers", "consumer", streams, {.block = ruvia::RedisBlockWait::indefinitely()});
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
    const std::array<ruvia::detail::RedisDefinition, 2> definitions{{
        redisDefinition("cache"),
        redisDefinition("default"),
    }};
    ruvia::detail::RedisRegistry registry(ioContext, std::pmr::get_default_resource(), definitions);
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

RUVIA_TEST(redis_registry_owns_nested_pmr_configuration) {
    TrackingResource sourceResource;
    std::pmr::unsynchronized_pool_resource targetResource;
    asio::io_context ioContext;
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
    registry.emplace(ioContext, &targetResource, std::span<const ruvia::detail::RedisDefinition>(&*definition, 1));
    definition.reset();
    sourceResource.release();
    registry.reset();

    RUVIA_CHECK(!sourceResource.deallocatedAfterRelease());
}

RUVIA_TEST(redis_request_capabilities_reject_after_parent_scope_closes) {
    asio::io_context ioContext;
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(ioContext, std::pmr::get_default_resource(), definitions);
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
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(ioContext, std::pmr::get_default_resource(), definitions);
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
    const std::array definitions{redisDefinition("default")};
    ruvia::detail::RedisRegistry registry(ioContext, std::pmr::get_default_resource(), definitions);
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
                    std::string_view(error.what()).find("reply 3") != std::string_view::npos &&
                    std::string_view(error.what()).find("EXECABORT") != std::string_view::npos;
    }
    RUVIA_CHECK(preserved);
}

RUVIA_TEST(redis_active_command_reports_pool_closing_instead_of_io_error) {
    StalledRedisCommandServer server;
    asio::io_context ioContext;
    auto config = ruvia::RedisConfig{};
    config.host = "127.0.0.1";
    config.port = server.port();
    config.commandTimeout = std::nullopt;
    auto* const resource = std::pmr::get_default_resource();
    ruvia::detail::RedisPool pool(
        ioContext,
        ruvia::detail::RedisConfigStorage(config, resource),
        1,
        resource);

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

    auto result = asio::co_spawn(
        ioContext,
        ruvia::detail::taskAsAwaitable(exercise()),
        asio::use_future);
    std::jthread runner([&ioContext] { ioContext.run(); });
    server.waitUntilCommandRead();
    asio::post(ioContext, [&pool] { pool.closeNow(); });

    RUVIA_CHECK(result.get() == ruvia::RedisError::Code::kClosing);
    runner.join();
}

RUVIA_TEST(redis_value_move_assignment_propagates_allocator_failure) {
    RejectingMemoryResource rejecting;
    const auto longValue = std::string_view("redis value large enough to exceed any small-string buffer");

    auto destination = ruvia::detail::RedisTypesAccess::keyValue({}, {}, &rejecting);
    auto source = ruvia::detail::RedisTypesAccess::keyValue(longValue, longValue, std::pmr::get_default_resource());
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
    auto sourceValue = ruvia::detail::RedisTypesAccess::stringValue(longValue, std::pmr::get_default_resource());
    rejecting.rejectAllocations();
    allocationFailure = false;
    try {
        destinationValue = std::move(sourceValue);
    } catch (const std::bad_alloc&) {
        allocationFailure = true;
    }
    RUVIA_CHECK(allocationFailure);
}
