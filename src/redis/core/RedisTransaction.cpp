#include "ruvia/redis/Redis.h"

#include "../RedisInternal.h"
#include "RedisUtils.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

RedisTransaction::RedisTransaction(RedisPipeline pipeline) noexcept
    : pipeline_(std::move(pipeline)),
      watches_(pipeline_.resource_) {}

RedisTransaction& RedisTransaction::markActive() noexcept {
    discarded_ = false;
    return *this;
}

RedisTransaction& RedisTransaction::command(std::initializer_list<std::string_view> args) {
    pipeline_.command(args);
    return markActive();
}

RedisTransaction& RedisTransaction::command(std::span<const std::string_view> args) {
    pipeline_.command(args);
    return markActive();
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
    detail::emplaceRedisString(command.args, "WATCH");
    for (const auto key : keys) {
        detail::emplaceRedisString(command.args, key);
    }
    watches_.emplace_back(std::move(command));
    return markActive();
}

RedisTransaction& RedisTransaction::unwatch() {
    RedisPipeline::Command command{std::pmr::vector<std::pmr::string>(pipeline_.resource_)};
    detail::emplaceRedisString(command.args, "UNWATCH");
    watches_.emplace_back(std::move(command));
    return markActive();
}

RedisTransaction& RedisTransaction::discard() noexcept {
    watches_.clear();
    pipeline_.commands_.clear();
    discarded_ = true;
    return *this;
}

RedisTransaction& RedisTransaction::get(std::string_view key) {
    pipeline_.get(key);
    return markActive();
}

RedisTransaction& RedisTransaction::set(std::string_view key, std::string_view value) {
    pipeline_.set(key, value);
    return markActive();
}

RedisTransaction& RedisTransaction::getDel(std::string_view key) {
    pipeline_.getDel(key);
    return markActive();
}

RedisTransaction& RedisTransaction::getSet(std::string_view key, std::string_view value) {
    pipeline_.getSet(key, value);
    return markActive();
}

RedisTransaction& RedisTransaction::append(std::string_view key, std::string_view value) {
    pipeline_.append(key, value);
    return markActive();
}

RedisTransaction& RedisTransaction::strlen(std::string_view key) {
    pipeline_.strlen(key);
    return markActive();
}

RedisTransaction& RedisTransaction::del(std::string_view key) {
    pipeline_.del(key);
    return markActive();
}

RedisTransaction& RedisTransaction::unlink(std::string_view key) {
    pipeline_.unlink(key);
    return markActive();
}

RedisTransaction& RedisTransaction::exists(std::string_view key) {
    pipeline_.exists(key);
    return markActive();
}

RedisTransaction& RedisTransaction::touch(std::string_view key) {
    pipeline_.touch(key);
    return markActive();
}

RedisTransaction& RedisTransaction::type(std::string_view key) {
    pipeline_.type(key);
    return markActive();
}

RedisTransaction& RedisTransaction::rename(std::string_view key, std::string_view newKey) {
    pipeline_.rename(key, newKey);
    return markActive();
}

RedisTransaction& RedisTransaction::renameNx(std::string_view key, std::string_view newKey) {
    pipeline_.renameNx(key, newKey);
    return markActive();
}

RedisTransaction& RedisTransaction::incr(std::string_view key) {
    pipeline_.incr(key);
    return markActive();
}

RedisTransaction& RedisTransaction::incrBy(std::string_view key, std::int64_t value) {
    pipeline_.incrBy(key, value);
    return markActive();
}

RedisTransaction& RedisTransaction::decr(std::string_view key) {
    pipeline_.decr(key);
    return markActive();
}

RedisTransaction& RedisTransaction::decrBy(std::string_view key, std::int64_t value) {
    pipeline_.decrBy(key, value);
    return markActive();
}

RedisTransaction& RedisTransaction::hget(std::string_view key, std::string_view field) {
    pipeline_.hget(key, field);
    return markActive();
}

RedisTransaction& RedisTransaction::hset(std::string_view key, std::string_view field, std::string_view value) {
    pipeline_.hset(key, field, value);
    return markActive();
}

