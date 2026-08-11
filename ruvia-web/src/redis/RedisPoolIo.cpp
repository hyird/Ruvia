#include "ruvia/web/redis/Redis.h"

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisProtocol.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>
#include <hiredis/hiredis.h>

#include <array>
#include <charconv>
#include <memory>
#include <system_error>
#include <utility>

namespace ruvia::detail {

void RedisPool::close(Connection& connection) noexcept {
    std::error_code ignored;
    connection.resolver.cancel();
    connection.socket.cancel(ignored);
    connection.socket.close(ignored);
    connection.connected = false;
    (void)clearDeadline(connection);
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

bool RedisPool::armDeadline(Connection& connection, const OperationTimeout& timeout, Connection::DeadlineKind kind) {
    connection.deadlineTimer->cancel();
    const auto remaining = timeout.remaining();
    if (!remaining.has_value()) {
        connection.deadline.reset();
        return true;
    }
    if (remaining->count() == 0) {
        connection.deadline.reset();
        return false;
    }
    const auto deadline = workerTimerDeadlineAfter(*remaining);
    connection.deadline.arm(deadline, kind);
    if (worker_ != nullptr && worker_->valid()) {
        WorkerHandleAccess::scheduleTimer(*worker_, *connection.deadlineTimer, deadline, [&connection](WorkerTimerOutcome outcome) noexcept {
            if (outcome != WorkerTimerOutcome::kExpired) {
                return;
            }
            const auto expiredKind = connection.deadline.expire(std::chrono::steady_clock::now());
            if (!expiredKind.has_value()) {
                return;
            }
            std::error_code ignored;
            if (*expiredKind == Connection::DeadlineKind::kResolve) {
                connection.resolver.cancel();
            } else {
                connection.socket.cancel(ignored);
            }
        });
    }
    return true;
}

bool RedisPool::clearDeadline(Connection& connection) noexcept {
    connection.deadlineTimer->cancel();
    return connection.deadline.clear();
}

Task<std::error_code> RedisPool::asyncSocketWrite(Connection& connection, const OperationTimeout& timeout) {
    if (!armDeadline(connection, timeout, Connection::DeadlineKind::kSocket)) {
        co_return asio::error::timed_out;
    }
    const auto writeCompletion = co_await asyncAsio([&connection](auto handler) mutable { asio::async_write(connection.socket, asio::buffer(connection.writeBuffer), std::move(handler)); });
    throwIfAborted(connection);
    const auto ec = writeCompletion.errorCode();
    if (clearDeadline(connection) || timeout.expired()) {
        co_return asio::error::timed_out;
    }
    co_return ec;
}

Task<AsioCompletion<std::size_t>> RedisPool::asyncSocketReadSome(Connection& connection, std::span<char> buffer, const OperationTimeout& timeout) {
    if (!armDeadline(connection, timeout, Connection::DeadlineKind::kSocket)) {
        co_return AsioCompletion<std::size_t>::completed(asio::error::timed_out, 0);
    }
    auto result = co_await asyncAsio<std::size_t>([&connection, buffer](auto handler) mutable { connection.socket.async_read_some(asio::buffer(buffer.data(), buffer.size()), std::move(handler)); });
    throwIfAborted(connection);
    if (clearDeadline(connection) || timeout.expired()) {
        co_return AsioCompletion<std::size_t>::completed(asio::error::timed_out, 0);
    }
    co_return result;
}

Task<RedisValue> RedisPool::readReply(Connection& connection, const OperationTimeout& timeout, std::pmr::memory_resource* resource) {
    ensureReader(connection);
    for (;;) {
        void* rawReply = nullptr;
        const auto readerStatus = redisReaderGetReply(connection.reader.get(), &rawReply);
        if (readerStatus != REDIS_OK) {
            throw RedisError(RedisError::Code::kProtocolError, hiredisReaderError(*connection.reader));
        }
        if (rawReply != nullptr) {
            std::unique_ptr<redisReply, decltype(&freeReplyObject)> reply(static_cast<redisReply*>(rawReply), freeReplyObject);
            connection.replyBytes = 0;
            co_return hiredisReplyToValue(*reply, 0, config_.maxArrayDepth, detail::pmrResourceOrDefault(resource));
        }

        auto readCompletion = co_await asyncSocketReadSome(connection, std::span<char>(connection.readBuffer), timeout);
        const auto readEc = readCompletion.errorCode();
        const auto bytesRead = readCompletion.result();
        if (readEc) {
            if (readEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, readEc.message());
        }
        if (config_.maxReplyBytes.has_value() && (bytesRead > *config_.maxReplyBytes || connection.replyBytes > *config_.maxReplyBytes - bytesRead)) {
            throw RedisError(RedisError::Code::kProtocolError, "redis reply exceeds configured limit");
        }
        connection.replyBytes += bytesRead;
        if (redisReaderFeed(connection.reader.get(), connection.readBuffer.data(), bytesRead) != REDIS_OK) {
            throw RedisError(RedisError::Code::kProtocolError, hiredisReaderError(*connection.reader));
        }
    }
}

Task<void> RedisPool::connect(Connection& connection, const OperationTimeout* operationTimeout) {
    if (connection.connected) {
        co_return;
    }

    std::array<char, 8> portBuffer;
    auto [portEnd, portEc] = std::to_chars(portBuffer.data(), portBuffer.data() + portBuffer.size(), config_.port);
    if (portEc != std::errc{}) {
        throw RedisError(RedisError::Code::kConnectFailed, "invalid redis port");
    }
    const auto port = std::string_view(portBuffer.data(), static_cast<std::size_t>(portEnd - portBuffer.data()));

    const auto deadline = operationTimeout != nullptr
        ? operationTimeout->constrainedBy(config_.connectTimeout)
        : OperationTimeout(config_.connectTimeout);
    if (!armDeadline(connection, deadline, Connection::DeadlineKind::kResolve)) {
        throw RedisError(RedisError::Code::kTimeout, "redis resolve timed out");
    }
    auto resolveCompletion = co_await asyncAsio<asio::ip::tcp::resolver::results_type>([this, &connection, port](auto handler) mutable { connection.resolver.async_resolve(config_.host, port, std::move(handler)); });
    throwIfAborted(connection);
    const auto resolveEc = resolveCompletion.errorCode();
    auto endpoints = std::move(resolveCompletion).takeResult();
    if (clearDeadline(connection) || deadline.expired()) {
        throw RedisError(RedisError::Code::kTimeout, "redis resolve timed out");
    }
    if (resolveEc) {
        throw RedisError(RedisError::Code::kConnectFailed, resolveEc.message());
    }

    if (!armDeadline(connection, deadline, Connection::DeadlineKind::kSocket)) {
        throw RedisError(RedisError::Code::kTimeout, "redis connect timed out");
    }
    const auto connectCompletion = co_await asyncAsio([&connection, &endpoints](auto handler) mutable { asio::async_connect(connection.socket, endpoints, std::move(handler)); });
    throwIfAborted(connection);
    const auto connectEc = connectCompletion.errorCode();
    if (clearDeadline(connection) || deadline.expired()) {
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
        co_await authenticate(connection, deadline);
    } catch (...) {
        close(connection);
        throw;
    }
}

Task<void> RedisPool::authenticate(Connection& connection, const OperationTimeout& connectTimeout) {
    auto runControl = [this, &connection, &connectTimeout](std::span<const std::string_view> args) -> Task<RedisValue> {
        connection.writeBuffer.clear();
        connection.writeBuffer.reserve(respCommandSerializedSize(args));
        appendRespCommand(connection.writeBuffer, args);
        const auto deadline = connectTimeout.constrainedBy(config_.commandTimeout);
        const auto writeEc = co_await asyncSocketWrite(connection, deadline);
        if (writeEc) {
            if (writeEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, writeEc.message());
        }

        co_return co_await readReply(connection, deadline, resource_);
    };

    if (!config_.password.empty()) {
        RedisValue reply(resource_);
        if (!config_.username.empty()) {
            std::array<std::string_view, 3> args{"AUTH", std::string_view(config_.username), std::string_view(config_.password)};
            reply = co_await runControl(args);
        } else {
            std::array<std::string_view, 2> args{"AUTH", std::string_view(config_.password)};
            reply = co_await runControl(args);
        }
        if (reply.kind() == RedisValue::Kind::kError) {
            throw RedisError(RedisError::Code::kAuthFailed, reply.error());
        }
    }

    if (config_.database != 0) {
        std::pmr::string db(resource_);
        detail::appendRedisNumber(db, static_cast<std::uint64_t>(config_.database));
        std::array<std::string_view, 2> args{"SELECT", std::string_view(db)};
        auto reply = co_await runControl(args);
        if (reply.kind() == RedisValue::Kind::kError) {
            throw RedisError(RedisError::Code::kCommandError, reply.error());
        }
    }
}

}  // namespace ruvia::detail
