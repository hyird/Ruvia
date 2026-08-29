#pragma once

#include <array>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <system_error>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/io/OperationDeadline.h"
#include "ruvia/core/detail/pool/PoolLeaseScheduler.h"
#include "ruvia/core/detail/worker/WorkerCancellationPost.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/http/detail/client/HttpClientAccess.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/web/HttpClientHandle.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/HttpClientRequestStorage.h"
#include "ruvia/web/detail/integration/NamedCapability.h"

namespace ruvia::detail {

struct HttpClientRequestStorageAccess final {
    [[nodiscard]] static HttpClientRequestView view(
        const HttpClientRequestStorage& request, std::pmr::vector<HttpHeaderView>& headers);
};

class HttpClientPool;
using HttpClientOperationCancellationMailbox = WorkerCancellationMailbox<HttpClientPool>;

class HttpClientPool final {
public:
    HttpClientPool(asio::io_context& ioContext, const WorkerHandle& worker,
        HttpClientConfigStorage config, std::pmr::memory_resource* resource);
    HttpClientPool(asio::io_context&, WorkerHandle&&, HttpClientConfigStorage,
        std::pmr::memory_resource*) = delete;
    ~HttpClientPool();
    HttpClientPool(const HttpClientPool&) = delete;
    HttpClientPool& operator=(const HttpClientPool&) = delete;

    [[nodiscard]] Task<HttpClientResponse> execute(
        HttpClientRequestStorage request, OperationOptions options);
    void closeNow() noexcept;
    [[nodiscard]] Task<void> join();
    [[nodiscard]] HttpClientStats stats() const noexcept;
    [[nodiscard]] std::string_view host() const noexcept {
        return config_.host;
    }
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] HttpScheme scheme() const noexcept {
        return config_.scheme;
    }

private:
    friend class WorkerCancellationMailbox<HttpClientPool>;
    friend class ::ruvia::HttpClientResponse;
    friend class ::ruvia::HttpClientResponseBody;

    enum class WireProtocol : std::uint8_t { kUnknown, kHttp1, kHttp2 };
    enum class AbortReason : std::uint8_t { kNone, kTimeout, kCancelled, kClosing };
    enum class DeadlineKind : std::uint8_t { kResolve, kSocket };

    struct Http2PendingStream final {
        Http2PendingStream(const WorkerHandle& worker, HttpClientResponse& value)
            : signal(worker),
              response(&value) {}
        Http2PendingStream(WorkerHandle&&, HttpClientResponse&) = delete;

        WorkerSignal signal;
        HttpClientResponse* response;
        std::optional<HttpClientError::Code> error;
        std::exception_ptr failure;
        std::uint64_t requestId{0};
        std::uint64_t cancellationId{0};
        std::uint32_t streamId{0};
        std::size_t responseHeaderCount{0};
        bool complete{false};
        bool retryable{false};

        [[nodiscard]] bool failed() const noexcept {
            return error.has_value() || failure != nullptr;
        }
    };

    struct Http2Runtime final {
        Http2Runtime(const WorkerHandle& worker, std::pmr::memory_resource* resource)
            : writeSignal(worker),
              stateSignal(worker),
              connectScheduler(1, worker, resource),
              http1Scheduler(1, worker, resource),
              pending(resource) {}
        Http2Runtime(WorkerHandle&&, std::pmr::memory_resource*) = delete;

        WorkerSignal writeSignal;
        WorkerSignal stateSignal;
        PoolLeaseScheduler connectScheduler;
        PoolLeaseScheduler http1Scheduler;
        std::pmr::vector<Http2PendingStream*> pending;
        std::uint64_t generation{0};
        std::uint64_t nextRequestId{0};
        std::uint64_t stateCancellationId{0};
        std::size_t sessionTasks{0};
        std::size_t http1Operations{0};
        std::size_t stateCancellationWaiters{0};
        bool running{false};
        bool connecting{false};
        bool draining{false};
        bool failed{false};
    };

