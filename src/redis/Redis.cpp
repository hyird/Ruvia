#include "ruvia/redis/Redis.h"

#include "ruvia/http/Context.h"
#include "../AsioAwait.h"
#include "RedisInternal.h"

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>
#include <hiredis/hiredis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <system_error>

namespace ruvia {
namespace {

[[nodiscard]] std::pmr::memory_resource* resolveResource(std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

[[nodiscard]] std::string_view redisValueString(const RedisValue& value) {
    if (value.kind() == RedisValue::Kind::kError) {
        throw RedisError(RedisError::Code::kCommandError, value.string());
    }
    return value.string();
}

void throwIfError(const RedisValue& value) {
    if (value.kind() == RedisValue::Kind::kError) {
        throw RedisError(RedisError::Code::kCommandError, value.string());
    }
}

[[nodiscard]] bool asciiEqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto a = left[i] >= 'A' && left[i] <= 'Z' ? static_cast<char>(left[i] + ('a' - 'A')) : left[i];
        const auto b = right[i] >= 'A' && right[i] <= 'Z' ? static_cast<char>(right[i] + ('a' - 'A')) : right[i];
        if (a != b) {
            return false;
        }
    }
    return true;
}

void appendNumber(std::pmr::string& output, std::uint64_t value) {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format redis number");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

void appendNumber(std::pmr::string& output, std::int64_t value) {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format redis number");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

void appendRespCommand(std::pmr::string& output, std::span<const std::string_view> args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    if (args.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("redis command has too many arguments");
    }

    constexpr std::size_t stackArgLimit = 32;
    std::array<const char*, stackArgLimit> stackArgv{};
    std::array<std::size_t, stackArgLimit> stackArgvLen{};
    std::pmr::vector<const char*> heapArgv(output.get_allocator().resource());
    std::pmr::vector<std::size_t> heapArgvLen(output.get_allocator().resource());

    const bool useStackArgs = args.size() <= stackArgLimit;
    auto* argv = stackArgv.data();
    auto* argvLen = stackArgvLen.data();
    if (!useStackArgs) {
        heapArgv.reserve(args.size());
        heapArgvLen.reserve(args.size());
        argv = heapArgv.data();
        argvLen = heapArgvLen.data();
    }

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (useStackArgs) {
            stackArgv[i] = args[i].data();
            stackArgvLen[i] = args[i].size();
        } else {
            heapArgv.push_back(args[i].data());
            heapArgvLen.push_back(args[i].size());
        }
    }
    if (!useStackArgs) {
        argv = heapArgv.data();
        argvLen = heapArgvLen.data();
    }

    char* command = nullptr;
    const auto size = redisFormatCommandArgv(
        &command,
        static_cast<int>(args.size()),
        argv,
        argvLen);
    if (size < 0 || command == nullptr) {
        throw RedisError(RedisError::Code::kProtocolError, "failed to format redis command");
    }
    output.append(command, static_cast<std::size_t>(size));
    redisFreeCommand(command);
}

[[nodiscard]] RedisValue hiredisReplyToValue(
    const redisReply& reply,
    std::size_t depth,
    std::size_t maxDepth,
    std::pmr::memory_resource* resource) {
    if (maxDepth > 0 && depth > maxDepth) {
        throw RedisError(RedisError::Code::kProtocolError, "redis array nesting is too deep");
    }

    switch (reply.type) {
        case REDIS_REPLY_STATUS:
        case REDIS_REPLY_STRING:
#ifdef REDIS_REPLY_BIGNUM
        case REDIS_REPLY_BIGNUM:
#endif
#ifdef REDIS_REPLY_VERB
        case REDIS_REPLY_VERB:
#endif
            return RedisValue::stringValue(std::string_view(reply.str == nullptr ? "" : reply.str, reply.len), resource);
        case REDIS_REPLY_ERROR:
            return RedisValue::errorValue(std::string_view(reply.str == nullptr ? "" : reply.str, reply.len), resource);
        case REDIS_REPLY_INTEGER:
            return RedisValue::integerValue(static_cast<std::int64_t>(reply.integer), resource);
        case REDIS_REPLY_NIL:
            return RedisValue::nullValue(resource);
#ifdef REDIS_REPLY_DOUBLE
        case REDIS_REPLY_DOUBLE:
            return RedisValue::stringValue(std::string_view(reply.str == nullptr ? "" : reply.str, reply.len), resource);
#endif
#ifdef REDIS_REPLY_BOOL
        case REDIS_REPLY_BOOL:
            return RedisValue::integerValue(reply.integer == 0 ? 0 : 1, resource);
#endif
        case REDIS_REPLY_ARRAY:
#ifdef REDIS_REPLY_MAP
        case REDIS_REPLY_MAP:
#endif
#ifdef REDIS_REPLY_SET
        case REDIS_REPLY_SET:
#endif
#ifdef REDIS_REPLY_ATTR
        case REDIS_REPLY_ATTR:
#endif
#ifdef REDIS_REPLY_PUSH
        case REDIS_REPLY_PUSH:
#endif
        {
            if (maxDepth > 0 && depth >= maxDepth) {
                throw RedisError(RedisError::Code::kProtocolError, "redis array nesting is too deep");
            }
            std::pmr::vector<RedisValue> values(resource);
            values.reserve(reply.elements);
            for (std::size_t i = 0; i < reply.elements; ++i) {
                if (reply.element == nullptr || reply.element[i] == nullptr) {
                    throw RedisError(RedisError::Code::kProtocolError, "invalid redis array reply");
                }
                values.emplace_back(hiredisReplyToValue(*reply.element[i], depth + 1, maxDepth, resource));
            }
            return RedisValue::arrayValue(std::move(values), resource);
        }
        default:
            throw RedisError(RedisError::Code::kProtocolError, "unsupported redis reply type");
    }
}

[[nodiscard]] const char* hiredisReaderError(const redisReader& reader) noexcept {
    return reader.errstr[0] == '\0' ? "redis protocol error" : reader.errstr;
}

[[nodiscard]] std::pmr::vector<std::pmr::string> ownArgs(
    std::span<const std::string_view> args,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> owned(resource);
    owned.reserve(args.size());
    for (const auto arg : args) {
        owned.emplace_back(std::pmr::string(arg.data(), arg.size(), resource));
    }
    return owned;
}

[[nodiscard]] std::pmr::vector<std::pmr::string> ownArgs(
    std::initializer_list<std::string_view> args,
    std::pmr::memory_resource* resource) {
    return ownArgs(std::span<const std::string_view>(args.begin(), args.size()), resource);
}

[[nodiscard]] std::pmr::vector<std::string_view> viewArgs(
    const std::pmr::vector<std::pmr::string>& args,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::string_view> views(resource);
    views.reserve(args.size());
    for (const auto& arg : args) {
        views.emplace_back(arg.data(), arg.size());
    }
    return views;
}

Task<RedisValue> executeOwned(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto views = viewArgs(args, resource);
    co_return co_await pool.execute(std::span<const std::string_view>(views.data(), views.size()), resource);
}

Task<std::optional<std::pmr::string>> stringCommand(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwned(pool, std::move(args), resource);
    throwIfError(value);
    if (value.null()) {
        co_return std::nullopt;
    }
    const auto text = redisValueString(value);
    co_return std::pmr::string(text.data(), text.size(), resource);
}

Task<std::int64_t> integerCommand(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwned(pool, std::move(args), resource);
    throwIfError(value);
    co_return value.integer();
}

Task<std::pmr::vector<std::pmr::string>> stringArrayCommand(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwned(pool, std::move(args), resource);
    throwIfError(value);
    const auto items = value.array();
    std::pmr::vector<std::pmr::string> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throwIfError(item);
        if (!item.null()) {
            const auto text = redisValueString(item);
            output.emplace_back(std::pmr::string(text.data(), text.size(), resource));
        }
    }
    co_return output;
}

Task<std::pmr::vector<bool>> boolArrayCommand(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwned(pool, std::move(args), resource);
    throwIfError(value);
    const auto items = value.array();
    std::pmr::vector<bool> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throwIfError(item);
        output.emplace_back(item.integer() != 0);
    }
    co_return output;
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> optionalStringArrayCommand(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwned(pool, std::move(args), resource);
    throwIfError(value);
    const auto items = value.array();
    std::pmr::vector<std::optional<std::pmr::string>> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throwIfError(item);
        if (item.null()) {
            output.emplace_back(std::nullopt);
        } else {
            const auto text = redisValueString(item);
            output.emplace_back(std::pmr::string(text.data(), text.size(), resource));
        }
    }
    co_return output;
}

Task<void> okCommand(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwned(pool, std::move(args), resource);
    throwIfError(value);
    if (!asciiEqualsIgnoreCase(redisValueString(value), "OK")) {
        throw RedisError(RedisError::Code::kCommandError, "unexpected redis status reply");
    }
    co_return;
}

