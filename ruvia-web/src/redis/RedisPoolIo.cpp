#include "ruvia/web/redis/Redis.h"

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/web/detail/redis/RedisInternal.h"
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

void RedisPool::setDeadline(
    Connection& connection,
    std::optional<std::chrono::milliseconds> timeout,
    Connection::DeadlineKind kind) noexcept {
    connection.deadlineKind = kind;
    connection.timedOut = false;
    if (!timeout.has_value()) {
        connection.deadlineActive = false;
        return;
    }
    connection.deadline = std::chrono::steady_clock::now() + *timeout;
    connection.deadlineActive = true;
}

void RedisPool::clearDeadline(Connection& connection) noexcept {
    connection.deadlineActive = false;
    connection.deadlineKind = Connection::DeadlineKind::kNone;
}

Task<std::error_code> RedisPool::asyncSocketWrite(
    Connection& connection,
    std::optional<std::chrono::milliseconds> timeout) {
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
    std::optional<std::chrono::milliseconds> timeout) {
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

Task<RedisValue> RedisPool::readReply(
    Connection& connection,
    std::optional<std::chrono::milliseconds> timeout,
    std::pmr::memory_resource* resource) {
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
            co_return hiredisReplyToValue(*reply, 0, config_.maxArrayDepth, detail::pmrResourceOrDefault(resource));
        }

        const auto [readEc, bytesRead] = co_await asyncSocketReadSome(
            connection,
            std::span<char>(connection.readBuffer.data(), connection.readBuffer.size()),
            timeout);
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
        if (redisReaderFeed(connection.reader.get(), connection.readBuffer.data(), bytesRead) != REDIS_OK) {
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

    std::array<char, 8> portBuffer;
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
        connection.writeBuffer.reserve(respCommandSerializedSize(args));
        appendRespCommand(connection.writeBuffer, args);
        const auto timeout = config_.commandTimeout.has_value()
            ? config_.commandTimeout
            : config_.connectTimeout;
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
        detail::appendRedisNumber(db, static_cast<std::uint64_t>(config_.database));
        std::array<std::string_view, 2> args{"SELECT", std::string_view(db)};
        auto reply = co_await runControl(args);
        if (reply.kind() == RedisValue::Kind::kError) {
            throw RedisError(RedisError::Code::kCommandError, reply.string());
        }
    }
}

}  // namespace ruvia::detail
