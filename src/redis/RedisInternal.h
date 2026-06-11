#pragma once

#include "ruvia/redis/Redis.h"

#ifndef RUVIA_ENABLE_REDIS

#include <asio/io_context.hpp>

#include <memory_resource>
#include <span>

namespace ruvia::detail {

class RedisRegistry final {
public:
    RedisRegistry(
        asio::io_context&,
        std::pmr::memory_resource*,
        std::span<const RedisDefinition>) {}

    RedisRegistry(const RedisRegistry&) = delete;
    RedisRegistry& operator=(const RedisRegistry&) = delete;

    [[nodiscard]] Task<void> connect() {
        co_return;
    }

    void closeNow() noexcept {}
    [[nodiscard]] bool empty() const noexcept { return true; }
    [[nodiscard]] bool hasAnyTimeout() const noexcept { return false; }
    void scanDeadlines() noexcept {}
};

}  // namespace ruvia::detail

#else

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

struct redisReader;

namespace ruvia::detail {

struct RedisReaderDeleter final {
    void operator()(redisReader* reader) const noexcept;
};

class RedisPool final {
public:
    RedisPool(
        asio::io_context& ioContext,
        RedisConfig config,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~RedisPool();

    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    Task<void> connect();
    void closeNow() noexcept;
    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;
    Task<RedisValue> execute(std::span<const std::string_view> args, std::pmr::memory_resource* resource);
    Task<RedisValue> executeWithTimeout(
        std::span<const std::string_view> args,
        std::chrono::milliseconds timeout,
        std::pmr::memory_resource* resource);
    Task<std::pmr::vector<RedisValue>> executePipeline(
        std::span<const RedisPipeline::Command> commands,
        std::pmr::memory_resource* resource);

private:
    friend class ::ruvia::RedisHandle;
    friend class ::ruvia::RedisPipeline;

    struct PoolWaiter {
        bool* ready{nullptr};
        bool* timedOut{nullptr};
        std::size_t* index{nullptr};
        std::chrono::steady_clock::time_point deadline{};
        std::coroutine_handle<> handle{};
        PoolWaiter* previous{nullptr};
        PoolWaiter* next{nullptr};
        bool queued{false};
    };

    struct Connection final {
        explicit Connection(asio::io_context& ioContext, std::pmr::memory_resource* resource);
        ~Connection();

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) noexcept;
        Connection& operator=(Connection&&) noexcept;

        asio::ip::tcp::socket socket;
        asio::ip::tcp::resolver resolver;
        std::pmr::string writeBuffer;
        std::unique_ptr<redisReader, RedisReaderDeleter> reader;
        std::size_t replyBytes{0};
        std::chrono::steady_clock::time_point deadline{};
        bool busy{false};
        bool connected{false};
        bool deadlineActive{false};
        bool timedOut{false};
        enum class DeadlineKind : std::uint8_t {
            kNone,
            kResolve,
            kSocket
        };
        DeadlineKind deadlineKind{DeadlineKind::kNone};
    };

    class ConnectionGuard final {
    public:
        ConnectionGuard(RedisPool& pool, std::size_t index) noexcept;
        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;
        ~ConnectionGuard();

        [[nodiscard]] Connection& connection() noexcept;
        void discard() noexcept;

    private:
        RedisPool* pool_{nullptr};
        std::size_t index_{0};
        bool discard_{false};
    };

    Task<std::size_t> acquire();
    void release(std::size_t index) noexcept;
    void enqueueWaiter(PoolWaiter& waiter) noexcept;
    void removeWaiter(PoolWaiter& waiter) noexcept;
    [[nodiscard]] bool resumeNextWaiter(std::size_t index) noexcept;
    void close(Connection& connection) noexcept;
    void configureSocket(Connection& connection) noexcept;
    void ensureReader(Connection& connection);
    void setDeadline(Connection& connection, std::chrono::milliseconds timeout, Connection::DeadlineKind kind) noexcept;
    void clearDeadline(Connection& connection) noexcept;
    Task<void> connect(Connection& connection);
    Task<void> authenticate(Connection& connection);
    Task<RedisValue> readReply(Connection& connection, std::chrono::milliseconds timeout, std::pmr::memory_resource* resource);
    Task<std::error_code> asyncSocketWrite(Connection& connection, std::chrono::milliseconds timeout);
    Task<std::pair<std::error_code, std::size_t>> asyncSocketReadSome(
        Connection& connection,
        std::span<char> buffer,
        std::chrono::milliseconds timeout);
    asio::io_context& ioContext_;
    RedisConfig config_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<Connection> connections_;
    std::pmr::vector<std::size_t> free_;
    PoolWaiter* waiterHead_{nullptr};
    PoolWaiter* waiterTail_{nullptr};
    bool closing_{false};
};

class RedisRegistry final {
public:
    RedisRegistry(
        asio::io_context& ioContext,
        std::pmr::memory_resource* resource,
        std::span<const RedisDefinition> redis);
    ~RedisRegistry();

    RedisRegistry(const RedisRegistry&) = delete;
    RedisRegistry& operator=(const RedisRegistry&) = delete;

    Task<void> connect();
    void closeNow() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;
    [[nodiscard]] RedisHandle get(std::pmr::memory_resource* resource, RequestMemory* requestMemory = nullptr) const;
    [[nodiscard]] RedisHandle get(
        std::string_view alias,
        std::pmr::memory_resource* resource,
        RequestMemory* requestMemory = nullptr) const;
    void scanDeadlines() noexcept;

private:
    struct Entry final {
        std::pmr::string alias;
        std::unique_ptr<RedisPool> pool;
    };

    std::pmr::memory_resource* resource_;
    std::pmr::vector<Entry> pools_;
    RedisPool* defaultPool_{nullptr};
};

}  // namespace ruvia::detail

#endif