RedisTransaction& RedisTransaction::hdel(std::string_view key, std::string_view field) {
    pipeline_.hdel(key, field);
    return markActive();
}

RedisTransaction& RedisTransaction::hexists(std::string_view key, std::string_view field) {
    pipeline_.hexists(key, field);
    return markActive();
}

RedisTransaction& RedisTransaction::hlen(std::string_view key) {
    pipeline_.hlen(key);
    return markActive();
}

RedisTransaction& RedisTransaction::hgetAll(std::string_view key) {
    pipeline_.hgetAll(key);
    return markActive();
}

RedisTransaction& RedisTransaction::lpush(std::string_view key, std::string_view value) {
    pipeline_.lpush(key, value);
    return markActive();
}

RedisTransaction& RedisTransaction::rpush(std::string_view key, std::string_view value) {
    pipeline_.rpush(key, value);
    return markActive();
}

RedisTransaction& RedisTransaction::lpop(std::string_view key) {
    pipeline_.lpop(key);
    return markActive();
}

RedisTransaction& RedisTransaction::rpop(std::string_view key) {
    pipeline_.rpop(key);
    return markActive();
}

RedisTransaction& RedisTransaction::llen(std::string_view key) {
    pipeline_.llen(key);
    return markActive();
}

RedisTransaction& RedisTransaction::lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    pipeline_.lrange(key, start, stop);
    return markActive();
}

RedisTransaction& RedisTransaction::sadd(std::string_view key, std::string_view member) {
    pipeline_.sadd(key, member);
    return markActive();
}

RedisTransaction& RedisTransaction::srem(std::string_view key, std::string_view member) {
    pipeline_.srem(key, member);
    return markActive();
}

RedisTransaction& RedisTransaction::smembers(std::string_view key) {
    pipeline_.smembers(key);
    return markActive();
}

RedisTransaction& RedisTransaction::scard(std::string_view key) {
    pipeline_.scard(key);
    return markActive();
}

RedisTransaction& RedisTransaction::zadd(std::string_view key, double score, std::string_view member) {
    pipeline_.zadd(key, score, member);
    return markActive();
}

RedisTransaction& RedisTransaction::zrem(std::string_view key, std::string_view member) {
    pipeline_.zrem(key, member);
    return markActive();
}

RedisTransaction& RedisTransaction::zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    pipeline_.zrange(key, start, stop);
    return markActive();
}

RedisTransaction& RedisTransaction::zscore(std::string_view key, std::string_view member) {
    pipeline_.zscore(key, member);
    return markActive();
}

RedisTransaction& RedisTransaction::zcard(std::string_view key) {
    pipeline_.zcard(key);
    return markActive();
}

Task<std::pmr::vector<RedisValue>> RedisTransaction::exec() {
    if (discarded_) {
        co_return std::pmr::vector<RedisValue>(pipeline_.resource_);
    }
    auto& commands = pipeline_.commands_;
    const auto resource = pipeline_.resource_;
    std::pmr::vector<detail::RedisCommandArgsView> framed(resource);
    framed.reserve(watches_.size() + commands.size() + 2);
    auto makeFrameCommand = [resource](std::string_view value) {
        RedisPipeline::Command command{std::pmr::vector<std::pmr::string>(resource)};
        detail::emplaceRedisString(command.args, value);
        return command;
    };
    auto appendCommandView = [&framed](const RedisPipeline::Command& command) {
        framed.emplace_back(std::span<const std::pmr::string>(command.args.data(), command.args.size()));
    };
    auto multi = makeFrameCommand("MULTI");
    auto exec = makeFrameCommand("EXEC");
    for (const auto& command : watches_) {
        appendCommandView(command);
    }
    appendCommandView(multi);
    for (const auto& command : commands) {
        appendCommandView(command);
    }
    appendCommandView(exec);

    auto replies = co_await pipeline_.pool_->executePipeline(
        std::span<const detail::RedisCommandArgsView>(framed.data(), framed.size()),
        resource);
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
