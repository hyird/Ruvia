#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisRegistry.h"
#include <stdexcept>
#include <utility>

namespace ruvia {

RedisTransaction::RedisTransaction(RedisPipeline pipeline) noexcept
    : detail::ScopedCapabilityNode(pipeline.operationScope(), &RedisTransaction::expireCapability),
      pipeline_(std::move(pipeline)),
      watches_(pipeline_.resource()) {}

void RedisTransaction::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& transaction = static_cast<RedisTransaction&>(capability);
    std::pmr::vector<RedisPipeline::Command> empty(transaction.watches_.get_allocator().resource());
    transaction.watches_.swap(empty);
}

RedisTransaction& RedisTransaction::command(std::span<const std::string_view> args) {
    pipeline_.command(args);
    return *this;
}

RedisTransaction& RedisTransaction::watch(std::string_view key) {
    return watch(std::span<const std::string_view>(&key, 1));
}

RedisTransaction& RedisTransaction::watch(std::span<const std::string_view> keys) {
    pipeline_.requireActive();
    if (keys.empty()) {
        return *this;
    }
    RedisPipeline::appendCommand(watches_, pipeline_.resource(), "WATCH", keys);
    return *this;
}

RedisTransaction& RedisTransaction::unwatch() {
    pipeline_.requireActive();
    RedisPipeline::appendCommand(watches_, pipeline_.resource(), "UNWATCH");
    return *this;
}

RedisTransaction& RedisTransaction::get(std::string_view key) {
    pipeline_.get(key);
    return *this;
}

RedisTransaction& RedisTransaction::set(std::string_view key, std::string_view value) {
    pipeline_.set(key, value);
    return *this;
}

RedisTransaction& RedisTransaction::getDel(std::string_view key) {
    pipeline_.getDel(key);
    return *this;
}

RedisTransaction& RedisTransaction::getSet(std::string_view key, std::string_view value) {
    pipeline_.getSet(key, value);
    return *this;
}

RedisTransaction& RedisTransaction::append(std::string_view key, std::string_view value) {
    pipeline_.append(key, value);
    return *this;
}

RedisTransaction& RedisTransaction::strlen(std::string_view key) {
    pipeline_.strlen(key);
    return *this;
}

RedisTransaction& RedisTransaction::del(std::string_view key) {
    pipeline_.del(key);
    return *this;
}

RedisTransaction& RedisTransaction::unlink(std::string_view key) {
    pipeline_.unlink(key);
    return *this;
}

RedisTransaction& RedisTransaction::exists(std::string_view key) {
    pipeline_.exists(key);
    return *this;
}

RedisTransaction& RedisTransaction::touch(std::string_view key) {
    pipeline_.touch(key);
    return *this;
}

RedisTransaction& RedisTransaction::type(std::string_view key) {
    pipeline_.type(key);
    return *this;
}

RedisTransaction& RedisTransaction::rename(std::string_view key, std::string_view newKey) {
    pipeline_.rename(key, newKey);
    return *this;
}

RedisTransaction& RedisTransaction::renameNx(std::string_view key, std::string_view newKey) {
    pipeline_.renameNx(key, newKey);
    return *this;
}

RedisTransaction& RedisTransaction::incr(std::string_view key) {
    pipeline_.incr(key);
    return *this;
}

RedisTransaction& RedisTransaction::incrBy(std::string_view key, std::int64_t value) {
    pipeline_.incrBy(key, value);
    return *this;
}

RedisTransaction& RedisTransaction::decr(std::string_view key) {
    pipeline_.decr(key);
    return *this;
}

RedisTransaction& RedisTransaction::decrBy(std::string_view key, std::int64_t value) {
    pipeline_.decrBy(key, value);
    return *this;
}

RedisTransaction& RedisTransaction::hget(std::string_view key, std::string_view field) {
    pipeline_.hget(key, field);
    return *this;
}

RedisTransaction& RedisTransaction::hset(std::string_view key, std::string_view field, std::string_view value) {
    pipeline_.hset(key, field, value);
    return *this;
}

