#include "test_harness.h"

#include <array>
#include <chrono>
#include <concepts>
#include <initializer_list>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/redis/RedisHandle.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisTypesAccess.h"

namespace {

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

static_assert(std::is_move_assignable_v<ruvia::RedisKeyValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisKeyValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisScoredValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisScoredValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisValue>);

template <typename T>
concept ExposesAnyRvalueRedisOwnedView = requires(T&& value) { std::move(value).duration(); } || requires(T&& value) { std::move(value).key(); } || requires(T&& value) { std::move(value).value(); } || requires(T&& value) { std::move(value).values(); } || requires(T&& value) { std::move(value).entries(); } || requires(T&& value) { std::move(value).message(); } || requires(T&& value) { std::move(value).string(); } || requires(T&& value) { std::move(value).array(); };

static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisSetExpiration>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisKeyValue>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisScoredValue>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisHashScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisZScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisError>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisValue>);

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
concept HasLegacyRedisSetOptionBooleans = requires(T& options) {
    options.ttl;
    options.nx;
    options.xx;
    options.get;
    options.keepTtl;
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

// An owning-string temporary would leave the borrowed argument dangling; unlike
// DbValue there is no deleted constructor to catch it, so the concept must.
template <typename T>
concept HasRedisHandleOwningTemporaryArgs = requires(const T& handle, std::string_view key) { handle.mget(key, std::string("owned")); };

template <typename T>
concept HasRedisHandleOwningLvalueArgs = requires(const T& handle, std::string_view key, std::string owned) { handle.mget(key, owned); };

template <typename T>
concept HasRedisPipelineVariadicCommand = requires(T& pipeline, std::string_view key) { pipeline.command("TYPE", key); };

template <typename T>
concept HasRedisTransactionVariadicCommand = requires(T& transaction, std::string_view key) { transaction.command("TYPE", key); };

template <typename T>
concept HasRedisTransactionVariadicWatch = requires(T& transaction, std::string_view key) { transaction.watch(key, key); };

static_assert(HasRedisHandleSpanArgs<ruvia::RedisHandle>);
static_assert(!HasRedisHandleInitializerListArgs<ruvia::RedisHandle>);
static_assert(HasRedisHandleVariadicArgs<ruvia::RedisHandle>);
static_assert(!HasRedisHandleOddPairArgs<ruvia::RedisHandle>);
static_assert(!HasRedisHandleOwningTemporaryArgs<ruvia::RedisHandle>);
static_assert(HasRedisHandleOwningLvalueArgs<ruvia::RedisHandle>);
static_assert(HasRedisPipelineSpanCommand<ruvia::RedisPipeline>);
static_assert(!HasRedisPipelineInitializerListCommand<ruvia::RedisPipeline>);
static_assert(HasRedisPipelineVariadicCommand<ruvia::RedisPipeline>);
static_assert(HasRedisTransactionSpanCommand<ruvia::RedisTransaction>);
static_assert(!HasRedisTransactionInitializerListCommand<ruvia::RedisTransaction>);
static_assert(HasRedisTransactionVariadicCommand<ruvia::RedisTransaction>);
static_assert(HasRedisTransactionVariadicWatch<ruvia::RedisTransaction>);
static_assert(!HasRedisTransactionDiscard<ruvia::RedisTransaction>);
static_assert(!HasLvalueRedisExec<ruvia::RedisPipeline>);
static_assert(HasRvalueRedisExec<ruvia::RedisPipeline>);
static_assert(!HasLvalueRedisExec<ruvia::RedisTransaction>);
static_assert(HasRvalueRedisExec<ruvia::RedisTransaction>);
static_assert(std::move_constructible<ruvia::RedisPipeline>);
static_assert(!std::assignable_from<ruvia::RedisPipeline&, ruvia::RedisPipeline&&>);
static_assert(std::move_constructible<ruvia::RedisTransaction>);
static_assert(!std::assignable_from<ruvia::RedisTransaction&, ruvia::RedisTransaction&&>);
static_assert(!HasLegacyRedisSetOptionBooleans<ruvia::RedisSetOptions>);
static_assert(std::same_as<decltype(std::declval<ruvia::RedisSetOptions>().condition), std::optional<ruvia::RedisSetCondition>>);
static_assert(std::same_as<decltype(std::declval<ruvia::RedisSetOptions>().expiration), std::optional<ruvia::RedisSetExpiration>>);
static_assert(!std::default_initializable<ruvia::RedisSetExpiration>);
static_assert(std::same_as<decltype(ruvia::RedisScanOptions{}.count), std::optional<std::uint64_t>>);
static_assert(std::is_aggregate_v<ruvia::RedisScanOptions>);
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

RUVIA_TEST(redis_registry_derives_default_pool_from_owned_entry_index) {
    asio::io_context ioContext;
    const std::array<ruvia::detail::RedisDefinition, 2> definitions{{
        {std::pmr::string("cache"), ruvia::RedisConfig{}},
        {std::pmr::string("default"), ruvia::RedisConfig{}},
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
        .host = std::pmr::string(80, 'h', &sourceResource),
        .port = 6379,
        .username = std::pmr::string(80, 'u', &sourceResource),
        .password = std::pmr::string(80, 'p', &sourceResource),
        .database = 0,
        .poolSizePerWorker = 1,
    };
    definition.emplace(std::pmr::string("default", &sourceResource), std::move(config));

    std::optional<ruvia::detail::RedisRegistry> registry;
    registry.emplace(ioContext, &targetResource, std::span<const ruvia::detail::RedisDefinition>(&*definition, 1));
    definition.reset();
    sourceResource.release();
    registry.reset();

    RUVIA_CHECK(!sourceResource.deallocatedAfterRelease());
}

RUVIA_TEST(redis_request_capabilities_reject_after_parent_scope_closes) {
    asio::io_context ioContext;
    const std::array definitions{ruvia::detail::RedisDefinition{std::pmr::string("default"), ruvia::RedisConfig{}}};
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
