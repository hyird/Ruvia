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

struct RedisPool::OperationCancellation final {
    RedisPool* pool{nullptr};
    std::size_t index{0};
    std::uint64_t generation{0};
    bool active{true};
};

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
    auto [cancellation, stopRegistration] = registerCancellation(index, generation, std::move(options.stopToken));
    auto finishCancellation = [&]() noexcept {
        if (cancellation != nullptr) {
            cancellation->active = false;
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
Task<std::pmr::vector<RedisValue>> RedisPool::executePipelineImpl(CommandSource commands, std::pmr::memory_resource* resource) {
    const auto resolved = detail::pmrResourceOrDefault(resource);
    std::pmr::vector<RedisValue> replies(resolved);
    replies.reserve(commands.size());
    if (commands.empty()) {
        co_return replies;
    }

    const OperationTimeout operationTimeout(std::nullopt);
    const auto index = co_await acquire(operationTimeout, {});
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

std::pair<std::shared_ptr<RedisPool::OperationCancellation>, StopRegistration> RedisPool::registerCancellation(std::size_t index, std::uint64_t generation, StopToken stopToken) {
    if (!stopToken.stoppable()) {
        return {};
    }
    if (worker_ == nullptr || !worker_->valid()) {
        throw std::logic_error("cancellable redis operation requires a valid worker");
    }

    auto cancellation = std::make_shared<OperationCancellation>(OperationCancellation{this, index, generation, true});
    auto registration = stopToken.registerCallback([worker = worker_, cancellation] {
        WorkerHandleAccess::deferOrTerminate(*worker, [cancellation] {
            if (cancellation->active) {
                cancellation->pool->cancelOperation(cancellation->index, cancellation->generation);
            }
        });
    });
    if (stopToken.stopRequested()) {
        cancelOperation(index, generation);
    }
    return {std::move(cancellation), std::move(registration)};
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

Task<std::pmr::vector<RedisValue>> RedisPool::executePipeline(std::span<const RedisPipeline::Command> commands, std::pmr::memory_resource* resource) {
    return executePipelineImpl(commands, resource);
}

Task<std::pmr::vector<RedisValue>> RedisPool::executePipeline(std::span<const RedisCommandArgsView> commands, std::pmr::memory_resource* resource) {
    return executePipelineImpl(commands, resource);
}

}  // namespace detail

}  // namespace ruvia