Task<std::pmr::string> statusCommand(
    detail::RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwned(pool, std::move(args), resource);
    throwIfError(value);
    const auto text = redisValueString(value);
    co_return std::pmr::string(text.data(), text.size(), resource);
}

[[nodiscard]] std::pmr::string secondsString(std::chrono::seconds ttl, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    appendNumber(output, static_cast<std::int64_t>(ttl.count()));
    return output;
}

[[nodiscard]] std::pmr::string millisecondsString(std::chrono::milliseconds ttl, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    appendNumber(output, static_cast<std::int64_t>(ttl.count()));
    return output;
}

[[nodiscard]] std::pmr::string intString(std::int64_t value, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    appendNumber(output, value);
    return output;
}

[[nodiscard]] std::pmr::string scoreString(double value, std::pmr::memory_resource* resource) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("redis sorted set score must be finite");
    }
    std::array<char, 64> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::invalid_argument("redis sorted set score is invalid");
    }
    return std::pmr::string(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()), resource);
}

[[nodiscard]] std::pmr::vector<std::pmr::string> commandWithKeys(
    std::string_view command,
    std::span<const std::string_view> keys,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(keys.size() + 1);
    args.emplace_back(std::pmr::string(command.data(), command.size(), resource));
    for (const auto key : keys) {
        args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
    }
    return args;
}

[[nodiscard]] std::pmr::vector<std::pmr::string> msetArgs(
    std::span<const std::pair<std::string_view, std::string_view>> items,
    std::pmr::memory_resource* resource) {
    if (items.empty()) {
        throw std::invalid_argument("redis mset requires at least one item");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(items.size() * 2 + 1);
    args.emplace_back(std::pmr::string("MSET", 4, resource));
    for (const auto& [key, value] : items) {
        args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
        args.emplace_back(std::pmr::string(value.data(), value.size(), resource));
    }
    return args;
}

[[nodiscard]] std::pmr::vector<std::pmr::string> hsetFieldsArgs(
    std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields,
    std::pmr::memory_resource* resource) {
    if (fields.empty()) {
        throw std::invalid_argument("redis hset requires at least one field");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(fields.size() * 2 + 2);
    args.emplace_back(std::pmr::string("HSET", 4, resource));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
    for (const auto& [field, value] : fields) {
        args.emplace_back(std::pmr::string(field.data(), field.size(), resource));
        args.emplace_back(std::pmr::string(value.data(), value.size(), resource));
    }
    return args;
}

[[nodiscard]] std::pmr::vector<std::pmr::string> commandWithKeyFields(
    std::string_view command,
    std::string_view key,
    std::span<const std::string_view> fields,
    std::pmr::memory_resource* resource) {
    if (fields.empty()) {
        throw std::invalid_argument("redis command requires at least one field");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(fields.size() + 2);
    args.emplace_back(std::pmr::string(command.data(), command.size(), resource));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
    for (const auto field : fields) {
        args.emplace_back(std::pmr::string(field.data(), field.size(), resource));
    }
    return args;
}

void appendScanOptions(std::pmr::vector<std::pmr::string>& args, const RedisScanOptions& options, std::pmr::memory_resource* resource) {
    if (!options.match.empty()) {
        args.emplace_back(std::pmr::string("MATCH", 5, resource));
        args.emplace_back(std::pmr::string(options.match.data(), options.match.size(), resource));
    }
    if (options.count != 0) {
        args.emplace_back(std::pmr::string("COUNT", 5, resource));
        std::pmr::string count(resource);
        appendNumber(count, options.count);
        args.emplace_back(std::move(count));
    }
}

[[nodiscard]] std::pmr::string cursorString(std::uint64_t cursor, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    appendNumber(output, cursor);
    return output;
}

[[nodiscard]] double parseDouble(std::string_view value, std::string_view context) {
    double output = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), output);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        throw RedisError(RedisError::Code::kProtocolError, context);
    }
    return output;
}

[[nodiscard]] std::pmr::vector<RedisKeyValue> parseKeyValueArray(
    const RedisValue& value,
    std::pmr::memory_resource* resource,
    std::string_view context) {
    throwIfError(value);
    const auto values = value.array();
    if (values.size() % 2 != 0) {
        throw RedisError(RedisError::Code::kProtocolError, context);
    }
    std::pmr::vector<RedisKeyValue> result(resource);
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto key = redisValueString(values[i]);
        const auto fieldValue = redisValueString(values[i + 1]);
        result.push_back(RedisKeyValue{
            std::pmr::string(key.data(), key.size(), resource),
            std::pmr::string(fieldValue.data(), fieldValue.size(), resource)});
    }
    return result;
}

[[nodiscard]] std::pmr::vector<RedisScoredValue> parseScoredArray(const RedisValue& value, std::pmr::memory_resource* resource) {
    throwIfError(value);
    const auto values = value.array();
    if (values.size() % 2 != 0) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis scored array reply");
    }
    std::pmr::vector<RedisScoredValue> result(resource);
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto member = redisValueString(values[i]);
        const auto scoreText = redisValueString(values[i + 1]);
        result.push_back(RedisScoredValue{
            std::pmr::string(member.data(), member.size(), resource),
            parseDouble(scoreText, "invalid redis score")});
    }
    return result;
}

[[nodiscard]] std::uint64_t parseCursor(std::string_view value) {
    std::uint64_t cursor = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), cursor);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        throw RedisError(RedisError::Code::kProtocolError, "invalid redis scan cursor");
    }
    return cursor;
}

[[nodiscard]] RedisScanResult parseScanResult(const RedisValue& value, std::pmr::memory_resource* resource) {
    throwIfError(value);
    const auto root = value.array();
    if (root.size() != 2) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis scan reply");
    }
    const auto cursor = parseCursor(redisValueString(root[0]));
    const auto values = root[1].array();
    RedisScanResult result{.cursor = cursor, .values = std::pmr::vector<std::pmr::string>(resource)};
    result.values.reserve(values.size());
    for (const auto& item : values) {
        const auto text = redisValueString(item);
        result.values.emplace_back(std::pmr::string(text.data(), text.size(), resource));
    }
    return result;
}

[[nodiscard]] RedisHashScanResult parseHashScanResult(const RedisValue& value, std::pmr::memory_resource* resource) {
    throwIfError(value);
    const auto root = value.array();
    if (root.size() != 2) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis hscan reply");
    }
    const auto cursor = parseCursor(redisValueString(root[0]));
    const auto values = root[1].array();
    if (values.size() % 2 != 0) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis hscan entry count");
    }
    RedisHashScanResult result{.cursor = cursor, .entries = std::pmr::vector<RedisKeyValue>(resource)};
    result.entries.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto key = redisValueString(values[i]);
        const auto fieldValue = redisValueString(values[i + 1]);
        result.entries.push_back(RedisKeyValue{
            std::pmr::string(key.data(), key.size(), resource),
            std::pmr::string(fieldValue.data(), fieldValue.size(), resource)});
    }
    return result;
}

[[nodiscard]] RedisZScanResult parseZScanResult(const RedisValue& value, std::pmr::memory_resource* resource) {
    throwIfError(value);
    const auto root = value.array();
    if (root.size() != 2) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis zscan reply");
    }
    const auto cursor = parseCursor(redisValueString(root[0]));
    const auto values = root[1].array();
    if (values.size() % 2 != 0) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis zscan entry count");
    }
    RedisZScanResult result{.cursor = cursor, .entries = std::pmr::vector<RedisScoredValue>(resource)};
    result.entries.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto member = redisValueString(values[i]);
        const auto scoreText = redisValueString(values[i + 1]);
        result.entries.push_back(RedisScoredValue{
            std::pmr::string(member.data(), member.size(), resource),
            parseDouble(scoreText, "invalid redis zscan score")});
    }
    return result;
}

[[nodiscard]] std::pmr::vector<std::pmr::string> evalArgs(
    std::string_view command,
    std::string_view script,
    std::span<const std::string_view> keys,
    std::span<const std::string_view> argv,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(3 + keys.size() + argv.size());
    args.emplace_back(std::pmr::string(command.data(), command.size(), resource));
    args.emplace_back(std::pmr::string(script.data(), script.size(), resource));
    args.emplace_back(intString(static_cast<std::int64_t>(keys.size()), resource));
    for (const auto key : keys) {
        args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
    }
    for (const auto arg : argv) {
        args.emplace_back(std::pmr::string(arg.data(), arg.size(), resource));
    }
    return args;
}

