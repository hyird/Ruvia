#pragma once

#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/web/detail/integration/NamedCapability.h"
#include "ruvia/web/detail/redis/RedisConfigStorage.h"
#include "ruvia/web/detail/redis/RedisOwnedCommand.h"
#include "ruvia/web/redis/RedisHandle.h"

#ifndef RUVIA_ENABLE_REDIS

#include <memory_resource>
#include <span>
#include <stdexcept>

#include <asio/io_context.hpp>

namespace ruvia::detail {

class RedisRegistry final {
public:
    RedisRegistry(asio::io_context&, std::pmr::memory_resource*, std::span<const RedisDefinition>,
        WorkerHandle worker) {
        if (!worker.valid()) {
            throw std::invalid_argument("redis registry requires a valid worker");
        }
    }

    RedisRegistry(const RedisRegistry&) = delete;
    RedisRegistry& operator=(const RedisRegistry&) = delete;

    [[nodiscard]] Task<void> connect() {
        co_return;
    }

    void closeNow() noexcept {}
    [[nodiscard]] bool empty() const noexcept {
        return true;
    }
};

}  // namespace ruvia::detail

#else

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

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/OperationTimeout.h"
#include "ruvia/core/PoolLeaseScheduler.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/detail/redis/RedisClientRuntime.h"

struct redisReader;

namespace ruvia::detail {

template <typename Result>
class AsioCompletion;

struct RedisReaderDeleter {
    void operator()(redisReader* reader) const noexcept;
};

struct RedisReaderBudget;
struct RedisReaderBudgetDeleter final {
    std::pmr::memory_resource* resource{nullptr};
    void operator()(RedisReaderBudget* budget) const noexcept;
};

class RedisPool;
using redis_cancellation_target = worker_cancellation_target<RedisPool>;

struct RedisCommandArgsView final {
    std::span<const std::pmr::string> args;
};

inline constexpr std::size_t kRedisReadBufferBytes = 8192;

class RedisPool final {
public:
    RedisPool(asio::io_context& ioContext, const RedisConfigStorage& config,
        std::optional<std::chrono::milliseconds> commandTimeout, std::size_t poolSize,
        const WorkerHandle& worker, std::pmr::memory_resource* resource = nullptr);
    RedisPool(asio::io_context&, RedisConfigStorage&&, std::optional<std::chrono::milliseconds>,
        std::size_t, const WorkerHandle&, std::pmr::memory_resource* = nullptr) = delete;
    RedisPool(asio::io_context&, const RedisConfigStorage&&,
        std::optional<std::chrono::milliseconds>, std::size_t, const WorkerHandle&,
        std::pmr::memory_resource* = nullptr) = delete;
    RedisPool(asio::io_context&, const RedisConfigStorage&,
        std::optional<std::chrono::milliseconds>, std::size_t, WorkerHandle&&,
        std::pmr::memory_resource* = nullptr) = delete;
    RedisPool(asio::io_context&, const RedisConfigStorage&,
        std::optional<std::chrono::milliseconds>, std::size_t, const WorkerHandle&&,
        std::pmr::memory_resource* = nullptr) = delete;
    ~RedisPool();

    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    Task<void> connect();
    void closeNow() noexcept;
    Task<RedisValue> executeOwned(std::pmr::vector<std::pmr::string> args,
        std::pmr::memory_resource* resource, OperationOptions options = {});
    Task<std::pmr::vector<RedisValue>> executePipeline(
        std::span<const redis_owned_command> commands, OperationOptions options,
        std::pmr::memory_resource* resource);
    Task<std::pmr::vector<RedisValue>> executePipeline(
        std::span<const RedisCommandArgsView> commands, OperationOptions options,
        std::pmr::memory_resource* resource);

private:
    friend class ::ruvia::RedisHandle;
    friend class worker_cancellation_target<RedisPool>;

    struct Connection final {
        explicit Connection(asio::io_context& ioContext, std::pmr::memory_resource* resource);
        ~Connection();

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) noexcept;
        Connection& operator=(Connection&&) noexcept;

