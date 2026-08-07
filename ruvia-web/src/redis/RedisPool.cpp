#include "ruvia/web/redis/Redis.h"

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisProtocol.h"
#include <asio/error.hpp>

#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ruvia {
namespace detail {
namespace {

[[nodiscard]] std::span<const std::pmr::string> redisArgSpan(const std::pmr::vector<std::pmr::string>& args) noexcept {
    return args;
}

}  // namespace

Task<RedisValue> RedisPool::executeOwned(std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource, RedisOperationOptions options) {
    return executeWithTimeoutImpl(std::move(args), std::move(options), resource);
}

template <typename ArgSource>
Task<RedisValue> RedisPool::executeWithTimeoutImpl(ArgSource args, RedisOperationOptions options, std::pmr::memory_resource* resource) {
    const OperationTimeout operationTimeout(options.timeout);
    const auto index = co_await acquire(operationTimeout, options.stopToken);
    ConnectionGuard guard(*this, index);
    auto& connection = guard.connection();
    connection.abortReason = Connection::AbortReason::kNone;
    const auto generation = ++connection.operationGeneration;
    auto stopRegistration = options.stopToken.registerCallback([pool = this, index, generation] {
        WorkerHandleAccess::deferOrTerminate(*pool->worker_, [pool, index, generation] {
            pool->cancelOperation(index, generation);
        });
    });
    if (options.stopToken.stopRequested()) {
        cancelOperation(index, generation);
    }
    auto finishCancellation = [&]() noexcept {
        if (connection.operationGeneration == generation) {
            if (++connection.operationGeneration == 0) {
                ++connection.operationGeneration;
            }
        }
        stopRegistration.reset();
    };
    try {
        if (!connection.connected) {
            co_await connect(connection, &operationTimeout);
        }
        throwIfCancelled(connection);

        connection.writeBuffer.clear();
        const auto argSpan = redisArgSpan(args);
        connection.writeBuffer.reserve(respCommandSerializedSize(argSpan));
        appendRespCommand(connection.writeBuffer, argSpan);
        const auto deadline = operationTimeout.constrainedBy(config_.commandTimeout);
        const auto writeEc = co_await asyncSocketWrite(connection, deadline);
        throwIfCancelled(connection);
        if (writeEc) {
            if (writeEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, writeEc.message());
        }

        auto reply = co_await readReply(connection, deadline, resource);
        throwIfCancelled(connection);
        finishCancellation();
        co_return reply;
    } catch (...) {
        finishCancellation();
        guard.discard();
        throw;
    }
}

template <typename CommandSource>
Task<std::pmr::vector<RedisValue>> RedisPool::executePipelineImpl(CommandSource commands, RedisOperationOptions options, std::pmr::memory_resource* resource) {
    const auto resolved = detail::pmrResourceOrDefault(resource);
    std::pmr::vector<RedisValue> replies(resolved);
    replies.reserve(commands.size());
    if (commands.empty()) {
        co_return replies;
    }

    const OperationTimeout operationTimeout(options.timeout);
    const auto index = co_await acquire(operationTimeout, options.stopToken);
    ConnectionGuard guard(*this, index);
    auto& connection = guard.connection();
    connection.abortReason = Connection::AbortReason::kNone;
    const auto generation = ++connection.operationGeneration;
    auto stopRegistration = options.stopToken.registerCallback([pool = this, index, generation] {
        WorkerHandleAccess::deferOrTerminate(*pool->worker_, [pool, index, generation] {
            pool->cancelOperation(index, generation);
        });
    });
    if (options.stopToken.stopRequested()) {
        cancelOperation(index, generation);
    }
    auto finishCancellation = [&]() noexcept {
        if (connection.operationGeneration == generation) {
            if (++connection.operationGeneration == 0) {
                ++connection.operationGeneration;
            }
        }
        stopRegistration.reset();
    };
    try {
        if (!connection.connected) {
            co_await connect(connection, &operationTimeout);
        }
        throwIfCancelled(connection);

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

        const auto deadline = operationTimeout.constrainedBy(config_.commandTimeout);
        const auto writeEc = co_await asyncSocketWrite(connection, deadline);
        throwIfCancelled(connection);
        if (writeEc) {
            if (writeEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, writeEc.message());
        }

        while (replies.size() < commands.size()) {
            replies.emplace_back(co_await readReply(connection, deadline, resolved));
            throwIfCancelled(connection);
        }

        finishCancellation();
        co_return replies;
    } catch (...) {
        finishCancellation();
        guard.discard();
        throw;
    }
}

void RedisPool::cancelOperation(std::size_t index, std::uint64_t generation) noexcept {
    if (index >= connections_.size()) {
        std::terminate();
    }
    auto& connection = connections_[index];
    if (connection.operationGeneration != generation) {
        return;
    }
    connection.abortReason = Connection::AbortReason::kCancelled;
    close(connection);
}

void RedisPool::throwIfCancelled(const Connection& connection) const {
    if (connection.abortReason == Connection::AbortReason::kCancelled) {
        throw RedisError(RedisError::Code::kCancelled, "redis operation cancelled");
    }
}

Task<std::pmr::vector<RedisValue>> RedisPool::executePipeline(std::span<const RedisPipeline::Command> commands, RedisOperationOptions options, std::pmr::memory_resource* resource) {
    return executePipelineImpl(commands, std::move(options), resource);
}

Task<std::pmr::vector<RedisValue>> RedisPool::executePipeline(std::span<const RedisCommandArgsView> commands, RedisOperationOptions options, std::pmr::memory_resource* resource) {
    return executePipelineImpl(commands, std::move(options), resource);
}

}  // namespace detail

}  // namespace ruvia
