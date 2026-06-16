#include "ruvia/redis/Redis.h"

#include "RedisInternal.h"
#include "RedisUtils.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

RedisPipeline::RedisPipeline(
    detail::RedisPool& pool,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) noexcept
    : pool_(&pool),
      resource_(detail::resolveRedisResource(resource)),
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
    auto amount = detail::redisIntString(value, resource_);
    return command({"INCRBY", key, std::string_view(amount)});
}

RedisPipeline& RedisPipeline::decr(std::string_view key) {
    return command({"DECR", key});
}

RedisPipeline& RedisPipeline::decrBy(std::string_view key, std::int64_t value) {
    auto amount = detail::redisIntString(value, resource_);
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
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
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
    auto scoreValue = detail::redisScoreString(score, resource_);
    return command({"ZADD", key, std::string_view(scoreValue), member});
}

RedisPipeline& RedisPipeline::zrem(std::string_view key, std::string_view member) {
    return command({"ZREM", key, member});
}

RedisPipeline& RedisPipeline::zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
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
        auto views = detail::viewRedisArgs(command.args, resource);
        framed.command(std::span<const std::string_view>(views.data(), views.size()));
    }
    framed.command({"MULTI"});
    for (const auto& command : commands) {
        auto views = detail::viewRedisArgs(command.args, resource);
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

}  // namespace ruvia