        asio::ip::tcp::socket socket;
        // Pool construction reserves all slots before any TLS stream exists.
        // Once connect() starts, slots never move: TLS and pending I/O borrow
        // this socket until their owning connection is retired.
        using tls_stream_type = asio::ssl::stream<asio::ip::tcp::socket&>;
        std::unique_ptr<tls_stream_type, PmrObjectDeleter<tls_stream_type>> tls_stream;
        asio::ip::tcp::resolver resolver;
        std::pmr::string writeBuffer;
        std::array<char, kRedisReadBufferBytes> readBuffer;
        std::unique_ptr<RedisReaderBudget, RedisReaderBudgetDeleter> readerBudget;
        std::unique_ptr<redisReader, RedisReaderDeleter> reader;
        std::size_t replyBytes{0};
        bool connected{false};
        enum class AbortReason : std::uint8_t { kNone,
            kCancelled,
            kClosing };
        AbortReason abortReason{AbortReason::kNone};
        std::uint64_t cancellationId{0};
        enum class DeadlineKind : std::uint8_t { kResolve,
            kSocket };
        OperationDeadline<DeadlineKind> deadline;
        std::unique_ptr<WorkerTimerRegistration, PmrObjectDeleter<WorkerTimerRegistration>>
            deadlineTimer;
    };

    class ConnectionGuard final {
    public:
        ConnectionGuard(RedisPool& pool, std::size_t index, const StopToken& stopToken);
        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;
        ~ConnectionGuard();

        [[nodiscard]] Connection& connection() noexcept;
        void discard() noexcept;

    private:
        RedisPool& pool_;
        std::size_t index_{0};
        worker_cancellation_registration<redis_cancellation_target> cancellation_;
        bool discard_{false};
    };

    Task<std::size_t> acquire(const ruvia::OperationTimeout& timeout, StopToken stopToken);
    void release(std::size_t index) noexcept;
    void close(Connection& connection) noexcept;
    void configureSocket(Connection& connection) noexcept;
    void ensureReader(Connection& connection);
    [[nodiscard]] bool armDeadline(
        Connection& connection, const ruvia::OperationTimeout& timeout, Connection::DeadlineKind kind);
    [[nodiscard]] bool clearDeadline(Connection& connection) noexcept;
    Task<void> connect(Connection& connection, const ruvia::OperationTimeout* operationTimeout = nullptr);
    Task<void> authenticate(Connection& connection, const ruvia::OperationTimeout& connectTimeout);
    Task<RedisValue> readReply(Connection& connection, const ruvia::OperationTimeout& timeout,
        std::pmr::memory_resource* resource);
    template <typename ArgSource>
    Task<RedisValue> executeWithTimeoutImpl(
        ArgSource args, OperationOptions options, std::pmr::memory_resource* resource);
    template <typename CommandSource>
    Task<std::pmr::vector<RedisValue>> executePipelineImpl(
        CommandSource commands, OperationOptions options, std::pmr::memory_resource* resource);
    Task<void> asyncSocketWrite(Connection& connection, const ruvia::OperationTimeout& timeout);
    Task<AsioCompletion<std::size_t>> asyncSocketReadSome(
        Connection& connection, std::span<char> buffer, const ruvia::OperationTimeout& timeout);
    void cancelOperationById(std::uint64_t cancellationId) noexcept;
    void throwIfAborted(const Connection& connection) const;
    asio::io_context& ioContext_;
    const WorkerHandle& worker_;
    const RedisConfigStorage& config_;
    std::optional<std::chrono::milliseconds> commandTimeout_;
    std::pmr::memory_resource* resource_;
    std::optional<asio::ssl::context> tls_context_;
    std::pmr::vector<Connection> connections_;
    PoolLeaseScheduler scheduler_;
    std::shared_ptr<redis_cancellation_target> cancellation_target_;
};

struct RedisCommandExecutor final {
    RedisPool* pool{nullptr};
    OperationOptions options;
};

class RedisRegistry final {
public:
    RedisRegistry(asio::io_context& ioContext, std::pmr::memory_resource* resource,
        std::span<const RedisDefinition> redis, WorkerHandle worker);
    ~RedisRegistry();

    RedisRegistry(const RedisRegistry&) = delete;
    RedisRegistry& operator=(const RedisRegistry&) = delete;

    Task<void> connect();
    void closeNow() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] RedisHandle get(::ruvia::operation_scope& operationScope) const;
    [[nodiscard]] RedisHandle get(std::string_view alias, ::ruvia::operation_scope& operationScope) const;

private:
    using Entry = std::unique_ptr<RedisClientRuntime, PmrObjectDeleter<RedisClientRuntime>>;

    WorkerHandle worker_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<Entry> pools_;
    NamedCapabilityIndex aliasIndex_;
};

}  // namespace ruvia::detail

#endif
