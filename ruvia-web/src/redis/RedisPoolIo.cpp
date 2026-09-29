#include <hiredis/hiredis.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/TcpSocketOptions.h"
#include "ruvia/web/detail/redis/RedisProtocol.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisUtils.h"
#include "ruvia/web/redis/Redis.h"

namespace ruvia::detail {

struct RedisReaderBudget final {
    enum class Rejection : std::uint8_t { kNone,
        kElements,
        kDepth };

    RedisReaderBudget(redisReader& reader, std::optional<std::size_t> maxReplyBytes,
        std::size_t maxDepth) noexcept
        : functions(*reader.fn),
          originalCreateArray(functions.createArray),
          maximumElements(maxReplyBytes.has_value()
                              ? *maxReplyBytes / 3
                              : std::numeric_limits<std::size_t>::max()),
          remainingElements(maximumElements),
          maxDepth(maxDepth) {
        functions.createArray = &RedisReaderBudget::createArray;
        bind(reader);
    }

    void bind(redisReader& reader) noexcept {
        reader.fn = &functions;
        reader.privdata = this;
    }

    void reset() noexcept {
        remainingElements = maximumElements;
        rejection = Rejection::kNone;
    }

    static void* createArray(const redisReadTask* task, std::size_t elements) noexcept {
        auto& budget = *static_cast<RedisReaderBudget*>(task->privdata);
        if (budget.maxDepth != 0) {
            std::size_t depth = 0;
            for (auto* parent = task; parent != nullptr; parent = parent->parent) {
                if (++depth > budget.maxDepth) {
                    budget.rejection = Rejection::kDepth;
                    return nullptr;
                }
            }
        }
        if (elements > budget.remainingElements) {
            budget.rejection = Rejection::kElements;
            return nullptr;
        }
        auto* reply = budget.originalCreateArray(task, elements);
        if (reply != nullptr) {
            budget.remainingElements -= elements;
        }
        return reply;
    }

    redisReplyObjectFunctions functions;
    void* (*originalCreateArray)(const redisReadTask*, std::size_t);
    std::size_t maximumElements;
    std::size_t remainingElements;
    std::size_t maxDepth;
    Rejection rejection{Rejection::kNone};
};

void RedisReaderBudgetDeleter::operator()(RedisReaderBudget* budget) const noexcept {
    destroyPmrObject(budget, resource);
}

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
    ruvia::applyTcpSocketPolicies(connection.socket, config_.tcpNoDelay, config_.tcpKeepAlive);
}

void RedisPool::ensureReader(Connection& connection) {
    if (connection.reader == nullptr) {
        connection.reader.reset(redisReaderCreate());
        if (connection.reader != nullptr && connection.readerBudget != nullptr) {
            connection.readerBudget->bind(*connection.reader);
        }
    }
    if (connection.reader == nullptr) {
        throw RedisError(RedisError::Code::kProtocolError, "failed to create redis reader");
    }
    if (connection.readerBudget == nullptr) {
        connection.readerBudget.get_deleter().resource = resource_;
        connection.readerBudget.reset(constructPmrObject<RedisReaderBudget>(
            resource_, *connection.reader, config_.maxReplyBytes, config_.maxArrayDepth));
    }
    if (config_.maxReplyBytes.has_value()) {
        // Even a short array header can otherwise reserve billions of reply
        // pointers before the byte budget can inspect the complete reply.
        // Every element requires at least three RESP bytes (e.g. "+\r\n").
        const auto elements = std::max<std::size_t>(1, *config_.maxReplyBytes / 3);
        connection.reader->maxelements = static_cast<long long>(
            std::min(elements, static_cast<std::size_t>(REDIS_READER_MAX_ARRAY_ELEMENTS)));
    }
    connection.readerBudget->reset();
    connection.replyBytes = 0;
}