RedisTransaction& RedisTransaction::hdel(std::string_view key, std::string_view field) {
    pipeline_.hdel(key, field);
    return *this;
}

RedisTransaction& RedisTransaction::hexists(std::string_view key, std::string_view field) {
    pipeline_.hexists(key, field);
    return *this;
}

RedisTransaction& RedisTransaction::hlen(std::string_view key) {
    pipeline_.hlen(key);
    return *this;
}

RedisTransaction& RedisTransaction::hgetAll(std::string_view key) {
    pipeline_.hgetAll(key);
    return *this;
}

RedisTransaction& RedisTransaction::lpush(std::string_view key, std::string_view value) {
    pipeline_.lpush(key, value);
    return *this;
}

RedisTransaction& RedisTransaction::rpush(std::string_view key, std::string_view value) {
    pipeline_.rpush(key, value);
    return *this;
}

RedisTransaction& RedisTransaction::lpop(std::string_view key) {
    pipeline_.lpop(key);
    return *this;
}

RedisTransaction& RedisTransaction::rpop(std::string_view key) {
    pipeline_.rpop(key);
    return *this;
}

RedisTransaction& RedisTransaction::llen(std::string_view key) {
    pipeline_.llen(key);
    return *this;
}

RedisTransaction& RedisTransaction::lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    pipeline_.lrange(key, start, stop);
    return *this;
}

RedisTransaction& RedisTransaction::sadd(std::string_view key, std::string_view member) {
    pipeline_.sadd(key, member);
    return *this;
}

RedisTransaction& RedisTransaction::srem(std::string_view key, std::string_view member) {
    pipeline_.srem(key, member);
    return *this;
}

RedisTransaction& RedisTransaction::smembers(std::string_view key) {
    pipeline_.smembers(key);
    return *this;
}

RedisTransaction& RedisTransaction::scard(std::string_view key) {
    pipeline_.scard(key);
    return *this;
}

RedisTransaction& RedisTransaction::zadd(std::string_view key, double score, std::string_view member) {
    pipeline_.zadd(key, score, member);
    return *this;
}

RedisTransaction& RedisTransaction::zrem(std::string_view key, std::string_view member) {
    pipeline_.zrem(key, member);
    return *this;
}

RedisTransaction& RedisTransaction::zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    pipeline_.zrange(key, start, stop);
    return *this;
}

RedisTransaction& RedisTransaction::zscore(std::string_view key, std::string_view member) {
    pipeline_.zscore(key, member);
    return *this;
}

RedisTransaction& RedisTransaction::zcard(std::string_view key) {
    pipeline_.zcard(key);
    return *this;
}

Task<std::pmr::vector<RedisValue>> RedisTransaction::executeOwned(detail::RedisPool& pool, std::pmr::memory_resource* resource, std::pmr::vector<RedisPipeline::Command> watches, std::pmr::vector<RedisPipeline::Command> commands) {
    std::pmr::vector<detail::RedisCommandArgsView> framed(resource);
    framed.reserve(watches.size() + commands.size() + 2);
    auto appendCommandView = [&framed](const RedisPipeline::Command& command) { framed.emplace_back(command.args); };
    auto multi = RedisPipeline::makeCommand(resource, "MULTI");
    auto exec = RedisPipeline::makeCommand(resource, "EXEC");
    for (const auto& command : watches) {
        appendCommandView(command);
    }
    appendCommandView(multi);
    for (const auto& command : commands) {
        appendCommandView(command);
    }
    appendCommandView(exec);

    auto replies = co_await pool.executePipeline(std::span<const detail::RedisCommandArgsView>(framed), resource);
    if (replies.empty() || replies.back().kind() == RedisValue::Kind::kError) {
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

ScopedOperation<std::pmr::vector<RedisValue>> RedisTransaction::exec() && {
    auto* commandResource = pipeline_.resource();
    auto& pool = pipeline_.consumePool();
    return detail::makeScopedOperation(pipeline_.operationScope(), executeOwned(pool, commandResource, std::move(watches_), std::move(pipeline_.commands_)));
}

}  // namespace ruvia