    struct Connection final {
        Connection(asio::io_context& ioContext, asio::ssl::context& tlsContext,
            const WorkerHandle& worker, std::pmr::memory_resource* resource);
        Connection(asio::io_context&, asio::ssl::context&, WorkerHandle&&,
            std::pmr::memory_resource*) = delete;
        ~Connection();
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) noexcept;
        Connection& operator=(Connection&&) = delete;
        asio::ip::tcp::resolver resolver;
        asio::ssl::stream<asio::ip::tcp::socket> stream;
        std::pmr::string readBuffer;
        std::pmr::string writeBuffer;
        std::unique_ptr<Http2Connection, PmrObjectDeleter<Http2Connection>> http2;
        std::unique_ptr<Http2Runtime, PmrObjectDeleter<Http2Runtime>> http2Runtime;
        OperationDeadline<DeadlineKind> deadline;
        std::unique_ptr<WorkerTimerRegistration, PmrObjectDeleter<WorkerTimerRegistration>>
            deadlineTimer;
        std::uint64_t generation{0};
        std::uint64_t cancellationId{0};
        WireProtocol protocol{WireProtocol::kUnknown};
        AbortReason abortReason{AbortReason::kNone};
        bool connected{false};
    };

    class Http2PendingRegistration final {
    public:
        Http2PendingRegistration(
            HttpClientPool& pool, Connection& connection, Http2PendingStream& pending) noexcept
            : pool_(pool),
              connection_(connection),
              pending_(pending) {}
        ~Http2PendingRegistration() {
            reset();
        }

        Http2PendingRegistration(const Http2PendingRegistration&) = delete;
        Http2PendingRegistration& operator=(const Http2PendingRegistration&) = delete;

        void reset() noexcept;

    private:
        HttpClientPool& pool_;
        Connection& connection_;
        Http2PendingStream& pending_;
        bool active_{true};
    };

    struct StoredCookie final {
        StoredCookie(
            std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
            : name(name, resource),
              value(value, resource),
              path("/", resource),
              domain(resource) {}
        std::pmr::string name;
        std::pmr::string value;
        std::pmr::string path;
        std::pmr::string domain;
        std::optional<std::chrono::system_clock::time_point> expires;
        bool secure{false};
        bool hostOnly{true};
        bool persistent{true};
    };

    class Lease final {
    public:
        Lease(HttpClientPool& pool, std::size_t index) noexcept
            : pool_(pool),
              index_(index) {}
        ~Lease();
        [[nodiscard]] Connection& connection() noexcept {
            return pool_.connections_[index_ % pool_.connections_.size()];
        }
        void discard() noexcept {
            discard_ = true;
        }

    private:
        HttpClientPool& pool_;
        std::size_t index_;
        bool discard_{false};
    };

    [[nodiscard]] Task<std::size_t> acquire(const OperationTimeout& timeout, StopToken stopToken);
    void release(std::size_t index) noexcept;
    void close(Connection& connection) noexcept;
    void cancelOperationById(std::uint64_t cancellationId) noexcept;
    void cancelOperation(std::size_t index, std::uint64_t generation, AbortReason reason) noexcept;
    [[nodiscard]] bool armDeadline(
        Connection& connection, const OperationTimeout& timeout, DeadlineKind kind);
    [[nodiscard]] bool clearDeadline(Connection& connection) noexcept;
    void throwAbort(const Connection& connection) const;
    [[nodiscard]] Task<void> ensureConnected(Connection& connection,
        const OperationTimeout& timeout, const OperationTimeout& acquireTimeout,
        StopToken stopToken);
    [[nodiscard]] Task<void> initializeHttp2(
        Connection& connection, const OperationTimeout& timeout);
    [[nodiscard]] Task<void> runHttp2Reader(Connection& connection, std::uint64_t generation);
    [[nodiscard]] Task<void> runHttp2Writer(Connection& connection, std::uint64_t generation);
    [[nodiscard]] Task<void> executeInto(
        HttpClientRequestStorage request, OperationOptions options, HttpClientResponseState* state);
    [[nodiscard]] Task<void> executeRequestInto(
        HttpClientRequestStorage request, OperationOptions options, HttpClientResponseState* state);
    [[nodiscard]] Task<void> executeHttp1(Connection& connection,
        const HttpClientRequestStorage& request, const OperationTimeout& timeout,
        HttpClientResponse& response);
    [[nodiscard]] Task<void> executeHttp2(Connection& connection,
        const HttpClientRequestStorage& request, const OperationTimeout& timeout,
        StopToken stopToken, HttpClientResponse& response);
    [[nodiscard]] Task<void> write(
        Connection& connection, std::string_view bytes, const OperationTimeout& timeout);
    [[nodiscard]] Task<std::size_t> readSome(Connection& connection, std::span<char> bytes,
        const OperationTimeout& timeout, bool allowEof = false);
    void appendAutomaticHeaders(const HttpClientRequestStorage& request,
        std::pmr::vector<HttpHeaderView>& headers, std::pmr::string& cookieHeader);
    void retainResponseCookies(
        const HttpClientRequestStorage& request, const HttpClientResponse& response);
    void addCookie(std::string_view name, std::string_view value);
    static void decodeResponseContentEncoding(HttpClientResponse& response,
        bool contentSemanticsPresent, std::size_t maxDecodedBytes,
        std::pmr::memory_resource* resource);
    [[nodiscard]] static std::size_t cookieStorageBytes(std::string_view name,
        std::string_view value, std::string_view path, std::string_view domain) noexcept;
    [[nodiscard]] bool cookieCapacityAvailable(
        std::size_t replacedBytes, std::size_t replacementBytes, bool adding) const noexcept;
    void drainHttp2Events(Connection& connection);
    void failHttp2Session(Connection& connection, std::uint64_t generation,
        std::error_code transportError,
        const std::exception_ptr& failure = std::exception_ptr{}) noexcept;
    void finishHttp2SessionTask(Connection& connection, std::uint64_t generation) noexcept;
    void submitHttp2Reset(Connection& connection, std::uint32_t streamId) noexcept;
    void cancelHttp2Stream(
        Connection& connection, std::uint64_t requestId, AbortReason reason) noexcept;
    void abandonResponse(HttpClientResponseState& state) noexcept;
    void releaseResponseData(HttpClientResponseState& state) noexcept;
    void removeHttp2Pending(Connection& connection, Http2PendingStream& pending) noexcept;
    [[nodiscard]] Task<void> waitForHttp2SessionStop(
        Connection& connection, const OperationTimeout& timeout, StopToken stopToken);
    asio::io_context& ioContext_;
    const WorkerHandle& worker_;
    std::pmr::memory_resource* resource_;
    HttpClientConfigStorage config_;
    asio::ssl::context tlsContext_;
    std::pmr::vector<Connection> connections_;
    PoolLeaseScheduler scheduler_;
    std::shared_ptr<HttpClientOperationCancellationMailbox> cancellationMailbox_;
    TaskScope backgroundTasks_;
    std::pmr::vector<StoredCookie> cookies_;
    std::size_t requestsBuffered_{0};
    std::size_t requestsInFlight_{0};
    std::size_t completedRequests_{0};
    std::size_t failedRequests_{0};
    std::size_t bytesSent_{0};
    std::size_t bytesReceived_{0};
    std::size_t cookieBytes_{0};
    bool backgroundJoined_{false};
};

