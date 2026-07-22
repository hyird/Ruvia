#include "ruvia/web/redis/Redis.h"

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/web/detail/redis/RedisInternal.h"
#include "ruvia/web/detail/redis/RedisProtocol.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <asio/error.hpp>

#include <system_error>
#include <utility>

namespace ruvia {
namespace detail {
namespace {

[[nodiscard]] std::span<const std::pmr::string> redisArgSpan(
    std::span<const std::pmr::string> args) noexcept {
    return args;
}

[[nodiscard]] std::span<const std::pmr::string> redisArgSpan(
    const std::pmr::vector<std::pmr::string>& args) noexcept {
    return args;
}

}  // namespace

Task<RedisValue> RedisPool::executeOwned(
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    return executeWithTimeoutImpl(std::move(args), config_.commandTimeout, resource);
}

template <typename ArgSource>
Task<RedisValue> RedisPool::executeWithTimeoutImpl(
    ArgSource args,
    std::optional<std::chrono::milliseconds> timeout,
    std::pmr::memory_resource* resource) {
    const auto index = co_await acquire();
    ConnectionGuard guard(*this, index);
    auto& connection = guard.connection();
    try {
        if (!connection.connected) {
            co_await connect(connection);
        }

        connection.writeBuffer.clear();
        const auto argSpan = redisArgSpan(args);
        connection.writeBuffer.reserve(respCommandSerializedSize(argSpan));
        appendRespCommand(connection.writeBuffer, argSpan);
        const OperationTimeout deadline(timeout);
        const auto writeEc = co_await asyncSocketWrite(connection, deadline);
        if (writeEc) {
            if (writeEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, writeEc.message());
        }

        co_return co_await readReply(connection, deadline, resource);
    } catch (...) {
        guard.discard();
        throw;
    }
}

Task<RedisValue> RedisPool::executeWithTimeout(
    std::span<const std::pmr::string> args,
    std::optional<std::chrono::milliseconds> timeout,
    std::pmr::memory_resource* resource) {
    return executeWithTimeoutImpl(args, timeout, resource);
}

template <typename CommandSource>
Task<std::pmr::vector<RedisValue>> RedisPool::executePipelineImpl(
    CommandSource commands,
    std::pmr::memory_resource* resource) {
    const auto resolved = detail::pmrResourceOrDefault(resource);
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
        std::size_t serializedBytes = 0;
        for (const auto& command : commands) {
            const std::span<const std::pmr::string> args = command.args;
            serializedBytes += respCommandSerializedSize(
                args);
        }
        connection.writeBuffer.reserve(serializedBytes);
        for (const auto& command : commands) {
            const std::span<const std::pmr::string> args = command.args;
            appendRespCommand(
                connection.writeBuffer,
                args);
        }

        const OperationTimeout deadline(config_.commandTimeout);
        const auto writeEc = co_await asyncSocketWrite(connection, deadline);
        if (writeEc) {
            if (writeEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, writeEc.message());
        }

        while (replies.size() < commands.size()) {
            replies.emplace_back(co_await readReply(connection, deadline, resolved));
        }

        co_return replies;
    } catch (...) {
        guard.discard();
        throw;
    }
}

Task<std::pmr::vector<RedisValue>> RedisPool::executePipeline(
    std::span<const RedisPipeline::Command> commands,
    std::pmr::memory_resource* resource) {
    return executePipelineImpl(commands, resource);
}

Task<std::pmr::vector<RedisValue>> RedisPool::executePipeline(
    std::span<const RedisCommandArgsView> commands,
    std::pmr::memory_resource* resource) {
    return executePipelineImpl(commands, resource);
}

}  // namespace detail

}  // namespace ruvia
