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
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/redis/RedisHandle.h"
#include "ruvia/web/detail/redis/RedisInternal.h"
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

static_assert(std::is_move_assignable_v<ruvia::RedisKeyValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisKeyValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisScoredValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisScoredValue>);
static_assert(std::is_move_assignable_v<ruvia::RedisValue>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::RedisValue>);

template <typename T>
concept ExposesAnyRvalueRedisOwnedView =
    requires(T&& value) { std::move(value).duration(); } ||
    requires(T&& value) { std::move(value).key(); } ||
    requires(T&& value) { std::move(value).value(); } ||
    requires(T&& value) { std::move(value).values(); } ||
    requires(T&& value) { std::move(value).entries(); } ||
    requires(T&& value) { std::move(value).message(); } ||
    requires(T&& value) { std::move(value).string(); } ||
    requires(T&& value) { std::move(value).array(); };

static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisSetExpiration>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisKeyValue>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisScoredValue>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisHashScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisZScanResult>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisError>);
static_assert(!ExposesAnyRvalueRedisOwnedView<ruvia::RedisValue>);

template <typename T>
concept HasRedisHandleSpanArgs = requires(
    const T& handle,
    std::span<const std::string_view> keys,
    std::span<const std::pair<std::string_view, std::string_view>> pairs) {
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
concept HasRedisHandleInitializerListArgs = requires(
    const T& handle,
    std::initializer_list<std::string_view> keys,
    std::initializer_list<std::pair<std::string_view, std::string_view>> pairs) {
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
concept HasRedisPipelineSpanCommand = requires(T& pipeline, std::span<const std::string_view> args) {
    pipeline.command(args);
};

template <typename T>
concept HasRedisPipelineInitializerListCommand =
    requires(T& pipeline, std::initializer_list<std::string_view> args) {
        pipeline.command(args);
    };

template <typename T>
concept HasRedisTransactionSpanCommand = requires(T& transaction, std::span<const std::string_view> args) {
    transaction.command(args);
};

template <typename T>
concept HasRedisTransactionInitializerListCommand =
    requires(T& transaction, std::initializer_list<std::string_view> args) {
        transaction.command(args);
    };

template <typename T>
concept HasRedisTransactionDiscard = requires(T& transaction) {
    transaction.discard();
};

template <typename T>
concept HasLvalueRedisExec = requires(T& batch) {
    batch.exec();
};

template <typename T>
concept HasRvalueRedisExec = requires(T& batch) {
    std::move(batch).exec();
};

template <typename T>
concept HasLegacyRedisSetOptionBooleans = requires(T& options) {
    options.ttl;
    options.nx;
    options.xx;
    options.get;
    options.keepTtl;
};

static_assert(HasRedisHandleSpanArgs<ruvia::RedisHandle>);
static_assert(!HasRedisHandleInitializerListArgs<ruvia::RedisHandle>);
static_assert(HasRedisPipelineSpanCommand<ruvia::RedisPipeline>);
static_assert(!HasRedisPipelineInitializerListCommand<ruvia::RedisPipeline>);
static_assert(HasRedisTransactionSpanCommand<ruvia::RedisTransaction>);
static_assert(!HasRedisTransactionInitializerListCommand<ruvia::RedisTransaction>);
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
static_assert(std::same_as<
    decltype(std::declval<ruvia::RedisSetOptions>().condition),
    std::optional<ruvia::RedisSetCondition>>);
static_assert(std::same_as<
    decltype(std::declval<ruvia::RedisSetOptions>().expiration),
    std::optional<ruvia::RedisSetExpiration>>);
static_assert(!std::default_initializable<ruvia::RedisSetExpiration>);
static_assert(std::same_as<
    decltype(ruvia::RedisScanOptions{}.count),
    std::optional<std::uint64_t>>);

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
    ruvia::detail::RedisRegistry registry(
        ioContext,
        std::pmr::get_default_resource(),
        definitions);

    bool defaultResolved = true;
    bool aliasResolved = true;
    try {
        (void)registry.get(std::pmr::get_default_resource());
    } catch (...) {
        defaultResolved = false;
    }
    try {
        (void)registry.get("cache", std::pmr::get_default_resource());
    } catch (...) {
        aliasResolved = false;
    }
    RUVIA_CHECK(defaultResolved);
    RUVIA_CHECK(aliasResolved);
}

RUVIA_TEST(redis_set_expiration_cannot_represent_conflicting_modes) {
    const auto expiring = ruvia::RedisSetExpiration::expiresAfter(
        std::chrono::milliseconds(1500));
    RUVIA_CHECK(expiring.duration() != nullptr);
    RUVIA_CHECK_EQ(
        expiring.duration()->count(),
        std::chrono::milliseconds::rep{1500});
    RUVIA_CHECK(!expiring.keepsExisting());

    const auto keep = ruvia::RedisSetExpiration::keepExisting();
    RUVIA_CHECK(keep.duration() == nullptr);
    RUVIA_CHECK(keep.keepsExisting());

    bool zeroRejected = false;
    try {
        (void)ruvia::RedisSetExpiration::expiresAfter(
            std::chrono::milliseconds(0));
    } catch (const std::invalid_argument&) {
        zeroRejected = true;
    }
    RUVIA_CHECK(zeroRejected);
}

RUVIA_TEST(redis_value_move_assignment_propagates_allocator_failure) {
    RejectingMemoryResource rejecting;
    const auto longValue = std::string_view(
        "redis value large enough to exceed any small-string buffer");

    auto destination = ruvia::detail::RedisTypesAccess::keyValue(
        {}, {}, &rejecting);
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
    auto destinationValue = ruvia::detail::RedisTypesAccess::nullValue(
        &rejecting);
    auto sourceValue = ruvia::detail::RedisTypesAccess::stringValue(
        longValue, std::pmr::get_default_resource());
    rejecting.rejectAllocations();
    allocationFailure = false;
    try {
        destinationValue = std::move(sourceValue);
    } catch (const std::bad_alloc&) {
        allocationFailure = true;
    }
    RUVIA_CHECK(allocationFailure);
}
