#include "ruvia/redis/Redis.h"

#include "../../AsioAwait.h"
#include "../RedisInternal.h"
#include "RedisProtocol.h"
#include "RedisUtils.h"

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>
#include <hiredis/hiredis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <system_error>

namespace ruvia {
namespace detail {

void RedisReaderDeleter::operator()(redisReader* reader) const noexcept {
    if (reader != nullptr) {
        redisReaderFree(reader);
    }
}

RedisPool::Connection::Connection(asio::io_context& ioContext, std::pmr::memory_resource* resource)
    : socket(ioContext),
      resolver(ioContext),
      writeBuffer(detail::resolveRedisResource(resource)),
      reader(redisReaderCreate()) {}

RedisPool::Connection::~Connection() = default;

RedisPool::Connection::Connection(Connection&&) noexcept = default;
RedisPool::Connection& RedisPool::Connection::operator=(Connection&&) noexcept = default;

RedisPool::ConnectionGuard::ConnectionGuard(RedisPool& pool, std::size_t index) noexcept
    : pool_(&pool),
      index_(index) {}

RedisPool::ConnectionGuard::~ConnectionGuard() {
    if (pool_ == nullptr) {
        return;
    }
    if (discard_) {
        pool_->close(pool_->connections_[index_]);
    }
    pool_->release(index_);
}

RedisPool::Connection& RedisPool::ConnectionGuard::connection() noexcept {
    return pool_->connections_[index_];
}

void RedisPool::ConnectionGuard::discard() noexcept {
    discard_ = true;
}

RedisPool::RedisPool(asio::io_context& ioContext, RedisConfig config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(detail::resolveRedisResource(resource)),
      connections_(resource_),
      free_(resource_) {
    const auto poolSize = std::max<std::size_t>(1, config_.poolSizePerWorker);
    connections_.reserve(poolSize);
    free_.reserve(poolSize);
    for (std::size_t i = 0; i < poolSize; ++i) {
        connections_.emplace_back(ioContext_, resource_);
        free_.push_back(i);
    }
}

RedisPool::~RedisPool() {
    closeNow();
}

Task<void> RedisPool::connect() {
    for (auto& connection : connections_) {
        if (!connection.connected) {
            co_await connect(connection);
        }
    }
    co_return;
}

void RedisPool::closeNow() noexcept {
    closing_ = true;
    while (waiterHead_ != nullptr) {
        auto* waiter = waiterHead_;
        removeWaiter(*waiter);
        if (waiter->ready != nullptr) {
            *waiter->ready = true;
        }
        if (waiter->handle) {
            waiter->handle.resume();
        }
    }
    for (auto& connection : connections_) {
        close(connection);
    }
}

void RedisPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    auto* waiter = waiterHead_;
    while (waiter != nullptr) {
        auto* next = waiter->next;
        if (config_.acquireTimeout.count() > 0 && waiter->deadline <= now) {
            removeWaiter(*waiter);
            if (waiter->timedOut != nullptr) {
                *waiter->timedOut = true;
            }
            if (waiter->ready != nullptr) {
                *waiter->ready = true;
            }
            if (waiter->handle) {
                waiter->handle.resume();
            }
        }
        waiter = next;
    }

    for (auto& connection : connections_) {
        if (!connection.deadlineActive || connection.deadline > now) {
            continue;
        }
        connection.timedOut = true;
        std::error_code ignored;
        if (connection.deadlineKind == Connection::DeadlineKind::kResolve) {
            connection.resolver.cancel();
        } else if (connection.deadlineKind == Connection::DeadlineKind::kSocket) {
            connection.socket.cancel(ignored);
        }
    }
}

bool RedisPool::hasAnyTimeout() const noexcept {
    return config_.connectTimeout.count() > 0 ||
        config_.commandTimeout.count() > 0 ||
        config_.acquireTimeout.count() > 0;
}

Task<std::size_t> RedisPool::acquire() {
    if (closing_) {
        throw RedisError(RedisError::Code::kIoError, "redis pool is closing");
    }
    if (!free_.empty()) {
        const auto index = free_.back();
        free_.pop_back();
        connections_[index].busy = true;
        co_return index;
    }

    struct WaiterGuard final {
        RedisPool& pool;
        PoolWaiter& waiter;

        ~WaiterGuard() {
            pool.removeWaiter(waiter);
        }
    };

    bool ready = false;
    bool timedOut = false;
    std::size_t waitedIndex = 0;
    PoolWaiter waiter{
        .ready = &ready,
        .timedOut = &timedOut,
        .index = &waitedIndex,
        .deadline = std::chrono::steady_clock::now() + config_.acquireTimeout};
    enqueueWaiter(waiter);
    WaiterGuard guard{*this, waiter};

    struct WaiterAwaiter final {
        PoolWaiter& waiter;
        bool& ready;

        [[nodiscard]] bool await_ready() const noexcept {
            return ready;
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            waiter.handle = handle;
        }

        void await_resume() const noexcept {}
    };

    co_await WaiterAwaiter{waiter, ready};

    if (timedOut) {
        throw RedisError(RedisError::Code::kTimeout, "redis connection pool acquire timed out");
    }

    if (closing_ || waitedIndex >= connections_.size()) {
        throw RedisError(RedisError::Code::kIoError, "redis pool is closing");
    }

    co_return waitedIndex;
}

void RedisPool::release(std::size_t index) noexcept {
    if (index >= connections_.size()) {
        return;
    }
    if (closing_) {
        connections_[index].busy = false;
        return;
    }
    if (resumeNextWaiter(index)) {
        connections_[index].busy = true;
        return;
    }
    connections_[index].busy = false;
    free_.push_back(index);
}

void RedisPool::enqueueWaiter(PoolWaiter& waiter) noexcept {
    if (waiter.queued) {
        return;
    }
    waiter.previous = waiterTail_;
    waiter.next = nullptr;
    waiter.queued = true;
    if (waiterTail_ != nullptr) {
        waiterTail_->next = &waiter;
    } else {
        waiterHead_ = &waiter;
    }
    waiterTail_ = &waiter;
}

void RedisPool::removeWaiter(PoolWaiter& waiter) noexcept {
    if (!waiter.queued) {
        return;
    }
    if (waiter.previous != nullptr) {
        waiter.previous->next = waiter.next;
    } else {
        waiterHead_ = waiter.next;
    }
    if (waiter.next != nullptr) {
        waiter.next->previous = waiter.previous;
    } else {
        waiterTail_ = waiter.previous;
    }
    waiter.previous = nullptr;
    waiter.next = nullptr;
    waiter.queued = false;
}

bool RedisPool::resumeNextWaiter(std::size_t index) noexcept {
    while (waiterHead_ != nullptr) {
        auto* waiter = waiterHead_;
        removeWaiter(*waiter);
        if (waiter->ready != nullptr && waiter->index != nullptr) {
            *waiter->index = index;
            *waiter->ready = true;
            if (waiter->handle) {
                waiter->handle.resume();
            }
            return true;
        }
    }
    return false;
}

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

void RedisPool::setDeadline(Connection& connection, std::chrono::milliseconds timeout, Connection::DeadlineKind kind) noexcept {
    connection.deadlineKind = kind;
    connection.timedOut = false;
    if (timeout.count() <= 0) {
        connection.deadlineActive = false;
        return;
    }
    connection.deadline = std::chrono::steady_clock::now() + timeout;
    connection.deadlineActive = true;
}

void RedisPool::clearDeadline(Connection& connection) noexcept {
    connection.deadlineActive = false;
    connection.deadlineKind = Connection::DeadlineKind::kNone;
}

Task<std::error_code> RedisPool::asyncSocketWrite(Connection& connection, std::chrono::milliseconds timeout) {
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
    std::chrono::milliseconds timeout) {
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

Task<RedisValue> RedisPool::readReply(Connection& connection, std::chrono::milliseconds timeout, std::pmr::memory_resource* resource) {
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
            co_return hiredisReplyToValue(*reply, 0, config_.maxArrayDepth, detail::resolveRedisResource(resource));
        }

        std::array<char, 8192> buffer{};
        const auto [readEc, bytesRead] = co_await asyncSocketReadSome(connection, buffer, timeout);
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
        if (redisReaderFeed(connection.reader.get(), buffer.data(), bytesRead) != REDIS_OK) {
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

    std::array<char, 8> portBuffer{};
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
        appendRespCommand(connection.writeBuffer, args);
        const auto timeout = config_.commandTimeout.count() > 0 ? config_.commandTimeout : config_.connectTimeout;
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

Task<RedisValue> RedisPool::execute(std::span<const std::string_view> args, std::pmr::memory_resource* resource) {
    co_return co_await executeWithTimeout(args, config_.commandTimeout, resource);
}

Task<RedisValue> RedisPool::executeWithTimeout(
    std::span<const std::string_view> args,
    std::chrono::milliseconds timeout,
    std::pmr::memory_resource* resource) {
    const auto index = co_await acquire();
    ConnectionGuard guard(*this, index);
    auto& connection = guard.connection();
    try {
        if (!connection.connected) {
            co_await connect(connection);
        }

        connection.writeBuffer.clear();
        appendRespCommand(connection.writeBuffer, args);
        const auto writeEc = co_await asyncSocketWrite(connection, timeout);
        if (writeEc) {
            if (writeEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, writeEc.message());
        }

        co_return co_await readReply(connection, timeout, resource);
    } catch (...) {
        guard.discard();
        throw;
    }
}

Task<std::pmr::vector<RedisValue>> RedisPool::executePipeline(
    std::span<const RedisPipeline::Command> commands,
    std::pmr::memory_resource* resource) {
    const auto resolved = detail::resolveRedisResource(resource);
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
        for (const auto& command : commands) {
            auto views = detail::viewRedisArgs(command.args, resource_);
            appendRespCommand(connection.writeBuffer, std::span<const std::string_view>(views.data(), views.size()));
        }

        const auto writeEc = co_await asyncSocketWrite(connection, config_.commandTimeout);
        if (writeEc) {
            if (writeEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, writeEc.message());
        }

        while (replies.size() < commands.size()) {
            replies.emplace_back(co_await readReply(connection, config_.commandTimeout, resolved));
        }

        co_return replies;
    } catch (...) {
        guard.discard();
        throw;
    }
}

}  // namespace detail

}  // namespace ruvia
