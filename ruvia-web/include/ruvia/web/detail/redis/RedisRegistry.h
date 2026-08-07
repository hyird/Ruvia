#pragma once

#include "ruvia/web/redis/Redis.h"
#include "ruvia/web/detail/redis/RedisConfigStorage.h"
#include "ruvia/core/WorkerHandle.h"

#ifndef RUVIA_ENABLE_REDIS

#include <asio/io_context.hpp>

#include <memory_resource>
#include <span>

namespace ruvia::detail {

class RedisRegistry final {
public:
    RedisRegistry(asio::io_context&, std::pmr::memory_resource*, std::span<const RedisDefinition>, const WorkerHandle* = nullptr) {}

    RedisRegistry(const RedisRegistry&) = delete;
    RedisRegistry& operator=(const RedisRegistry&) = delete;

    [[nodiscard]] Task<void> connect() {
        co_return;
    }

    void closeNow() noexcept {}
    [[nodiscard]] bool empty() const noexcept {
        return true;
    }
    [[nodiscard]] bool needsDeadlineScan() const noexcept {
        return false;
    }
    void scanDeadlines() noexcept {}
};

}  // namespace ruvia::detail

#else

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/detail/io/OperationDeadline.h"
#include "ruvia/core/detail/pool/PoolLeaseScheduler.h"
#include "ruvia/core/memory/PmrObject.h"

struct redisReader;

namespace ruvia::detail {

template <typename Result>
class AsioCompletion;

struct RedisReaderDeleter final {
    void operator()(redisReader* reader) const noexcept;
};

struct RedisCommandArgsView final {
    std::span<const std::pmr::string> args;
};

inline constexpr std::size_t kRedisReadBufferBytes = 8192;

class RedisPool final {
public:
    RedisPool(asio::io_context& ioContext, RedisConfigStorage config, std::size_t poolSize, std::pmr::memory_resource* resource = nullptr, const WorkerHandle* worker = nullptr);
    ~RedisPool();

    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    Task<void> connect();
    void closeNow() noexcept;
    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool needsDeadlineScan() const noexcept;
    Task<RedisValue> executeOwned(std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource, RedisOperationOptions options = {});
    Task<std::pmr::vector<RedisValue>> executePipeline(std::span<const RedisPipeline::Command> commands, RedisOperationOptions options, std::pmr::memory_resource* resource);
    Task<std::pmr::vector<RedisValue>> executePipeline(std::span<const RedisCommandArgsView> commands, RedisOperationOptions options, std::pmr::memory_resource* resource);

private:
    friend class ::ruvia::RedisHandle;
    friend class ::ruvia::RedisPipeline;

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
        std::array<char, kRedisReadBufferBytes> readBuffer;
        std::unique_ptr<redisReader, RedisReaderDeleter> reader;
        std::size_t replyBytes{0};
        bool connected{false};
        enum class AbortReason : std::uint8_t { kNone, kCancelled };
        AbortReason abortReason{AbortReason::kNone};
        std::uint64_t operationGeneration{0};
        enum class DeadlineKind : std::uint8_t { kResolve, kSocket };
        OperationDeadline<DeadlineKind> deadline;
        std::unique_ptr<WorkerTimerRegistration, PmrObjectDeleter<WorkerTimerRegistration>> deadlineTimer;
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
        RedisPool& pool_;
        std::size_t index_{0};
        bool discard_{false};
    };

    Task<std::size_t> acquire(const OperationTimeout& timeout, StopToken stopToken);
    void release(std::size_t index) noexcept;
    void close(Connection& connection) noexcept;
    void configureSocket(Connection& connection) noexcept;
    void ensureReader(Connection& connection);
    [[nodiscard]] bool armDeadline(Connection& connection, const OperationTimeout& timeout, Connection::DeadlineKind kind);
    [[nodiscard]] bool clearDeadline(Connection& connection) noexcept;
    Task<void> connect(Connection& connection, const OperationTimeout* operationTimeout = nullptr);
    Task<void> authenticate(Connection& connection, const OperationTimeout& connectTimeout);
    Task<RedisValue> readReply(Connection& connection, const OperationTimeout& timeout, std::pmr::memory_resource* resource);
    template <typename ArgSource>
    Task<RedisValue> executeWithTimeoutImpl(ArgSource args, RedisOperationOptions options, std::pmr::memory_resource* resource);
    template <typename CommandSource>
    Task<std::pmr::vector<RedisValue>> executePipelineImpl(CommandSource commands, RedisOperationOptions options, std::pmr::memory_resource* resource);
    Task<std::error_code> asyncSocketWrite(Connection& connection, const OperationTimeout& timeout);
    Task<AsioCompletion<std::size_t>> asyncSocketReadSome(Connection& connection, std::span<char> buffer, const OperationTimeout& timeout);
    void cancelOperation(std::size_t index, std::uint64_t generation) noexcept;
    void throwIfCancelled(const Connection& connection) const;
    asio::io_context& ioContext_;
    const WorkerHandle* worker_;
    RedisConfigStorage config_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<Connection> connections_;
    PoolLeaseScheduler scheduler_;
};

struct RedisCommandExecutor final {
    RedisPool* pool{nullptr};
    RedisOperationOptions options;
};

class RedisRegistry final {
public:
    RedisRegistry(asio::io_context& ioContext, std::pmr::memory_resource* resource, std::span<const RedisDefinition> redis, const WorkerHandle* worker = nullptr);
    ~RedisRegistry();

    RedisRegistry(const RedisRegistry&) = delete;
    RedisRegistry& operator=(const RedisRegistry&) = delete;

    Task<void> connect();
    void closeNow() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool needsDeadlineScan() const noexcept;
    [[nodiscard]] RedisHandle get(std::pmr::memory_resource* resource, ScopedOperationScope& operationScope) const;
    [[nodiscard]] RedisHandle get(std::string_view alias, std::pmr::memory_resource* resource, ScopedOperationScope& operationScope) const;
    void scanDeadlines() noexcept;

private:
    using RedisPoolDeleter = PmrObjectDeleter<RedisPool>;

    struct Entry final {
        std::pmr::string alias;
        std::unique_ptr<RedisPool, RedisPoolDeleter> general;
        std::unique_ptr<RedisPool, RedisPoolDeleter> blocking;
    };

    std::pmr::memory_resource* resource_;
    std::pmr::vector<Entry> pools_;
    std::pmr::vector<std::size_t> aliasIndex_;
    std::optional<std::size_t> defaultPoolIndex_;
};

}  // namespace ruvia::detail

#endif
