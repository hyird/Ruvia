#include "ruvia/web/redis/Redis.h"

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisProtocol.h"
#include "ruvia/core/detail/worker/WorkerCancellationPost.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace ruvia {
namespace detail {
namespace {

[[nodiscard]] std::span<const std::pmr::string> redisArgSpan(
    const std::pmr::vector<std::pmr::string>& args) noexcept {
    return args;
}

}  // namespace

static_assert(workerCancellationPostIsInline<RedisOperationCancellationMailbox>);

Task<RedisValue> RedisPool::executeOwned(std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource, OperationOptions options) {
    return executeWithTimeoutImpl(std::move(args), std::move(options), resource);
}

template <typename ArgSource>
Task<RedisValue> RedisPool::executeWithTimeoutImpl(
    ArgSource args, OperationOptions options, std::pmr::memory_resource* resource) {
    const OperationTimeout operationTimeout(options.timeout);
    const auto index = co_await acquire(operationTimeout, options.stopToken);
    ConnectionGuard guard(*this, index, options.stopToken);
    auto& connection = guard.connection();
    try {
        if (!connection.connected) {
            co_await connect(connection, &operationTimeout);
        }
        throwIfAborted(connection);

        connection.writeBuffer.clear();
        const auto argSpan = redisArgSpan(args);
        connection.writeBuffer.reserve(respCommandSerializedSize(argSpan));
        appendRespCommand(connection.writeBuffer, argSpan);
        const auto deadline = operationTimeout.constrainedBy(commandTimeout_);
        co_await asyncSocketWrite(connection, deadline);

        auto reply = co_await readReply(connection, deadline, resource);
        throwIfAborted(connection);
        co_return reply;
    } catch (...) {
        guard.discard();
        throw;
    }
}

template <typename CommandSource>
Task<std::pmr::vector<RedisValue>> RedisPool::executePipelineImpl(
    CommandSource commands, OperationOptions options, std::pmr::memory_resource* resource) {
    const auto resolved = detail::pmrResourceOrDefault(resource);
    std::pmr::vector<RedisValue> replies(resolved);
    replies.reserve(commands.size());
    if (commands.empty()) {
        co_return replies;
    }

    const OperationTimeout operationTimeout(options.timeout);
    const auto index = co_await acquire(operationTimeout, options.stopToken);
    ConnectionGuard guard(*this, index, options.stopToken);
    auto& connection = guard.connection();
    try {
        if (!connection.connected) {
            co_await connect(connection, &operationTimeout);
        }
        throwIfAborted(connection);

        connection.writeBuffer.clear();
        std::size_t serializedBytes = 0;
        for (const auto& command : commands) {
            const std::span<const std::pmr::string> args = command.args;
            const auto commandBytes = respCommandSerializedSize(args);
            if (commandBytes > std::numeric_limits<std::size_t>::max() - serializedBytes) {
                throw std::length_error("redis RESP pipeline is too large");
            }
            serializedBytes += commandBytes;
        }
        connection.writeBuffer.reserve(serializedBytes);
        for (const auto& command : commands) {
            const std::span<const std::pmr::string> args = command.args;
            appendRespCommand(connection.writeBuffer, args);
        }

        const auto deadline = operationTimeout.constrainedBy(commandTimeout_);
        co_await asyncSocketWrite(connection, deadline);

        while (replies.size() < commands.size()) {
            replies.emplace_back(co_await readReply(connection, deadline, resolved));
            throwIfAborted(connection);
        }

        co_return replies;
    } catch (...) {
        guard.discard();
        throw;
    }
}

void RedisPool::cancelOperationById(std::uint64_t cancellationId) noexcept {
    if (cancellationId == 0) {
        return;
    }
    for (auto& connection : connections_) {
        if (connection.cancellationId != cancellationId) {
            continue;
        }
        connection.cancellationId = 0;
        connection.abortReason = Connection::AbortReason::kCancelled;
        close(connection);
        return;
    }
}

void RedisPool::throwIfAborted(const Connection& connection) const {
    if (connection.abortReason == Connection::AbortReason::kCancelled) {
        throw RedisError(RedisError::Code::kCancelled, "redis operation cancelled");
    }
    if (connection.abortReason == Connection::AbortReason::kClosing) {
        throw RedisError(RedisError::Code::kClosing, "redis pool is closing");
    }
}

Task<std::pmr::vector<RedisValue>> RedisPool::executePipeline(
    std::span<const RedisPipeline::Command> commands, OperationOptions options,
    std::pmr::memory_resource* resource) {
    return executePipelineImpl(commands, std::move(options), resource);
}

Task<std::pmr::vector<RedisValue>> RedisPool::executePipeline(
    std::span<const RedisCommandArgsView> commands, OperationOptions options,
    std::pmr::memory_resource* resource) {
    return executePipelineImpl(commands, std::move(options), resource);
}

}  // namespace detail

}  // namespace ruvia
