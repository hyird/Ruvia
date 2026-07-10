#include "test_harness.h"

#include <concepts>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/web/redis/RedisHandle.h"

namespace {

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

static_assert(HasRedisHandleSpanArgs<ruvia::RedisHandle>);
static_assert(!HasRedisHandleInitializerListArgs<ruvia::RedisHandle>);
static_assert(HasRedisPipelineSpanCommand<ruvia::RedisPipeline>);
static_assert(!HasRedisPipelineInitializerListCommand<ruvia::RedisPipeline>);
static_assert(HasRedisTransactionSpanCommand<ruvia::RedisTransaction>);
static_assert(!HasRedisTransactionInitializerListCommand<ruvia::RedisTransaction>);

}  // namespace

RUVIA_TEST(redis_api_surface_uses_span_args_without_initializer_list_overloads) {
    RUVIA_CHECK(true);
}