class HttpClientRegistry final {
public:
    HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
        std::pmr::memory_resource* resource, const HttpClientConfig& defaultConfig);
    HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
        std::pmr::memory_resource* resource, std::span<const HttpClientDefinition> definitions);
    HttpClientRegistry(asio::io_context&, WorkerHandle&&, std::pmr::memory_resource*,
        const HttpClientConfig&) = delete;
    HttpClientRegistry(asio::io_context&, WorkerHandle&&, std::pmr::memory_resource*,
        std::span<const HttpClientDefinition>) = delete;
    ~HttpClientRegistry();
    HttpClientRegistry(const HttpClientRegistry&) = delete;
    HttpClientRegistry& operator=(const HttpClientRegistry&) = delete;

    void closeNow() noexcept;
    [[nodiscard]] Task<void> join();
    [[nodiscard]] HttpClientHandle get(
        std::pmr::memory_resource* resource, ScopedOperationScope& scope) const;
    [[nodiscard]] HttpClientHandle get(std::string_view alias, std::pmr::memory_resource* resource,
        ScopedOperationScope& scope) const;

private:
    using PoolOwner = std::unique_ptr<HttpClientPool, PmrObjectDeleter<HttpClientPool>>;

    void add(
        asio::io_context& ioContext, const WorkerHandle& worker, HttpClientConfigStorage config);
    std::pmr::memory_resource* resource_;
    std::pmr::vector<PoolOwner> pools_;
    NamedCapabilityIndex aliasIndex_;
    bool closing_{false};
};

}  // namespace ruvia::detail
