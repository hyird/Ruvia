#pragma once

#include "ruvia/web/detail/redis/RedisRegistry.h"
#include <chrono>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ruvia::detail {

// Typed reply accessors that enforce the RedisValue kind and, on a mismatch,
// throw RedisError(kProtocolError) rather than the raw accessors' std::logic_error.
// A reply's type is chosen by the (untrusted) server, so a wrong type is a
// protocol condition the caller catches via RedisError, not a program bug.
[[nodiscard]] std::string_view redisValueString(const RedisValue& value);
[[nodiscard]] std::int64_t redisValueInteger(const RedisValue& value);
[[nodiscard]] bool redisValueIntegerBool(const RedisValue& value);
[[nodiscard]] std::span<const RedisValue> redisValueArray(const RedisValue& value);
void throwIfRedisError(const RedisValue& value);
void throwIfRedisTransactionReplyError(const RedisValue& value, std::size_t index);
[[nodiscard]] bool validateRedisPooledCommand(
    std::span<const std::string_view> args, bool allowBlocking);
[[nodiscard]] bool redisSetReturnsPrevious(RedisSetPreviousValuePolicy policy);
[[nodiscard]] bool redisXReadGroupUsesNoAck(RedisXReadGroupAcknowledgementPolicy policy);

[[nodiscard]] std::pmr::vector<std::pmr::string> ownRedisArgs(
    std::span<const std::string_view> args, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> ownRedisArgs(
    std::initializer_list<std::string_view> args, std::pmr::memory_resource* resource);

Task<RedisValue> executeOwnedRedisCommand(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);
Task<RedisValue> executeOwnedRedisCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args,
    OperationOptions options, std::pmr::memory_resource* resource);
Task<std::optional<std::pmr::string>> redisStringCommand(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);
Task<std::int64_t> redisIntegerCommand(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);
Task<std::pmr::vector<std::pmr::string>> redisStringArrayCommand(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);
Task<std::pmr::vector<bool>> redisBoolArrayCommand(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);
Task<std::pmr::vector<std::optional<std::pmr::string>>> redisOptionalStringArrayCommand(
    RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource);
Task<void> redisOkCommand(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource);
Task<std::pmr::string> redisStatusCommand(RedisCommandExecutor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::string redisSecondsString(
    std::chrono::seconds ttl, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string redisMillisecondsString(
    std::chrono::milliseconds ttl, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string redisCursorString(
    std::optional<RedisScanCursor> cursor, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::vector<std::pmr::string> redisCommandWithKeys(std::string_view command,
    std::span<const std::string_view> keys, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redisMsetArgs(
    std::span<const std::pair<std::string_view, std::string_view>> items,
    std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redisSetArgs(std::string_view key,
    std::string_view value, const RedisSetOptions& options, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redisHsetFieldsArgs(std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields,
    std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redisCommandWithKeyFields(std::string_view command,
    std::string_view key, std::span<const std::string_view> fields,
    std::pmr::memory_resource* resource);
void appendRedisScanOptions(std::pmr::vector<std::pmr::string>& args,
    const RedisScanOptions& options, std::pmr::memory_resource* resource);

[[nodiscard]] double parseRedisDouble(std::string_view value, std::string_view context);
[[nodiscard]] std::pmr::vector<RedisKeyValue> parseRedisKeyValueArray(
    const RedisValue& value, std::pmr::memory_resource* resource, std::string_view context);
[[nodiscard]] std::pmr::vector<RedisScoredValue> parseRedisScoredArray(
    const RedisValue& value, std::pmr::memory_resource* resource);
[[nodiscard]] RedisScanResult parseRedisScanResult(
    const RedisValue& value, std::pmr::memory_resource* resource);
[[nodiscard]] RedisHashScanResult parseRedisHashScanResult(
    const RedisValue& value, std::pmr::memory_resource* resource);
[[nodiscard]] RedisZScanResult parseRedisZScanResult(
    const RedisValue& value, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::vector<std::pmr::string> redisEvalArgs(std::string_view command,
    std::string_view script, std::span<const std::string_view> keys,
    std::span<const std::string_view> argv, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redisBlockingPopArgs(std::string_view command,
    std::span<const std::string_view> keys, RedisBlockWait wait,
    std::pmr::memory_resource* resource);
[[nodiscard]] std::optional<RedisKeyValue> parseRedisBlockingPopReply(
    const RedisValue& value, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::vector<std::pmr::string> redisXReadGroupArgs(std::string_view group,
    std::string_view consumer, std::span<const RedisStreamReadView> streams,
    const RedisXReadGroupOptions& options, std::pmr::memory_resource* resource);
[[nodiscard]] std::optional<RedisXReadGroupResult> parseRedisXReadGroupReply(
    const RedisValue& value, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