[[nodiscard]] std::pmr::vector<std::pmr::string> blockingPopArgs(
    std::string_view command,
    std::span<const std::string_view> keys,
    std::chrono::seconds timeout,
    std::pmr::memory_resource* resource) {
    if (keys.empty()) {
        throw std::invalid_argument("redis blocking pop requires at least one key");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(keys.size() + 2);
    args.emplace_back(std::pmr::string(command.data(), command.size(), resource));
    for (const auto key : keys) {
        args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
    }
    args.emplace_back(secondsString(timeout, resource));
    return args;
}

[[nodiscard]] std::optional<RedisKeyValue> parseBlockingPopReply(const RedisValue& value, std::pmr::memory_resource* resource) {
    throwIfError(value);
    if (value.null()) {
        return std::nullopt;
    }
    const auto items = value.array();
    if (items.size() != 2) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis blocking pop reply");
    }
    const auto key = redisValueString(items[0]);
    const auto item = redisValueString(items[1]);
    return RedisKeyValue{
        std::pmr::string(key.data(), key.size(), resource),
        std::pmr::string(item.data(), item.size(), resource)};
}

}  // namespace

RedisError::RedisError(Code code, std::string_view message)
    : code_(code),
      message_(message, std::pmr::get_default_resource()) {}

const char* RedisError::what() const noexcept {
    return message_.c_str();
}

RedisError::Code RedisError::code() const noexcept {
    return code_;
}

std::string_view RedisError::message() const noexcept {
    return std::string_view(message_.data(), message_.size());
}

RedisValue::RedisValue(std::pmr::memory_resource* resource)
    : string_(resolveResource(resource)),
      array_(resolveResource(resource)) {}

RedisValue::Kind RedisValue::kind() const noexcept {
    return kind_;
}

bool RedisValue::null() const noexcept {
    return kind_ == Kind::kNull;
}

std::string_view RedisValue::string() const {
    if (kind_ != Kind::kString && kind_ != Kind::kError) {
        throw std::logic_error("redis value is not a string");
    }
    return std::string_view(string_.data(), string_.size());
}

std::int64_t RedisValue::integer() const {
    if (kind_ != Kind::kInteger) {
        throw std::logic_error("redis value is not an integer");
    }
    return integer_;
}

std::span<const RedisValue> RedisValue::array() const {
    if (kind_ != Kind::kArray) {
        throw std::logic_error("redis value is not an array");
    }
    return std::span<const RedisValue>(array_.data(), array_.size());
}

RedisValue RedisValue::nullValue(std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kNull;
    return value;
}

RedisValue RedisValue::stringValue(std::string_view input, std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kString;
    value.string_.assign(input.data(), input.size());
    return value;
}

RedisValue RedisValue::errorValue(std::string_view input, std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kError;
    value.string_.assign(input.data(), input.size());
    return value;
}

RedisValue RedisValue::integerValue(std::int64_t input, std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kInteger;
    value.integer_ = input;
    return value;
}

RedisValue RedisValue::arrayValue(std::pmr::vector<RedisValue> values, std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kArray;
    value.array_ = std::move(values);
    return value;
}

RedisPipeline::RedisPipeline(
    detail::RedisPool& pool,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) noexcept
    : pool_(&pool),
      resource_(resolveResource(resource)),
      requestMemory_(requestMemory),
      commands_(resource_) {}

RedisPipeline& RedisPipeline::command(std::initializer_list<std::string_view> args) {
    return command(std::span<const std::string_view>(args.begin(), args.size()));
}

RedisPipeline& RedisPipeline::command(std::span<const std::string_view> args) {
    Command command{std::pmr::vector<std::pmr::string>(resource_)};
    command.args.reserve(args.size());
    for (const auto arg : args) {
        command.args.emplace_back(std::pmr::string(arg.data(), arg.size(), resource_));
    }
    commands_.emplace_back(std::move(command));
    return *this;
}

RedisPipeline& RedisPipeline::get(std::string_view key) {
    return command({"GET", key});
}

RedisPipeline& RedisPipeline::set(std::string_view key, std::string_view value) {
    return command({"SET", key, value});
}

RedisPipeline& RedisPipeline::getDel(std::string_view key) {
    return command({"GETDEL", key});
}

RedisPipeline& RedisPipeline::getSet(std::string_view key, std::string_view value) {
    return command({"GETSET", key, value});
}

RedisPipeline& RedisPipeline::append(std::string_view key, std::string_view value) {
    return command({"APPEND", key, value});
}

RedisPipeline& RedisPipeline::strlen(std::string_view key) {
    return command({"STRLEN", key});
}

RedisPipeline& RedisPipeline::del(std::string_view key) {
    return command({"DEL", key});
}

RedisPipeline& RedisPipeline::unlink(std::string_view key) {
    return command({"UNLINK", key});
}

RedisPipeline& RedisPipeline::exists(std::string_view key) {
    return command({"EXISTS", key});
}

RedisPipeline& RedisPipeline::touch(std::string_view key) {
    return command({"TOUCH", key});
}

RedisPipeline& RedisPipeline::type(std::string_view key) {
    return command({"TYPE", key});
}

RedisPipeline& RedisPipeline::rename(std::string_view key, std::string_view newKey) {
    return command({"RENAME", key, newKey});
}

RedisPipeline& RedisPipeline::renameNx(std::string_view key, std::string_view newKey) {
    return command({"RENAMENX", key, newKey});
}

RedisPipeline& RedisPipeline::incr(std::string_view key) {
    return command({"INCR", key});
}

RedisPipeline& RedisPipeline::incrBy(std::string_view key, std::int64_t value) {
    auto amount = intString(value, resource_);
    return command({"INCRBY", key, std::string_view(amount)});
}

RedisPipeline& RedisPipeline::decr(std::string_view key) {
    return command({"DECR", key});
}

RedisPipeline& RedisPipeline::decrBy(std::string_view key, std::int64_t value) {
    auto amount = intString(value, resource_);
    return command({"DECRBY", key, std::string_view(amount)});
}

RedisPipeline& RedisPipeline::hget(std::string_view key, std::string_view field) {
    return command({"HGET", key, field});
}

RedisPipeline& RedisPipeline::hset(std::string_view key, std::string_view field, std::string_view value) {
    return command({"HSET", key, field, value});
}

RedisPipeline& RedisPipeline::hdel(std::string_view key, std::string_view field) {
    return command({"HDEL", key, field});
}

RedisPipeline& RedisPipeline::hexists(std::string_view key, std::string_view field) {
    return command({"HEXISTS", key, field});
}

RedisPipeline& RedisPipeline::hlen(std::string_view key) {
    return command({"HLEN", key});
}

RedisPipeline& RedisPipeline::hgetAll(std::string_view key) {
    return command({"HGETALL", key});
}

RedisPipeline& RedisPipeline::lpush(std::string_view key, std::string_view value) {
    return command({"LPUSH", key, value});
}

RedisPipeline& RedisPipeline::rpush(std::string_view key, std::string_view value) {
    return command({"RPUSH", key, value});
}

RedisPipeline& RedisPipeline::lpop(std::string_view key) {
    return command({"LPOP", key});
}

RedisPipeline& RedisPipeline::rpop(std::string_view key) {
    return command({"RPOP", key});
}

RedisPipeline& RedisPipeline::llen(std::string_view key) {
    return command({"LLEN", key});
}

RedisPipeline& RedisPipeline::lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    auto startValue = intString(start, resource_);
    auto stopValue = intString(stop, resource_);
    return command({"LRANGE", key, std::string_view(startValue), std::string_view(stopValue)});
}

RedisPipeline& RedisPipeline::sadd(std::string_view key, std::string_view member) {
    return command({"SADD", key, member});
}

RedisPipeline& RedisPipeline::srem(std::string_view key, std::string_view member) {
    return command({"SREM", key, member});
}

RedisPipeline& RedisPipeline::smembers(std::string_view key) {
    return command({"SMEMBERS", key});
}

RedisPipeline& RedisPipeline::scard(std::string_view key) {
    return command({"SCARD", key});
}

RedisPipeline& RedisPipeline::zadd(std::string_view key, double score, std::string_view member) {
    auto scoreValue = scoreString(score, resource_);
    return command({"ZADD", key, std::string_view(scoreValue), member});
}

RedisPipeline& RedisPipeline::zrem(std::string_view key, std::string_view member) {
    return command({"ZREM", key, member});
}

RedisPipeline& RedisPipeline::zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    auto startValue = intString(start, resource_);
    auto stopValue = intString(stop, resource_);
    return command({"ZRANGE", key, std::string_view(startValue), std::string_view(stopValue)});
}

RedisPipeline& RedisPipeline::zscore(std::string_view key, std::string_view member) {
    return command({"ZSCORE", key, member});
}

RedisPipeline& RedisPipeline::zcard(std::string_view key) {
    return command({"ZCARD", key});
}

Task<std::pmr::vector<RedisValue>> RedisPipeline::exec() {
    if (pool_ == nullptr) {
        throw std::logic_error("redis pipeline is empty");
    }
    co_return co_await pool_->executePipeline(std::span<const Command>(commands_.data(), commands_.size()), resource_);
}

