#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
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

Task<std::pmr::vector<RedisValue>> RedisTransaction::executeOwned(detail::RedisPool& pool, OperationOptions options, std::pmr::memory_resource* resource, std::pmr::vector<RedisPipeline::Command> watches, std::pmr::vector<RedisPipeline::Command> commands) {
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

    auto replies = co_await pool.executePipeline(std::span<const detail::RedisCommandArgsView>(framed), std::move(options), resource);
    if (replies.empty()) {
        throw RedisError(RedisError::Code::kProtocolError, "redis transaction returned no replies");
    }
    for (std::size_t i = 0; i < replies.size(); ++i) {
        detail::throwIfRedisTransactionReplyError(replies[i], i);
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
    return detail::makeScopedOperation(pipeline_.operationScope(), executeOwned(pool, pipeline_.operationOptions_, commandResource, std::move(watches_), std::move(pipeline_.commands_)));
}

}  // namespace ruvia