bool RedisPool::armDeadline(
    Connection& connection, const ruvia::OperationTimeout& timeout, Connection::DeadlineKind kind) {
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
    WorkerHandleAccess::scheduleTimer(worker_, *connection.deadlineTimer, deadline,
        [&connection](WorkerTimerOutcome outcome) noexcept {
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
    return true;
}

bool RedisPool::clearDeadline(Connection& connection) noexcept {
    connection.deadlineTimer->cancel();
    return connection.deadline.clear();
}

Task<void> RedisPool::asyncSocketWrite(
    Connection& connection, const ruvia::OperationTimeout& timeout) {
    if (!armDeadline(connection, timeout, Connection::DeadlineKind::kSocket)) {
        throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
    }
    const auto writeCompletion = co_await ruvia::asyncAsio([&connection](auto handler) mutable {
        asio::async_write(
            connection.socket, asio::buffer(connection.writeBuffer), std::move(handler));
    });
    throwIfAborted(connection);
    const auto ec = writeCompletion.errorCode();
    if (clearDeadline(connection) || timeout.expired() || ec == asio::error::timed_out) {
        throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
    }
    if (ec) {
        throw RedisError(RedisError::Code::kIoError, ec.message());
    }
}

Task<AsioCompletion<std::size_t>> RedisPool::asyncSocketReadSome(
    Connection& connection, std::span<char> buffer, const ruvia::OperationTimeout& timeout) {
    if (!armDeadline(connection, timeout, Connection::DeadlineKind::kSocket)) {
        co_return AsioCompletion<std::size_t>::completed(asio::error::timed_out, 0);
    }
    auto result = co_await ruvia::asyncAsio<std::size_t>([&connection, buffer](auto handler) mutable {
        connection.socket.async_read_some(
            asio::buffer(buffer.data(), buffer.size()), std::move(handler));
    });
    throwIfAborted(connection);
    if (clearDeadline(connection) || timeout.expired()) {
        co_return AsioCompletion<std::size_t>::completed(asio::error::timed_out, 0);
    }
    co_return result;
}

Task<RedisValue> RedisPool::readReply(
    Connection& connection, const ruvia::OperationTimeout& timeout, std::pmr::memory_resource* resource) {
    ensureReader(connection);
    for (;;) {
        const auto unreadBefore = connection.reader->len - connection.reader->pos;
        void* rawReply = nullptr;
        const auto readerStatus = redisReaderGetReply(connection.reader.get(), &rawReply);
        if (readerStatus != REDIS_OK) {
            switch (connection.readerBudget->rejection) {
                case RedisReaderBudget::Rejection::kElements:
                    throw RedisError(RedisError::Code::kProtocolError,
                        "redis reply exceeds configured element limit");
                case RedisReaderBudget::Rejection::kDepth:
                    throw RedisError(RedisError::Code::kProtocolError,
                        "redis array nesting is too deep");
                case RedisReaderBudget::Rejection::kNone:
                    throw RedisError(
                        RedisError::Code::kProtocolError, hiredisReaderError(*connection.reader));
            }
        }
        std::unique_ptr<redisReply, decltype(&freeReplyObject)> reply(
            static_cast<redisReply*>(rawReply), freeReplyObject);
        const auto unreadAfter = connection.reader->len - connection.reader->pos;
        const auto consumed = unreadBefore - unreadAfter;
        // One socket read can contain multiple replies. Only the bytes parsed for
        // this reply count against its limit; if incomplete, any unread bytes
        // still belong to it and must also stay within the limit.
        if (config_.maxReplyBytes.has_value()) {
            const auto limit = *config_.maxReplyBytes;
            if (consumed > limit || connection.replyBytes > limit - consumed ||
                (!reply && unreadAfter >= limit - (connection.replyBytes + consumed))) {
                throw RedisError(
                    RedisError::Code::kProtocolError, "redis reply exceeds configured limit");
            }
        }
        connection.replyBytes += consumed;
        if (reply) {
            connection.replyBytes = 0;
            co_return hiredisReplyToValue(
                *reply, 0, config_.maxArrayDepth, detail::pmrResourceOrDefault(resource));
        }

        auto readCapacity = connection.readBuffer.size();
        if (config_.maxReplyBytes.has_value()) {
            const auto unread = connection.reader->len - connection.reader->pos;
            readCapacity = std::min(readCapacity, *config_.maxReplyBytes - connection.replyBytes - unread);
        }
        auto readCompletion = co_await asyncSocketReadSome(connection,
            std::span<char>(connection.readBuffer.data(), readCapacity), timeout);
        const auto readEc = readCompletion.errorCode();
        const auto bytesRead = readCompletion.result();
        if (readEc) {
            if (readEc == asio::error::timed_out) {
                throw RedisError(RedisError::Code::kTimeout, "redis command timed out");
            }
            throw RedisError(RedisError::Code::kIoError, readEc.message());
        }
        if (redisReaderFeed(connection.reader.get(), connection.readBuffer.data(), bytesRead) !=
            REDIS_OK) {
            throw RedisError(
                RedisError::Code::kProtocolError, hiredisReaderError(*connection.reader));
        }
    }
}

Task<void> RedisPool::connect(Connection& connection, const ruvia::OperationTimeout* operationTimeout) {
    if (connection.connected) {
        co_return;
    }

    std::array<char, 8> portBuffer;
    auto [portEnd, portEc] =
        std::to_chars(portBuffer.data(), portBuffer.data() + portBuffer.size(), config_.port);
    if (portEc != std::errc{}) {
        throw RedisError(RedisError::Code::kConnectFailed, "invalid redis port");
    }
    const auto port =
        std::string_view(portBuffer.data(), static_cast<std::size_t>(portEnd - portBuffer.data()));

    const auto deadline = operationTimeout != nullptr
                              ? operationTimeout->constrainedBy(config_.connectTimeout)
                              : ruvia::OperationTimeout(config_.connectTimeout);
    if (!armDeadline(connection, deadline, Connection::DeadlineKind::kResolve)) {
        throw RedisError(RedisError::Code::kTimeout, "redis resolve timed out");
    }
    auto resolveCompletion = co_await ruvia::asyncAsio<asio::ip::tcp::resolver::results_type>(
        [this, &connection, port](auto handler) mutable {
            connection.resolver.async_resolve(config_.host, port, std::move(handler));
        });
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
    const auto connectCompletion =
        co_await ruvia::asyncAsio([&connection, &endpoints](auto handler) mutable {
            asio::async_connect(connection.socket, endpoints, std::move(handler));
        });
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

Task<void> RedisPool::authenticate(Connection& connection, const ruvia::OperationTimeout& connectTimeout) {
    auto runControl = [this, &connection, &connectTimeout](
                          std::span<const std::string_view> args) -> Task<RedisValue> {
        connection.writeBuffer.clear();
        connection.writeBuffer.reserve(respCommandSerializedSize(args));
        appendRespCommand(connection.writeBuffer, args);
        const auto deadline = connectTimeout.constrainedBy(commandTimeout_);
        co_await asyncSocketWrite(connection, deadline);

        co_return co_await readReply(connection, deadline, resource_);
    };

    if (!config_.password.empty()) {
        RedisValue reply(resource_);
        if (!config_.username.empty()) {
            std::array<std::string_view, 3> args{
                "AUTH", std::string_view(config_.username), std::string_view(config_.password)};
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