RedisTransaction::RedisTransaction(RedisPipeline pipeline) noexcept
    : pipeline_(std::move(pipeline)),
      watches_(pipeline_.resource_) {}

RedisTransaction& RedisTransaction::command(std::initializer_list<std::string_view> args) {
    pipeline_.command(args);
    return *this;
}

RedisTransaction& RedisTransaction::command(std::span<const std::string_view> args) {
    pipeline_.command(args);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::watch(std::string_view key) {
    return watch(std::span<const std::string_view>(&key, 1));
}

RedisTransaction& RedisTransaction::watch(std::span<const std::string_view> keys) {
    if (keys.empty()) {
        return *this;
    }
    RedisPipeline::Command command{std::pmr::vector<std::pmr::string>(pipeline_.resource_)};
    command.args.reserve(keys.size() + 1);
    command.args.emplace_back(std::pmr::string("WATCH", 5, pipeline_.resource_));
    for (const auto key : keys) {
        command.args.emplace_back(std::pmr::string(key.data(), key.size(), pipeline_.resource_));
    }
    watches_.emplace_back(std::move(command));
    return *this;
}

RedisTransaction& RedisTransaction::unwatch() {
    RedisPipeline::Command command{std::pmr::vector<std::pmr::string>(pipeline_.resource_)};
    command.args.emplace_back(std::pmr::string("UNWATCH", 7, pipeline_.resource_));
    watches_.emplace_back(std::move(command));
    return *this;
}

RedisTransaction& RedisTransaction::discard() noexcept {
    watches_.clear();
    pipeline_.commands_.clear();
    discarded_ = true;
    return *this;
}

RedisTransaction& RedisTransaction::get(std::string_view key) {
    pipeline_.get(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::set(std::string_view key, std::string_view value) {
    pipeline_.set(key, value);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::getDel(std::string_view key) {
    pipeline_.getDel(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::getSet(std::string_view key, std::string_view value) {
    pipeline_.getSet(key, value);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::append(std::string_view key, std::string_view value) {
    pipeline_.append(key, value);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::strlen(std::string_view key) {
    pipeline_.strlen(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::del(std::string_view key) {
    pipeline_.del(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::unlink(std::string_view key) {
    pipeline_.unlink(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::exists(std::string_view key) {
    pipeline_.exists(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::touch(std::string_view key) {
    pipeline_.touch(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::type(std::string_view key) {
    pipeline_.type(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::rename(std::string_view key, std::string_view newKey) {
    pipeline_.rename(key, newKey);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::renameNx(std::string_view key, std::string_view newKey) {
    pipeline_.renameNx(key, newKey);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::incr(std::string_view key) {
    pipeline_.incr(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::incrBy(std::string_view key, std::int64_t value) {
    pipeline_.incrBy(key, value);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::decr(std::string_view key) {
    pipeline_.decr(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::decrBy(std::string_view key, std::int64_t value) {
    pipeline_.decrBy(key, value);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::hget(std::string_view key, std::string_view field) {
    pipeline_.hget(key, field);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::hset(std::string_view key, std::string_view field, std::string_view value) {
    pipeline_.hset(key, field, value);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::hdel(std::string_view key, std::string_view field) {
    pipeline_.hdel(key, field);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::hexists(std::string_view key, std::string_view field) {
    pipeline_.hexists(key, field);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::hlen(std::string_view key) {
    pipeline_.hlen(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::hgetAll(std::string_view key) {
    pipeline_.hgetAll(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::lpush(std::string_view key, std::string_view value) {
    pipeline_.lpush(key, value);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::rpush(std::string_view key, std::string_view value) {
    pipeline_.rpush(key, value);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::lpop(std::string_view key) {
    pipeline_.lpop(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::rpop(std::string_view key) {
    pipeline_.rpop(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::llen(std::string_view key) {
    pipeline_.llen(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    pipeline_.lrange(key, start, stop);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::sadd(std::string_view key, std::string_view member) {
    pipeline_.sadd(key, member);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::srem(std::string_view key, std::string_view member) {
    pipeline_.srem(key, member);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::smembers(std::string_view key) {
    pipeline_.smembers(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::scard(std::string_view key) {
    pipeline_.scard(key);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::zadd(std::string_view key, double score, std::string_view member) {
    pipeline_.zadd(key, score, member);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::zrem(std::string_view key, std::string_view member) {
    pipeline_.zrem(key, member);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    pipeline_.zrange(key, start, stop);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::zscore(std::string_view key, std::string_view member) {
    pipeline_.zscore(key, member);
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::zcard(std::string_view key) {
    pipeline_.zcard(key);
    discarded_ = false;
    return *this;
}

Task<std::pmr::vector<RedisValue>> RedisTransaction::exec() {
    if (discarded_) {
        co_return std::pmr::vector<RedisValue>(pipeline_.resource_);
    }
    auto& commands = pipeline_.commands_;
    const auto resource = pipeline_.resource_;
    RedisPipeline framed(*pipeline_.pool_, resource, pipeline_.requestMemory_);
    for (const auto& command : watches_) {
        std::pmr::vector<std::string_view> views(resource);
        views.reserve(command.args.size());
        for (const auto& arg : command.args) {
            views.emplace_back(arg.data(), arg.size());
        }
        framed.command(std::span<const std::string_view>(views.data(), views.size()));
    }
    framed.command({"MULTI"});
    for (const auto& command : commands) {
        std::pmr::vector<std::string_view> views(resource);
        views.reserve(command.args.size());
        for (const auto& arg : command.args) {
            views.emplace_back(arg.data(), arg.size());
        }
        framed.command(std::span<const std::string_view>(views.data(), views.size()));
    }
    framed.command({"EXEC"});

    auto replies = co_await framed.exec();
    if (replies.empty() ||
        replies.back().kind() == RedisValue::Kind::kError) {
        throw RedisError(RedisError::Code::kCommandError, "redis transaction failed");
    }
    for (std::size_t i = 0; i + 1 < replies.size(); ++i) {
        if (replies[i].kind() == RedisValue::Kind::kError) {
            throw RedisError(RedisError::Code::kCommandError, "redis transaction failed");
        }
    }
    auto execReply = std::move(replies.back());
    if (execReply.null()) {
        throw RedisError(RedisError::Code::kTransactionAborted, "redis transaction aborted");
    }
    if (execReply.kind() != RedisValue::Kind::kArray) {
        throw RedisError(RedisError::Code::kCommandError, "unexpected redis transaction reply");
    }

    std::pmr::vector<RedisValue> result(resource);
    result.reserve(execReply.array().size());
    for (const auto& value : execReply.array()) {
        result.emplace_back(value);
    }
    co_return result;
}

RedisHandle::RedisHandle(
    detail::RedisPool& pool,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) noexcept
    : pool_(pool),
      resource_(resolveResource(resource)),
      requestMemory_(requestMemory) {}

Task<RedisValue> RedisHandle::command(std::initializer_list<std::string_view> args) const {
    return executeOwned(pool_, ownArgs(args, resource_), resource_);
}

Task<RedisValue> RedisHandle::command(std::span<const std::string_view> args) const {
    return executeOwned(pool_, ownArgs(args, resource_), resource_);
}

Task<void> RedisHandle::ping() const {
    auto reply = co_await statusCommand(pool_, ownArgs({"PING"}, resource_), resource_);
    if (!asciiEqualsIgnoreCase(reply, "PONG")) {
        throw RedisError(RedisError::Code::kCommandError, "unexpected redis ping reply");
    }
}

Task<std::pmr::string> RedisHandle::ping(std::string_view message) const {
    return statusCommand(pool_, ownArgs({"PING", message}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::get(std::string_view key) const {
    return stringCommand(pool_, ownArgs({"GET", key}, resource_), resource_);
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::mget(std::span<const std::string_view> keys) const {
    return optionalStringArrayCommand(pool_, commandWithKeys("MGET", keys, resource_), resource_);
}

Task<void> RedisHandle::set(std::string_view key, std::string_view value) const {
    return okCommand(pool_, ownArgs({"SET", key, value}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::set(std::string_view key, std::string_view value, RedisSetOptions options) const {
    if (options.nx && options.xx) {
        throw std::invalid_argument("redis set options cannot combine NX and XX");
    }
    if (options.ttl.count() > 0 && options.keepTtl) {
        throw std::invalid_argument("redis set options cannot combine TTL and KEEPTTL");
    }
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(8);
    args.emplace_back(std::pmr::string("SET", 3, resource_));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource_));
    args.emplace_back(std::pmr::string(value.data(), value.size(), resource_));
    std::pmr::string ttlValue(resource_);
    if (options.ttl.count() > 0) {
        ttlValue = millisecondsString(options.ttl, resource_);
        args.emplace_back(std::pmr::string("PX", 2, resource_));
        args.emplace_back(std::pmr::string(ttlValue.data(), ttlValue.size(), resource_));
    }
    if (options.nx) {
        args.emplace_back(std::pmr::string("NX", 2, resource_));
    }
    if (options.xx) {
        args.emplace_back(std::pmr::string("XX", 2, resource_));
    }
    if (options.get) {
        args.emplace_back(std::pmr::string("GET", 3, resource_));
    }
    if (options.keepTtl) {
        args.emplace_back(std::pmr::string("KEEPTTL", 7, resource_));
    }

    auto reply = co_await executeOwned(pool_, std::move(args), resource_);
    throwIfError(reply);
    if (reply.null()) {
        co_return std::nullopt;
    }
    const auto text = redisValueString(reply);
    if (!options.get) {
        if (!asciiEqualsIgnoreCase(text, "OK")) {
            throw RedisError(RedisError::Code::kCommandError, "unexpected redis set reply");
        }
        co_return std::nullopt;
    }
    co_return std::pmr::string(text.data(), text.size(), resource_);
}

Task<void> RedisHandle::mset(std::span<const std::pair<std::string_view, std::string_view>> items) const {
    return okCommand(pool_, msetArgs(items, resource_), resource_);
}

Task<void> RedisHandle::setEx(std::string_view key, std::chrono::seconds ttl, std::string_view value) const {
    auto ttlValue = secondsString(ttl, resource_);
    return okCommand(pool_, ownArgs({"SETEX", key, std::string_view(ttlValue), value}, resource_), resource_);
}

Task<bool> RedisHandle::setNx(std::string_view key, std::string_view value) const {
    auto args = ownArgs({"SET", key, value, "NX"}, resource_);
    auto reply = co_await executeOwned(pool_, std::move(args), resource_);
    throwIfError(reply);
    if (reply.null()) {
        co_return false;
    }
    co_return asciiEqualsIgnoreCase(redisValueString(reply), "OK");
}

Task<std::optional<std::pmr::string>> RedisHandle::getDel(std::string_view key) const {
    return stringCommand(pool_, ownArgs({"GETDEL", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::getSet(std::string_view key, std::string_view value) const {
    return stringCommand(pool_, ownArgs({"GETSET", key, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::append(std::string_view key, std::string_view value) const {
    return integerCommand(pool_, ownArgs({"APPEND", key, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::strlen(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"STRLEN", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::incrBy(std::string_view key, std::int64_t value) const {
    auto amount = intString(value, resource_);
    return integerCommand(pool_, ownArgs({"INCRBY", key, std::string_view(amount)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::decr(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"DECR", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::decrBy(std::string_view key, std::int64_t value) const {
    auto amount = intString(value, resource_);
    return integerCommand(pool_, ownArgs({"DECRBY", key, std::string_view(amount)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::del(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"DEL", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::unlink(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"UNLINK", key}, resource_), resource_);
}

Task<bool> RedisHandle::exists(std::string_view key) const {
    auto args = ownArgs({"EXISTS", key}, resource_);
    co_return (co_await integerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<bool> RedisHandle::touch(std::string_view key) const {
    auto args = ownArgs({"TOUCH", key}, resource_);
    co_return (co_await integerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<std::pmr::string> RedisHandle::type(std::string_view key) const {
    return statusCommand(pool_, ownArgs({"TYPE", key}, resource_), resource_);
}

Task<void> RedisHandle::rename(std::string_view key, std::string_view newKey) const {
    return okCommand(pool_, ownArgs({"RENAME", key, newKey}, resource_), resource_);
}

Task<bool> RedisHandle::renameNx(std::string_view key, std::string_view newKey) const {
    auto args = ownArgs({"RENAMENX", key, newKey}, resource_);
    co_return (co_await integerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<bool> RedisHandle::expire(std::string_view key, std::chrono::seconds ttl) const {
    auto ttlValue = secondsString(ttl, resource_);
    auto args = ownArgs({"EXPIRE", key, std::string_view(ttlValue)}, resource_);
    co_return (co_await integerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<bool> RedisHandle::expireAt(std::string_view key, std::chrono::seconds unixTime) const {
    auto value = secondsString(unixTime, resource_);
    auto args = ownArgs({"EXPIREAT", key, std::string_view(value)}, resource_);
    co_return (co_await integerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<bool> RedisHandle::persist(std::string_view key) const {
    auto args = ownArgs({"PERSIST", key}, resource_);
    co_return (co_await integerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<std::int64_t> RedisHandle::ttl(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"TTL", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::pttl(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"PTTL", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::incr(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"INCR", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::hget(std::string_view key, std::string_view field) const {
    return stringCommand(pool_, ownArgs({"HGET", key, field}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hset(std::string_view key, std::string_view field, std::string_view value) const {
    return integerCommand(pool_, ownArgs({"HSET", key, field, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hset(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields) const {
    return integerCommand(pool_, hsetFieldsArgs(key, fields, resource_), resource_);
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::hmget(std::string_view key, std::span<const std::string_view> fields) const {
    return optionalStringArrayCommand(pool_, commandWithKeyFields("HMGET", key, fields, resource_), resource_);
}

Task<std::pmr::vector<RedisKeyValue>> RedisHandle::hgetAll(std::string_view key) const {
    auto value = co_await executeOwned(pool_, ownArgs({"HGETALL", key}, resource_), resource_);
    co_return parseKeyValueArray(value, resource_, "unexpected redis hgetall reply");
}

Task<std::int64_t> RedisHandle::hdel(std::string_view key, std::string_view field) const {
    return integerCommand(pool_, ownArgs({"HDEL", key, field}, resource_), resource_);
}

Task<bool> RedisHandle::hexists(std::string_view key, std::string_view field) const {
    auto args = ownArgs({"HEXISTS", key, field}, resource_);
    co_return (co_await integerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<std::int64_t> RedisHandle::hlen(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"HLEN", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::hkeys(std::string_view key) const {
    return stringArrayCommand(pool_, ownArgs({"HKEYS", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::hvals(std::string_view key) const {
    return stringArrayCommand(pool_, ownArgs({"HVALS", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hincrBy(std::string_view key, std::string_view field, std::int64_t value) const {
    auto amount = intString(value, resource_);
    return integerCommand(pool_, ownArgs({"HINCRBY", key, field, std::string_view(amount)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::lpush(std::string_view key, std::string_view value) const {
    return integerCommand(pool_, ownArgs({"LPUSH", key, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::rpush(std::string_view key, std::string_view value) const {
    return integerCommand(pool_, ownArgs({"RPUSH", key, value}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::lpop(std::string_view key) const {
    return stringCommand(pool_, ownArgs({"LPOP", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::rpop(std::string_view key) const {
    return stringCommand(pool_, ownArgs({"RPOP", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::llen(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"LLEN", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::lrange(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = intString(start, resource_);
    auto stopValue = intString(stop, resource_);
    return stringArrayCommand(pool_, ownArgs({"LRANGE", key, std::string_view(startValue), std::string_view(stopValue)}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::lindex(std::string_view key, std::int64_t index) const {
    auto indexValue = intString(index, resource_);
    return stringCommand(pool_, ownArgs({"LINDEX", key, std::string_view(indexValue)}, resource_), resource_);
}

Task<void> RedisHandle::lset(std::string_view key, std::int64_t index, std::string_view value) const {
    auto indexValue = intString(index, resource_);
    return okCommand(pool_, ownArgs({"LSET", key, std::string_view(indexValue), value}, resource_), resource_);
}

Task<void> RedisHandle::ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = intString(start, resource_);
    auto stopValue = intString(stop, resource_);
    return okCommand(pool_, ownArgs({"LTRIM", key, std::string_view(startValue), std::string_view(stopValue)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::lrem(std::string_view key, std::int64_t count, std::string_view value) const {
    auto countValue = intString(count, resource_);
    return integerCommand(pool_, ownArgs({"LREM", key, std::string_view(countValue), value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::sadd(std::string_view key, std::string_view member) const {
    return integerCommand(pool_, ownArgs({"SADD", key, member}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::srem(std::string_view key, std::string_view member) const {
    return integerCommand(pool_, ownArgs({"SREM", key, member}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::smembers(std::string_view key) const {
    return stringArrayCommand(pool_, ownArgs({"SMEMBERS", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::scard(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"SCARD", key}, resource_), resource_);
}

Task<bool> RedisHandle::sismember(std::string_view key, std::string_view member) const {
    auto args = ownArgs({"SISMEMBER", key, member}, resource_);
    co_return (co_await integerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<std::optional<std::pmr::string>> RedisHandle::spop(std::string_view key) const {
    return stringCommand(pool_, ownArgs({"SPOP", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::srandMember(std::string_view key) const {
    return stringCommand(pool_, ownArgs({"SRANDMEMBER", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::sinter(std::span<const std::string_view> keys) const {
    return stringArrayCommand(pool_, commandWithKeys("SINTER", keys, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::sunion(std::span<const std::string_view> keys) const {
    return stringArrayCommand(pool_, commandWithKeys("SUNION", keys, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::sdiff(std::span<const std::string_view> keys) const {
    return stringArrayCommand(pool_, commandWithKeys("SDIFF", keys, resource_), resource_);
}

Task<std::int64_t> RedisHandle::zadd(std::string_view key, double score, std::string_view member) const {
    auto scoreValue = scoreString(score, resource_);
    return integerCommand(pool_, ownArgs({"ZADD", key, std::string_view(scoreValue), member}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::zrem(std::string_view key, std::string_view member) const {
    return integerCommand(pool_, ownArgs({"ZREM", key, member}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::zrange(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = intString(start, resource_);
    auto stopValue = intString(stop, resource_);
    return stringArrayCommand(
        pool_,
        ownArgs({"ZRANGE", key, std::string_view(startValue), std::string_view(stopValue)}, resource_),
        resource_);
}

Task<std::pmr::vector<RedisScoredValue>> RedisHandle::zrangeWithScores(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = intString(start, resource_);
    auto stopValue = intString(stop, resource_);
    auto value = co_await executeOwned(
        pool_,
        ownArgs({"ZRANGE", key, std::string_view(startValue), std::string_view(stopValue), "WITHSCORES"}, resource_),
        resource_);
    co_return parseScoredArray(value, resource_);
}

Task<std::optional<double>> RedisHandle::zscore(std::string_view key, std::string_view member) const {
    auto reply = co_await stringCommand(pool_, ownArgs({"ZSCORE", key, member}, resource_), resource_);
    if (!reply) {
        co_return std::nullopt;
    }
    co_return parseDouble(*reply, "invalid redis zscore reply");
}

Task<std::int64_t> RedisHandle::zcard(std::string_view key) const {
    return integerCommand(pool_, ownArgs({"ZCARD", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::zcount(std::string_view key, double min, double max) const {
    auto minValue = scoreString(min, resource_);
    auto maxValue = scoreString(max, resource_);
    return integerCommand(pool_, ownArgs({"ZCOUNT", key, std::string_view(minValue), std::string_view(maxValue)}, resource_), resource_);
}

Task<RedisScanResult> RedisHandle::scan(RedisScanOptions options) const {
    auto cursor = cursorString(options.cursor, resource_);
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(6);
    args.emplace_back(std::pmr::string("SCAN", 4, resource_));
    args.emplace_back(std::move(cursor));
    appendScanOptions(args, options, resource_);
    auto value = co_await executeOwned(pool_, std::move(args), resource_);
    co_return parseScanResult(value, resource_);
}

Task<RedisHashScanResult> RedisHandle::hscan(std::string_view key, RedisScanOptions options) const {
    auto cursor = cursorString(options.cursor, resource_);
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(7);
    args.emplace_back(std::pmr::string("HSCAN", 5, resource_));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource_));
    args.emplace_back(std::move(cursor));
    appendScanOptions(args, options, resource_);
    auto value = co_await executeOwned(pool_, std::move(args), resource_);
    co_return parseHashScanResult(value, resource_);
}

Task<RedisScanResult> RedisHandle::sscan(std::string_view key, RedisScanOptions options) const {
    auto cursor = cursorString(options.cursor, resource_);
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(7);
    args.emplace_back(std::pmr::string("SSCAN", 5, resource_));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource_));
    args.emplace_back(std::move(cursor));
    appendScanOptions(args, options, resource_);
    auto value = co_await executeOwned(pool_, std::move(args), resource_);
    co_return parseScanResult(value, resource_);
}

Task<RedisZScanResult> RedisHandle::zscan(std::string_view key, RedisScanOptions options) const {
    auto cursor = cursorString(options.cursor, resource_);
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(7);
    args.emplace_back(std::pmr::string("ZSCAN", 5, resource_));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource_));
    args.emplace_back(std::move(cursor));
    appendScanOptions(args, options, resource_);
    auto value = co_await executeOwned(pool_, std::move(args), resource_);
    co_return parseZScanResult(value, resource_);
}

Task<RedisValue> RedisHandle::eval(
    std::string_view script,
    std::span<const std::string_view> keys,
    std::span<const std::string_view> args) const {
    return executeOwned(pool_, evalArgs("EVAL", script, keys, args, resource_), resource_);
}

Task<RedisValue> RedisHandle::evalSha(
    std::string_view sha1,
    std::span<const std::string_view> keys,
    std::span<const std::string_view> args) const {
    return executeOwned(pool_, evalArgs("EVALSHA", sha1, keys, args, resource_), resource_);
}

Task<std::pmr::string> RedisHandle::scriptLoad(std::string_view script) const {
    return statusCommand(pool_, ownArgs({"SCRIPT", "LOAD", script}, resource_), resource_);
}

Task<std::pmr::vector<bool>> RedisHandle::scriptExists(std::span<const std::string_view> sha1s) const {
    if (sha1s.empty()) {
        throw std::invalid_argument("redis script exists requires at least one sha1");
    }
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(sha1s.size() + 2);
    args.emplace_back(std::pmr::string("SCRIPT", 6, resource_));
    args.emplace_back(std::pmr::string("EXISTS", 6, resource_));
    for (const auto sha1 : sha1s) {
        args.emplace_back(std::pmr::string(sha1.data(), sha1.size(), resource_));
    }
    return boolArrayCommand(pool_, std::move(args), resource_);
}

Task<std::optional<RedisKeyValue>> RedisHandle::blpop(std::span<const std::string_view> keys, std::chrono::seconds timeout) const {
    auto args = blockingPopArgs("BLPOP", keys, timeout, resource_);
    auto views = viewArgs(args, resource_);
    const auto clientTimeout = timeout <= std::chrono::seconds(0)
        ? std::chrono::milliseconds(0)
        : std::chrono::duration_cast<std::chrono::milliseconds>(timeout) + std::chrono::seconds(1);
    auto reply = co_await pool_.executeWithTimeout(std::span<const std::string_view>(views.data(), views.size()), clientTimeout, resource_);
    co_return parseBlockingPopReply(reply, resource_);
}

Task<std::optional<RedisKeyValue>> RedisHandle::brpop(std::span<const std::string_view> keys, std::chrono::seconds timeout) const {
    auto args = blockingPopArgs("BRPOP", keys, timeout, resource_);
    auto views = viewArgs(args, resource_);
    const auto clientTimeout = timeout <= std::chrono::seconds(0)
        ? std::chrono::milliseconds(0)
        : std::chrono::duration_cast<std::chrono::milliseconds>(timeout) + std::chrono::seconds(1);
    auto reply = co_await pool_.executeWithTimeout(std::span<const std::string_view>(views.data(), views.size()), clientTimeout, resource_);
    co_return parseBlockingPopReply(reply, resource_);
}

RedisPipeline RedisHandle::pipeline() const {
    return RedisPipeline(pool_, resource_, requestMemory_);
}

RedisTransaction RedisHandle::transaction() const {
    return RedisTransaction(pipeline());
}

namespace detail {

void RedisReaderDeleter::operator()(redisReader* reader) const noexcept {
    if (reader != nullptr) {
        redisReaderFree(reader);
    }
}

RedisPool::Connection::Connection(asio::io_context& ioContext, std::pmr::memory_resource* resource)
    : socket(ioContext),
      resolver(ioContext),
      writeBuffer(resolveResource(resource)),
      reader(redisReaderCreate()) {}

RedisPool::Connection::~Connection() = default;

RedisPool::Connection::Connection(Connection&&) noexcept = default;
RedisPool::Connection& RedisPool::Connection::operator=(Connection&&) noexcept = default;

RedisPool::ConnectionGuard::ConnectionGuard(RedisPool& pool, std::size_t index) noexcept
    : pool_(&pool),
      index_(index) {}

RedisPool::ConnectionGuard::~ConnectionGuard() {
    if (pool_ == nullptr) {
        return;
    }
    if (discard_) {
        pool_->close(pool_->connections_[index_]);
    }
    pool_->release(index_);
}

RedisPool::Connection& RedisPool::ConnectionGuard::connection() noexcept {
    return pool_->connections_[index_];
}

void RedisPool::ConnectionGuard::discard() noexcept {
    discard_ = true;
}

RedisPool::RedisPool(asio::io_context& ioContext, RedisConfig config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(resolveResource(resource)),
      connections_(resource_),
      free_(resource_) {
    const auto poolSize = std::max<std::size_t>(1, config_.poolSizePerWorker);
    connections_.reserve(poolSize);
    free_.reserve(poolSize);
    for (std::size_t i = 0; i < poolSize; ++i) {
        connections_.emplace_back(ioContext_, resource_);
        free_.push_back(i);
    }
}

RedisPool::~RedisPool() {
    closeNow();
}

Task<void> RedisPool::connect() {
    for (auto& connection : connections_) {
        if (!connection.connected) {
            co_await connect(connection);
        }
    }
    co_return;
}

void RedisPool::closeNow() noexcept {
    closing_ = true;
    while (waiterHead_ != nullptr) {
        auto* waiter = waiterHead_;
        removeWaiter(*waiter);
        if (waiter->ready != nullptr) {
            *waiter->ready = true;
        }
        if (waiter->handle) {
            waiter->handle.resume();
        }
    }
    for (auto& connection : connections_) {
        close(connection);
    }
}

void RedisPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    auto* waiter = waiterHead_;
    while (waiter != nullptr) {
        auto* next = waiter->next;
        if (config_.acquireTimeout.count() > 0 && waiter->deadline <= now) {
            removeWaiter(*waiter);
            if (waiter->timedOut != nullptr) {
                *waiter->timedOut = true;
            }
            if (waiter->ready != nullptr) {
                *waiter->ready = true;
            }
            if (waiter->handle) {
                waiter->handle.resume();
            }
        }
        waiter = next;
    }

    for (auto& connection : connections_) {
        if (!connection.deadlineActive || connection.deadline > now) {
            continue;
        }
        connection.timedOut = true;
        std::error_code ignored;
        if (connection.deadlineKind == Connection::DeadlineKind::kResolve) {
            connection.resolver.cancel();
        } else if (connection.deadlineKind == Connection::DeadlineKind::kSocket) {
            connection.socket.cancel(ignored);
        }
    }
}

bool RedisPool::hasAnyTimeout() const noexcept {
    return config_.connectTimeout.count() > 0 ||
        config_.commandTimeout.count() > 0 ||
        config_.acquireTimeout.count() > 0;
}

Task<std::size_t> RedisPool::acquire() {
    if (closing_) {
        throw RedisError(RedisError::Code::kIoError, "redis pool is closing");
    }
    if (!free_.empty()) {
        const auto index = free_.back();
        free_.pop_back();
        connections_[index].busy = true;
        co_return index;
    }

    struct WaiterGuard final {
        RedisPool& pool;
        PoolWaiter& waiter;

        ~WaiterGuard() {
            pool.removeWaiter(waiter);
        }
    };

    bool ready = false;
    bool timedOut = false;
    std::size_t waitedIndex = 0;
    PoolWaiter waiter{
        .ready = &ready,
        .timedOut = &timedOut,
        .index = &waitedIndex,
        .deadline = std::chrono::steady_clock::now() + config_.acquireTimeout};
    enqueueWaiter(waiter);
    WaiterGuard guard{*this, waiter};

    struct WaiterAwaiter final {
        PoolWaiter& waiter;
        bool& ready;

        [[nodiscard]] bool await_ready() const noexcept {
            return ready;
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            waiter.handle = handle;
        }

        void await_resume() const noexcept {}
    };

    co_await WaiterAwaiter{waiter, ready};

    if (timedOut) {
        throw RedisError(RedisError::Code::kTimeout, "redis connection pool acquire timed out");
    }

    if (closing_ || waitedIndex >= connections_.size()) {
        throw RedisError(RedisError::Code::kIoError, "redis pool is closing");
    }

    co_return waitedIndex;
}

void RedisPool::release(std::size_t index) noexcept {
    if (index >= connections_.size()) {
        return;
    }
    if (closing_) {
        connections_[index].busy = false;
        return;
    }
    if (resumeNextWaiter(index)) {
        connections_[index].busy = true;
        return;
    }
    connections_[index].busy = false;
    free_.push_back(index);
}

void RedisPool::enqueueWaiter(PoolWaiter& waiter) noexcept {
    if (waiter.queued) {
        return;
    }
    waiter.previous = waiterTail_;
    waiter.next = nullptr;
    waiter.queued = true;
    if (waiterTail_ != nullptr) {
        waiterTail_->next = &waiter;
    } else {
        waiterHead_ = &waiter;
    }
    waiterTail_ = &waiter;
}

void RedisPool::removeWaiter(PoolWaiter& waiter) noexcept {
    if (!waiter.queued) {
        return;
    }
    if (waiter.previous != nullptr) {
        waiter.previous->next = waiter.next;
    } else {
        waiterHead_ = waiter.next;
    }
    if (waiter.next != nullptr) {
        waiter.next->previous = waiter.previous;
    } else {
        waiterTail_ = waiter.previous;
    }
    waiter.previous = nullptr;
    waiter.next = nullptr;
    waiter.queued = false;
}

bool RedisPool::resumeNextWaiter(std::size_t index) noexcept {
    while (waiterHead_ != nullptr) {
        auto* waiter = waiterHead_;
        removeWaiter(*waiter);
        if (waiter->ready != nullptr && waiter->index != nullptr) {
            *waiter->index = index;
            *waiter->ready = true;
            if (waiter->handle) {
                waiter->handle.resume();
            }
            return true;
        }
    }
    return false;
}

void RedisPool::close(Connection& connection) noexcept {
    std::error_code ignored;
    connection.resolver.cancel();
    connection.socket.cancel(ignored);
    connection.socket.close(ignored);
    connection.connected = false;
    clearDeadline(connection);
    connection.reader.reset();
    connection.replyBytes = 0;
}

void RedisPool::configureSocket(Connection& connection) noexcept {
    std::error_code ignored;
    if (config_.tcpNoDelay) {
        connection.socket.set_option(asio::ip::tcp::no_delay(true), ignored);
    }
    if (config_.keepAlive) {
        connection.socket.set_option(asio::socket_base::keep_alive(true), ignored);
    }
}

void RedisPool::ensureReader(Connection& connection) {
    if (connection.reader == nullptr) {
        connection.reader.reset(redisReaderCreate());
    }
    if (connection.reader == nullptr) {
        throw RedisError(RedisError::Code::kProtocolError, "failed to create redis reader");
    }
    connection.replyBytes = 0;
}

void RedisPool::setDeadline(Connection& connection, std::chrono::milliseconds timeout, Connection::DeadlineKind kind) noexcept {
    connection.deadlineKind = kind;
    connection.timedOut = false;
    if (timeout.count() <= 0) {
        connection.deadlineActive = false;
        return;
    }
    connection.deadline = std::chrono::steady_clock::now() + timeout;
    connection.deadlineActive = true;
}

void RedisPool::clearDeadline(Connection& connection) noexcept {
    connection.deadlineActive = false;
    connection.deadlineKind = Connection::DeadlineKind::kNone;
}

Task<std::error_code> RedisPool::asyncSocketWrite(Connection& connection, std::chrono::milliseconds timeout) {
    setDeadline(connection, timeout, Connection::DeadlineKind::kSocket);
    auto ec = co_await asyncError([&connection](auto handler) mutable {
        asio::async_write(connection.socket, asio::buffer(connection.writeBuffer), std::move(handler));
    });
    const auto timedOut = connection.timedOut;
    clearDeadline(connection);
    if (timedOut) {
        co_return asio::error::timed_out;
    }
    co_return ec;
}

Task<std::pair<std::error_code, std::size_t>> RedisPool::asyncSocketReadSome(
    Connection& connection,
    std::span<char> buffer,
    std::chrono::milliseconds timeout) {
    setDeadline(connection, timeout, Connection::DeadlineKind::kSocket);
    auto result = co_await asyncResult<std::size_t>(
        [&connection, buffer](auto handler) mutable {
            connection.socket.async_read_some(asio::buffer(buffer.data(), buffer.size()), std::move(handler));
    });
    const auto timedOut = connection.timedOut;
    clearDeadline(connection);
    if (timedOut) {
        co_return std::pair<std::error_code, std::size_t>{asio::error::timed_out, 0};
    }
    co_return result;
}

Task<RedisValue> RedisPool::readReply(Connection& connection, std::chrono::milliseconds timeout, std::pmr::memory_resource* resource) {
    ensureReader(connection);
    for (;;) {
        void* rawReply = nullptr;
        const auto readerStatus = redisReaderGetReply(connection.reader.get(), &rawReply);
        if (readerStatus != REDIS_OK) {
            throw RedisError(
                RedisError::Code::kProtocolError,
                hiredisReaderError(*connection.reader));
        }
        if (rawReply != nullptr) {
            std::unique_ptr<redisReply, decltype(&freeReplyObject)> reply(
                static_cast<redisReply*>(rawReply),
                freeReplyObject);
            connection.replyBytes = 0;
            co_return hiredisReplyToValue(*reply, 0, config_.maxArrayDepth, resolveResource(resource));
        }

        std::array<char, 8192> buffer{};
        const auto [readEc, bytesRead] = co_await asyncSocketReadSome(connection, buffer, timeout);
        if (readEc) {
            if (readEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, readEc.message());
        }
        if (config_.maxReplyBytes > 0 && connection.replyBytes + bytesRead > config_.maxReplyBytes) {
            throw RedisError(RedisError::Code::kProtocolError, "redis reply exceeds configured limit");
        }
        connection.replyBytes += bytesRead;
        if (redisReaderFeed(connection.reader.get(), buffer.data(), bytesRead) != REDIS_OK) {
            throw RedisError(
                RedisError::Code::kProtocolError,
                hiredisReaderError(*connection.reader));
        }
    }
}

Task<void> RedisPool::connect(Connection& connection) {
    if (connection.connected) {
        co_return;
    }

    std::array<char, 8> portBuffer{};
    auto [portEnd, portEc] = std::to_chars(
        portBuffer.data(),
        portBuffer.data() + portBuffer.size(),
        config_.port);
    if (portEc != std::errc{}) {
        throw RedisError(RedisError::Code::kConnectFailed, "invalid redis port");
    }
    const auto port = std::string_view(portBuffer.data(), static_cast<std::size_t>(portEnd - portBuffer.data()));

    setDeadline(connection, config_.connectTimeout, Connection::DeadlineKind::kResolve);
    const auto [resolveEc, endpoints] = co_await asyncResult<asio::ip::tcp::resolver::results_type>(
        [this, &connection, port](auto handler) mutable {
            connection.resolver.async_resolve(
                config_.host,
                port,
                std::move(handler));
        });
    const auto resolveTimedOut = connection.timedOut;
    clearDeadline(connection);
    if (resolveTimedOut) {
        throw RedisError(RedisError::Code::kTimeout, "redis resolve timed out");
    }
    if (resolveEc) {
        throw RedisError(RedisError::Code::kConnectFailed, resolveEc.message());
    }

    setDeadline(connection, config_.connectTimeout, Connection::DeadlineKind::kSocket);
    const auto connectEc = co_await asyncError([&connection, &endpoints](auto handler) mutable {
        asio::async_connect(connection.socket, endpoints, std::move(handler));
    });
    const auto connectTimedOut = connection.timedOut;
    clearDeadline(connection);
    if (connectTimedOut) {
        throw RedisError(RedisError::Code::kTimeout, "redis connect timed out");
    }
    if (connectEc) {
        throw RedisError(RedisError::Code::kConnectFailed, connectEc.message());
    }
    configureSocket(connection);
    ensureReader(connection);
    connection.connected = true;
    connection.replyBytes = 0;

    try {
        co_await authenticate(connection);
    } catch (...) {
        close(connection);
        throw;
    }
}

Task<void> RedisPool::authenticate(Connection& connection) {
    auto runControl = [this, &connection](std::span<const std::string_view> args) -> Task<RedisValue> {
        connection.writeBuffer.clear();
        appendRespCommand(connection.writeBuffer, args);
        const auto timeout = config_.commandTimeout.count() > 0 ? config_.commandTimeout : config_.connectTimeout;
        const auto writeEc = co_await asyncSocketWrite(connection, timeout);
        if (writeEc) {
            if (writeEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, writeEc.message());
        }

        co_return co_await readReply(connection, timeout, resource_);
    };

    if (!config_.password.empty()) {
        RedisValue reply(resource_);
        if (!config_.username.empty()) {
            std::array<std::string_view, 3> args{
                "AUTH",
                std::string_view(config_.username),
                std::string_view(config_.password)};
            reply = co_await runControl(args);
        } else {
            std::array<std::string_view, 2> args{"AUTH", std::string_view(config_.password)};
            reply = co_await runControl(args);
        }
        if (reply.kind() == RedisValue::Kind::kError) {
            throw RedisError(RedisError::Code::kAuthFailed, reply.string());
        }
    }

    if (config_.database != 0) {
        std::pmr::string db(resource_);
        appendNumber(db, static_cast<std::uint64_t>(config_.database));
        std::array<std::string_view, 2> args{"SELECT", std::string_view(db)};
        auto reply = co_await runControl(args);
        if (reply.kind() == RedisValue::Kind::kError) {
            throw RedisError(RedisError::Code::kCommandError, reply.string());
        }
    }
}

Task<RedisValue> RedisPool::execute(std::span<const std::string_view> args, std::pmr::memory_resource* resource) {
    co_return co_await executeWithTimeout(args, config_.commandTimeout, resource);
}

Task<RedisValue> RedisPool::executeWithTimeout(
    std::span<const std::string_view> args,
    std::chrono::milliseconds timeout,
    std::pmr::memory_resource* resource) {
    const auto index = co_await acquire();
    ConnectionGuard guard(*this, index);
    auto& connection = guard.connection();
    try {
        if (!connection.connected) {
            co_await connect(connection);
        }

        connection.writeBuffer.clear();
        appendRespCommand(connection.writeBuffer, args);
        const auto writeEc = co_await asyncSocketWrite(connection, timeout);
        if (writeEc) {
            if (writeEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, writeEc.message());
        }

        co_return co_await readReply(connection, timeout, resource);
    } catch (...) {
        guard.discard();
        throw;
    }
}

Task<std::pmr::vector<RedisValue>> RedisPool::executePipeline(
    std::span<const RedisPipeline::Command> commands,
    std::pmr::memory_resource* resource) {
    const auto resolved = resolveResource(resource);
    std::pmr::vector<RedisValue> replies(resolved);
    replies.reserve(commands.size());
    if (commands.empty()) {
        co_return replies;
    }

    const auto index = co_await acquire();
    ConnectionGuard guard(*this, index);
    auto& connection = guard.connection();
    try {
        if (!connection.connected) {
            co_await connect(connection);
        }

        connection.writeBuffer.clear();
        for (const auto& command : commands) {
            auto views = viewArgs(command.args, resource_);
            appendRespCommand(connection.writeBuffer, std::span<const std::string_view>(views.data(), views.size()));
        }

        const auto writeEc = co_await asyncSocketWrite(connection, config_.commandTimeout);
        if (writeEc) {
            if (writeEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, writeEc.message());
        }

        while (replies.size() < commands.size()) {
            replies.emplace_back(co_await readReply(connection, config_.commandTimeout, resolved));
        }

        co_return replies;
    } catch (...) {
        guard.discard();
        throw;
    }
}

RedisRegistry::RedisRegistry(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource,
    std::span<const RedisDefinition> redis)
    : resource_(resolveResource(resource)),
      pools_(resource_) {
    pools_.reserve(redis.size());
    for (const auto& definition : redis) {
        if (definition.alias.empty()) {
            throw std::invalid_argument("redis alias must not be empty");
        }
        if (std::ranges::any_of(
                pools_,
                [&definition](const Entry& entry) {
                    return std::string_view(entry.alias.data(), entry.alias.size()) ==
                        std::string_view(definition.alias);
                })) {
            throw std::invalid_argument("duplicate redis alias");
        }
        pools_.push_back(Entry{
            std::pmr::string(definition.alias, resource_),
            std::make_unique<RedisPool>(ioContext, definition.config, resource_)});
        if (std::string_view(pools_.back().alias.data(), pools_.back().alias.size()) == kDefaultRedisAlias) {
            defaultPool_ = pools_.back().pool.get();
        }
    }
}

RedisRegistry::~RedisRegistry() = default;

Task<void> RedisRegistry::connect() {
    for (auto& entry : pools_) {
        co_await entry.pool->connect();
    }
    co_return;
}

void RedisRegistry::closeNow() noexcept {
    for (auto& entry : pools_) {
        entry.pool->closeNow();
    }
}

bool RedisRegistry::empty() const noexcept {
    return pools_.empty();
}

bool RedisRegistry::hasAnyTimeout() const noexcept {
    return std::ranges::any_of(pools_, [](const Entry& entry) {
        return entry.pool->hasAnyTimeout();
    });
}

RedisHandle RedisRegistry::get(std::pmr::memory_resource* resource, RequestMemory* requestMemory) const {
    if (defaultPool_ == nullptr) {
        throw RedisError(RedisError::Code::kNotConfigured, "default redis is not configured");
    }
    return RedisHandle(*defaultPool_, resource, requestMemory);
}

RedisHandle RedisRegistry::get(
    std::string_view alias,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) const {
    for (const auto& entry : pools_) {
        if (std::string_view(entry.alias.data(), entry.alias.size()) == alias) {
            return RedisHandle(*entry.pool, resource, requestMemory);
        }
    }
    throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
}

void RedisRegistry::scanDeadlines() noexcept {
    const auto now = std::chrono::steady_clock::now();
    for (auto& entry : pools_) {
        entry.pool->scanDeadlines(now);
    }
}

}  // namespace detail

RedisHandle Context::redis() const {
    if (redis_ == nullptr) {
        throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
    }
    return redis_->get(resource(), const_cast<RequestMemory*>(&memory_));
}

RedisHandle Context::redis(std::string_view alias) const {
    if (redis_ == nullptr) {
        throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
    }
    return redis_->get(alias, resource(), const_cast<RequestMemory*>(&memory_));
}

}  // namespace ruvia
