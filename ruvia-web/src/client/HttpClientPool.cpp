#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/connect.hpp>
#include <asio/ssl/error.hpp>
#include <asio/write.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/TcpSocketOptions.h"
#include "ruvia/core/WorkerCancellationPost.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/client/HttpClientResultBudget.h"
#include "ruvia/web/detail/http3/Http3ClientConnection.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t kConnectionReadBufferInitialBytes = std::size_t{16} * 1024;

ClientAlpnMode clientAlpnMode(HttpClientProtocol protocol) noexcept {
    switch (protocol) {
        case HttpClientProtocol::kNegotiate:
            return ClientAlpnMode::kNegotiate;
        case HttpClientProtocol::kHttp1Only:
            return ClientAlpnMode::kHttp11;
        case HttpClientProtocol::kHttp2Only:
            return ClientAlpnMode::kHttp2;
        case HttpClientProtocol::kHttp3Only:
            break;
    }
    std::terminate();
}

constexpr std::size_t kHttp3ConcurrentRequestsPerConnection =
    Http3QuicClientTransport::kMaxStreamsPerConnection -
    Http3QuicClientTransport::kPeerCriticalStreamReserve;
constexpr std::size_t kHttp3PoolReceiveBodyBytes = std::size_t{64} * 1024 * 1024;

std::size_t httpClientSchedulerSlots(const HttpClientConfigStorage& config) noexcept {
    if (config.protocol == HttpClientProtocol::kHttp1Only) {
        return config.connectionCount;
    }
    if (config.protocol == HttpClientProtocol::kHttp3Only) {
        return config.connectionCount * kHttp3ConcurrentRequestsPerConnection;
    }
    return config.connectionCount * config.maxConcurrentHttp2StreamsPerConnection;
}

[[nodiscard]] HttpClientError::Code http3ClientErrorCode(
    Http3ClientConnection::Outcome outcome) noexcept {
    using Outcome = Http3ClientConnection::Outcome;
    switch (outcome) {
        case Outcome::kCancelled:
            return HttpClientError::Code::kCancelled;
        case Outcome::kDeadline:
            return HttpClientError::Code::kTimeout;
        case Outcome::kConnectFailed:
            return HttpClientError::Code::kConnectFailed;
        case Outcome::kTransportError:
            return HttpClientError::Code::kIoError;
        case Outcome::kResponseTooLarge:
            return HttpClientError::Code::kResponseTooLarge;
        case Outcome::kResultBudgetExceeded:
            return HttpClientError::Code::kResultBudgetExceeded;
        case Outcome::kQueueFull:
            return HttpClientError::Code::kQueueFull;
        case Outcome::kConnectionDraining:
        case Outcome::kRequestRejected:
        case Outcome::kInvalidRequest:
        case Outcome::kProtocolError:
            return HttpClientError::Code::kProtocolError;
        case Outcome::kPending:
        case Outcome::kComplete:
            break;
    }
    return HttpClientError::Code::kProtocolError;
}

[[nodiscard]] std::shared_ptr<HttpClientResultBudgetDomain> requireHttpClientResultBudgetDomain(
    const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain) {
    if (!resultBudgetDomain) {
        throw std::invalid_argument("HTTP client result budget domain must not be null");
    }
    return resultBudgetDomain;
}

}  // namespace

static_assert(workerCancellationPostIsInline<HttpClientOperationCancellationMailbox>);

HttpClientPool::Connection::Connection(asio::io_context& ioContext, asio::ssl::context& tlsContext,
    const WorkerHandle& worker, std::pmr::memory_resource* resource)
    : resolver(ioContext),
      stream(ioContext, tlsContext),
      readBuffer(pmrResourceOrDefault(resource)),
      writeBuffer(pmrResourceOrDefault(resource)),
      http2(nullptr, PmrObjectDeleter<::ruvia::Http2Connection>{pmrResourceOrDefault(resource)}),
      http2Runtime(makePmrObject<Http2Runtime>(resource, worker, resource)),
      deadlineTimer(makePmrObject<WorkerTimerRegistration>(resource)) {
    readBuffer.reserve(kConnectionReadBufferInitialBytes);
}

HttpClientPool::Connection::~Connection() = default;
HttpClientPool::Connection::Connection(Connection&&) noexcept = default;

HttpClientPool::HttpClientPool(asio::io_context& ioContext, const WorkerHandle& worker,
    HttpClientConfigStorage config, HttpClientResultBudgetConfig resultBudget,
    std::pmr::memory_resource* resource)
    : HttpClientPool(ioContext, worker, std::move(config),
          std::make_shared<HttpClientResultBudgetDomain>(resultBudget), resource) {}

HttpClientPool::HttpClientPool(asio::io_context& ioContext, const WorkerHandle& worker,
    HttpClientConfigStorage config,
    const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain,
    std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      worker_(worker),
      resource_(pmrResourceOrDefault(resource)),
      config_(std::move(config)),
      resultBudgetDomain_(requireHttpClientResultBudgetDomain(resultBudgetDomain)),
      tlsContext_(asio::ssl::context::tls_client),
      connections_(resource_),
      scheduler_(httpClientSchedulerSlots(config_), worker_, resource_),
      backgroundTasks_(worker_, {.resource = resource_}),
      http3Tls_(nullptr, PmrObjectDeleter<Http3QuicClientTlsContext>{resource_}),
      http3BodyBudget_(nullptr, PmrObjectDeleter<Http3ClientBodyBudget>{resource_}),
      http3Connections_(resource_),
      http3GenerationSignal_(worker_),
      http3PendingCancellations_(resource_),
      cookies_(resource_) {
    for (const auto& [name, value] : config_.cookies) {
        addCookie(name, value);
    }
    if (config_.protocol == HttpClientProtocol::kHttp3Only) {
        http3Tls_ = makePmrObject<Http3QuicClientTlsContext>(
            resource_, config_.transport.view());
        http3BodyBudget_ =
            makePmrObject<Http3ClientBodyBudget>(resource_, kHttp3PoolReceiveBodyBytes);
        http3Connections_.reserve(config_.connectionCount);
        for (std::size_t i = 0; i < config_.connectionCount; ++i) {
            // HttpClientPool is staged before the business worker starts, but
            // QUIC connection owners capture and enforce worker-thread
            // affinity. Keep stable pool slots and construct each owner lazily
            // from the first request running on that worker.
            http3Connections_.emplace_back(
                nullptr, PmrObjectDeleter<Http3ClientConnection>{resource_});
        }
    } else {
        if (config_.scheme == HttpScheme::kHttps) {
            configureClientTlsContext(tlsContext_, config_.transport.view());
        }
        connections_.reserve(config_.connectionCount);
        for (std::size_t i = 0; i < config_.connectionCount; ++i) {
            connections_.emplace_back(ioContext_, tlsContext_, worker_, resource_);
        }
    }
    cancellationMailbox_ = makeWorkerCancellationMailbox(*this, worker_);
}

HttpClientPool::~HttpClientPool() {
    closeNow();
}

HttpClientPool::Lease::~Lease() {
    if (discard_) {
        pool_.close(connection());
    }
    pool_.release(index_);
}

void HttpClientPool::close(Connection& connection) noexcept {
    auto& runtime = *connection.http2Runtime;
    if (runtime.running) {
        // A multiplexed connection owns background reader/writer tasks. Route
        // every terminal close through their shared failure transition so both
        // drivers and all pending streams are woken before join().
        failHttp2Session(
            connection, runtime.generation, std::make_error_code(std::errc::operation_canceled));
        return;
    }
    connection.deadlineTimer->cancel();
    connection.deadline.reset();
    connection.resolver.cancel();
    std::error_code ignored;
    (void)connection.stream.lowest_layer().cancel(ignored);
    (void)connection.stream.lowest_layer().close(ignored);
    connection.connected = false;
    connection.protocol = WireProtocol::kUnknown;
    if (runtime.sessionTasks == 0) {
        connection.http2.reset();
        runtime.running = false;
        runtime.draining = false;
        runtime.failed = false;
    }
    connection.readBuffer.clear();
    connection.writeBuffer.clear();
}

void HttpClientPool::closeNow() noexcept {
    cancellationMailbox_->detach(*this);
    if (!scheduler_.close()) {
        return;
    }
    for (auto& connection : http3Connections_) {
        if (connection) {
            connection->requestStop();
        }
    }
    backgroundTasks_.requestStop();
    for (auto& connection : connections_) {
        connection.abortReason = AbortReason::kClosing;
        auto& runtime = *connection.http2Runtime;
        (void)runtime.connectScheduler.close();
        (void)runtime.http1Scheduler.close();
        if (runtime.running) {
            failHttp2Session(connection, runtime.generation,
                std::make_error_code(std::errc::operation_canceled));
        } else {
            close(connection);
        }
    }
}

Task<void> HttpClientPool::join() {
    if (backgroundJoined_) {
        co_return;
    }
    backgroundJoined_ = true;
    co_await backgroundTasks_.join();
    // Destroy QUIC SSL/socket/session owners while the worker loop and its PMR
    // owner are still alive. HttpClientPool itself is later destroyed by the
    // App lifecycle thread after the worker has joined.
    for (auto& connection : http3Connections_) {
        connection.reset();
    }
}

HttpClientStats HttpClientPool::stats() const noexcept {
    return {requestsBuffered_, requestsInFlight_, completedRequests_, failedRequests_, bytesSent_,
        bytesReceived_};
}

std::uint16_t HttpClientPool::port() const noexcept {
    return httpClientPort(config_);
}

Task<std::size_t> HttpClientPool::acquire(const ruvia::OperationTimeout& timeout, StopToken stopToken) {
    auto result = co_await scheduler_.acquire(
        timeout.constrainedBy(config_.acquireTimeout).remaining(), std::move(stopToken), worker_);
    switch (result.status()) {
        case ruvia::PoolWaiterResult::Status::kAcquired:
            co_return result.index();
        case ruvia::PoolWaiterResult::Status::kTimedOut:
            throw HttpClientError(
                HttpClientError::Code::kTimeout, "http client connection pool acquire timed out");
        case ruvia::PoolWaiterResult::Status::kCancelled:
            throw HttpClientError(HttpClientError::Code::kCancelled, "http client request cancelled");
        case ruvia::PoolWaiterResult::Status::kClosed:
            throw HttpClientError(HttpClientError::Code::kClosing, "http client pool is closing");
    }
    std::terminate();
}

void HttpClientPool::release(std::size_t index) noexcept {
    const auto status = scheduler_.release(index);
    if (status == PoolLeaseReleaseStatus::kInvalidSlot ||
        status == PoolLeaseReleaseStatus::kAlreadyReleased) {
        std::terminate();
    }
}

void HttpClientPool::cancelOperationById(std::uint64_t cancellationId) noexcept {
    if (cancellationId == 0) {
        return;
    }
    const auto http3 = std::ranges::find_if(http3PendingCancellations_,
        [cancellationId](const Http3PendingCancellation& pending) {
            return pending.cancellationId == cancellationId;
        });
    if (http3 != http3PendingCancellations_.end()) {
        if (http3->connection == nullptr) {
            http3GenerationSignal_.notify();
        } else {
            http3->connection->cancel(http3->requestId);
        }
        return;
    }
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        auto& connection = connections_[index];
        if (connection.cancellationId == cancellationId) {
            connection.cancellationId = 0;
            cancelOperation(index, connection.generation, AbortReason::kCancelled);
            return;
        }
        auto& runtime = *connection.http2Runtime;
        if (runtime.stateCancellationId == cancellationId) {
            runtime.stateSignal.notify();
            return;
        }
        const auto pending = std::ranges::find_if(
            runtime.pending, [cancellationId](const Http2PendingStream* stream) {
                return stream->cancellationId == cancellationId;
            });
        if (pending != runtime.pending.end()) {
            (*pending)->cancellationId = 0;
            cancelHttp2Stream(connection, (*pending)->requestId, AbortReason::kCancelled);
            return;
        }
    }
}

bool HttpClientPool::armDeadline(
    Connection& connection, const ruvia::OperationTimeout& timeout, DeadlineKind kind) {
    connection.deadlineTimer->cancel();
    const auto remaining = timeout.remaining();
    if (!remaining) {
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
            const auto expired = connection.deadline.expire(std::chrono::steady_clock::now());
            if (!expired) {
                return;
            }
            connection.abortReason = AbortReason::kTimeout;
            std::error_code ignored;
            if (*expired == DeadlineKind::kResolve) {
                connection.resolver.cancel();
            } else {
                (void)connection.stream.lowest_layer().cancel(ignored);
            }
        });
    return true;
}

bool HttpClientPool::clearDeadline(Connection& connection) noexcept {
    connection.deadlineTimer->cancel();
    return connection.deadline.clear();
}

void HttpClientPool::cancelOperation(
    std::size_t index, std::uint64_t generation, AbortReason reason) noexcept {
    if (connections_.empty()) {
        return;
    }
    auto& connection = connections_[index % connections_.size()];
    if (connection.generation != generation) {
        return;
    }
    connection.abortReason = reason;
    std::error_code ignored;
    connection.resolver.cancel();
    (void)connection.stream.lowest_layer().cancel(ignored);
    (void)connection.stream.lowest_layer().close(ignored);
    connection.connected = false;
    connection.protocol = WireProtocol::kUnknown;
    connection.http2Runtime->writeSignal.notify();
    connection.http2Runtime->stateSignal.notify();
}

void HttpClientPool::throwAbort(const Connection& connection) const {
    switch (connection.abortReason) {
        case AbortReason::kNone:
            return;
        case AbortReason::kTimeout:
            throw HttpClientError(HttpClientError::Code::kTimeout, "http client request timed out");
        case AbortReason::kCancelled:
            throw HttpClientError(
                HttpClientError::Code::kCancelled, "http client request cancelled");
        case AbortReason::kClosing:
            throw HttpClientError(HttpClientError::Code::kClosing, "http client pool is closing");
    }
}

HttpClientError::Code HttpClientPool::transportErrorCode(
    const std::error_code& error) const noexcept {
    return config_.scheme == HttpScheme::kHttps &&
                   (error.category() == asio::error::get_ssl_category() ||
                       error == asio::ssl::error::stream_truncated)
               ? HttpClientError::Code::kTlsFailed
               : HttpClientError::Code::kIoError;
}

Task<void> HttpClientPool::write(
    Connection& connection, std::string_view bytes, const ruvia::OperationTimeout& timeout) {
    if (bytes.empty()) {
        co_return;
    }
    const auto writeTimeout = timeout.constrainedBy(config_.writeTimeout);
    if (!armDeadline(connection, writeTimeout, DeadlineKind::kSocket)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client write timed out");
    }
    AsioCompletion<std::size_t> completion =
        config_.scheme == HttpScheme::kHttps
            ? co_await ruvia::asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable {
                  asio::async_write(connection.stream, asio::buffer(bytes), std::move(handler));
              })
            : co_await ruvia::asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable {
                  asio::async_write(
                      connection.stream.next_layer(), asio::buffer(bytes), std::move(handler));
              });
    const bool timedOut = clearDeadline(connection) || writeTimeout.expired();
    throwAbort(connection);
    if (timedOut) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client write timed out");
    }
    if (completion.errorCode()) {
        throw HttpClientError(
            transportErrorCode(completion.errorCode()), completion.errorCode().message());
    }
    bytesSent_ += completion.result();
}

Task<std::size_t> HttpClientPool::readSome(
    Connection& connection, std::span<char> bytes, const ruvia::OperationTimeout& timeout, bool allowEof) {
    if (!armDeadline(connection, timeout, DeadlineKind::kSocket)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client request timed out");
    }
    AsioCompletion<std::size_t> completion =
        config_.scheme == HttpScheme::kHttps
            ? co_await ruvia::asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable {
                  connection.stream.async_read_some(
                      asio::buffer(bytes.data(), bytes.size()), std::move(handler));
              })
            : co_await ruvia::asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable {
                  connection.stream.next_layer().async_read_some(
                      asio::buffer(bytes.data(), bytes.size()), std::move(handler));
              });
    const bool timedOut = clearDeadline(connection) || timeout.expired();
    throwAbort(connection);
    if (timedOut) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client request timed out");
    }
    if (completion.errorCode()) {
        if (allowEof && completion.errorCode() == asio::error::eof) {
            co_return 0;
        }
        throw HttpClientError(
            transportErrorCode(completion.errorCode()), completion.errorCode().message());
    }
    bytesReceived_ += completion.result();
    co_return completion.result();
}

Task<void> HttpClientPool::ensureConnected(Connection& connection,
    const ruvia::OperationTimeout& operationTimeout, const ruvia::OperationTimeout& acquireTimeout,
    StopToken stopToken) {
    auto& runtime = *connection.http2Runtime;
    auto acquired =
        co_await runtime.connectScheduler.acquire(acquireTimeout.remaining(), stopToken, worker_);
    switch (acquired.status()) {
        case ruvia::PoolWaiterResult::Status::kAcquired:
            break;
        case ruvia::PoolWaiterResult::Status::kTimedOut:
            throw HttpClientError(
                HttpClientError::Code::kTimeout, "HTTP client connect wait timed out");
        case ruvia::PoolWaiterResult::Status::kCancelled:
            throw HttpClientError(
                HttpClientError::Code::kCancelled, "HTTP client connect wait cancelled");
        case ruvia::PoolWaiterResult::Status::kClosed:
            throw HttpClientError(HttpClientError::Code::kClosing, "HTTP client pool is closing");
        default:
            std::terminate();
    }
    const auto connectSlot = acquired.index();
    struct ConnectLease final {
        PoolLeaseScheduler& scheduler;
        std::size_t slot;
        ~ConnectLease() {
            (void)scheduler.release(slot);
        }
    } connectLease{runtime.connectScheduler, connectSlot};
    if (connection.connected) {
        co_return;
    }
    connection.abortReason = AbortReason::kNone;
    ++connection.generation;
    if (connection.generation == 0) {
        ++connection.generation;
    }
    struct ConnectCancellationGeneration final {
        Connection& connection;
        ~ConnectCancellationGeneration() {
            ++connection.generation;
        }
    } cancellationGeneration{connection};
    connection.cancellationId = 0;
    std::uint64_t cancellationId = 0;
    StopRegistration stopRegistration;
    if (stopToken.stoppable()) {
        cancellationId = cancellationMailbox_->nextOperationId();
        connection.cancellationId = cancellationId;
        stopToken.registerCallback(
            stopRegistration, WorkerCancellationPost<HttpClientOperationCancellationMailbox>(
                                  cancellationMailbox_, cancellationId));
    }
    struct CancellationRegistrationGuard final {
        Connection& connection;
        std::uint64_t cancellationId;
        StopRegistration& registration;

        ~CancellationRegistrationGuard() {
            if (connection.cancellationId == cancellationId) {
                connection.cancellationId = 0;
            }
            registration.reset();
        }
    } cancellationRegistrationGuard{connection, cancellationId, stopRegistration};
    if (stopToken.stopRequested()) {
        cancelOperationById(cancellationId);
    }
    while (runtime.sessionTasks != 0 || !runtime.pending.empty()) {
        throwAbort(connection);
        co_await runtime.stateSignal.wait();
    }
    throwAbort(connection);
    runtime.connecting = true;
    struct ConnectGuard final {
        Http2Runtime& runtime;
        ~ConnectGuard() {
            runtime.connecting = false;
            runtime.stateSignal.notify();
        }
    } connectGuard{runtime};
    connection.http2.reset();
    runtime.running = false;
    runtime.draining = false;
    runtime.failed = false;
    const auto timeout = operationTimeout.constrainedBy(config_.connectTimeout);
    ClientPortTextBuffer portBuffer{};
    const auto port = formatClientPort(httpClientPort(config_), portBuffer);
    if (!armDeadline(connection, timeout, DeadlineKind::kResolve)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client resolve timed out");
    }
    auto resolve = co_await ruvia::asyncAsio<asio::ip::tcp::resolver::results_type>(
        [&connection, this, port](auto handler) mutable {
            connection.resolver.async_resolve(config_.host, port, std::move(handler));
        });
    const bool resolveTimedOut = clearDeadline(connection) || timeout.expired();
    throwAbort(connection);
    if (resolveTimedOut) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client resolve timed out");
    }
    if (resolve.errorCode()) {
        throw HttpClientError(HttpClientError::Code::kResolveFailed, resolve.errorCode().message());
    }
    auto endpoints = std::move(resolve).takeResult();

    if (!armDeadline(connection, timeout, DeadlineKind::kSocket)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client connect timed out");
    }
    auto connected = co_await ruvia::asyncAsio([&connection, &endpoints](auto handler) mutable {
        asio::async_connect(connection.stream.lowest_layer(), endpoints, std::move(handler));
    });
    const bool connectTimedOut = clearDeadline(connection) || timeout.expired();
    throwAbort(connection);
    if (connectTimedOut) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client connect timed out");
    }
    if (connected.errorCode()) {
        throw HttpClientError(
            HttpClientError::Code::kConnectFailed, connected.errorCode().message());
    }
    const auto transport = config_.transport.view();
    ruvia::applyTcpSocketPolicies(
        connection.stream.next_layer(), transport.tcpNoDelay, transport.tcpKeepAlive);

    if (config_.scheme == HttpScheme::kHttps) {
        const auto tlsSetup = prepareClientTlsStream(
            connection.stream, config_.host, transport, clientAlpnMode(config_.protocol));
        if (tlsSetup != ClientTlsSetupError::kNone) {
            throw HttpClientError(
                HttpClientError::Code::kTlsFailed, clientTlsSetupErrorMessage(tlsSetup));
        }
        if (!armDeadline(connection, timeout, DeadlineKind::kSocket)) {
            throw HttpClientError(
                HttpClientError::Code::kTimeout, "http client TLS handshake timed out");
        }
        auto handshake = co_await ruvia::asyncAsio([&connection](auto handler) mutable {
            connection.stream.async_handshake(asio::ssl::stream_base::client, std::move(handler));
        });
        const bool handshakeTimedOut = clearDeadline(connection) || timeout.expired();
        throwAbort(connection);
        if (handshakeTimedOut) {
            throw HttpClientError(
                HttpClientError::Code::kTimeout, "http client TLS handshake timed out");
        }
        if (handshake.errorCode()) {
            throw HttpClientError(
                HttpClientError::Code::kTlsFailed, handshake.errorCode().message());
        }
        const auto alpn = selectedClientAlpn(connection.stream.native_handle());
        if (config_.protocol == HttpClientProtocol::kHttp2Only && alpn != "h2") {
            throw HttpClientError(
                HttpClientError::Code::kProtocolUnavailable, "upstream did not negotiate HTTP/2");
        }
        connection.protocol = alpn == "h2" ? WireProtocol::kHttp2 : WireProtocol::kHttp1;
    } else {
        connection.protocol = config_.protocol == HttpClientProtocol::kHttp2Only
                                  ? WireProtocol::kHttp2
                                  : WireProtocol::kHttp1;
    }
    connection.connected = true;
    if (connection.protocol == WireProtocol::kHttp2) {
        co_await initializeHttp2(connection, timeout);
    }
}

HttpClientRequestStorage HttpClientPool::makeHttp3Request(
    const HttpClientRequestStorage& request) {
    std::pmr::vector<HttpHeaderView> headers(resource_);
    auto source = HttpClientRequestStorageAccess::view(request, headers);
    std::pmr::string cookieHeader(resource_);
    appendAutomaticHeaders(request, headers, cookieHeader);

    HttpClientRequestStorage wire(source.method.view(), source.target.view(), resource_);
    for (const auto& header : headers) {
        wire.appendHeader(header.name(), header.value());
    }
    if (const auto* bytes = source.content.borrowedBytes()) {
        wire.setBody(bytes->value());
    }
    return wire;
}

Http3ClientConnection& HttpClientPool::http3Connection(std::size_t connectionIndex) {
    if (http3Connections_.empty() || http3Tls_ == nullptr || http3BodyBudget_ == nullptr) {
        throw HttpClientError(
            HttpClientError::Code::kProtocolUnavailable, "HTTP/3 client is not configured");
    }
    auto& owner = http3Connections_.at(connectionIndex % http3Connections_.size());
    if (!owner || (owner->terminal() && !owner->running() && owner->retainedRequests() == 0)) {
        const auto origin = HttpOriginView::https(
            {.host = config_.host, .port = config_.port});
        owner = makePmrObject<Http3ClientConnection>(resource_, ioContext_, worker_,
            backgroundTasks_, *http3Tls_, origin, config_.connectTimeout, resource_,
            kHttp3ConcurrentRequestsPerConnection, config_.maxResponseBytes,
            std::chrono::seconds(30), http3BodyBudget_.get(), config_.writeTimeout,
            Http3ClientConnection::LifecycleNotification{
                .context = &http3GenerationSignal_,
                .notify = [](void* context) noexcept {
                    static_cast<WorkerSignal*>(context)->notify();
                },
            });
    }
    return *owner;
}

Task<void> HttpClientPool::executeHttp3(std::size_t connectionIndex,
    const HttpClientRequestStorage& request, const ruvia::OperationTimeout& timeout,
    StopToken stopToken, HttpClientResponse& response) {
    struct CancellationGuard final {
        std::pmr::vector<Http3PendingCancellation>& pending;
        StopRegistration& registration;
        HttpClientResponseState& state;
        std::uint64_t id;

        ~CancellationGuard() {
            registration.reset();
            if (state.cancellationId == id) {
                state.cancellationId = 0;
            }
            std::erase_if(pending,
                [id = id](const Http3PendingCancellation& item) {
                    return item.cancellationId == id;
                });
        }
    };

    std::optional<HttpClientRequestStorage> wire;
    wire.emplace(makeHttp3Request(request));
    auto absoluteDeadline = timeout.deadline();
    bool retriedRejectedRequest = false;
    const auto connectionCount = http3Connections_.size();
    if (connectionCount == 0) {
        throw HttpClientError(
            HttpClientError::Code::kProtocolUnavailable, "HTTP/3 client is not configured");
    }

    for (;;) {
        Http3ClientConnection* connection = nullptr;
        std::size_t selectedIndex = connectionIndex % connectionCount;
        for (;;) {
            for (std::size_t offset = 0; offset < connectionCount; ++offset) {
                const auto candidateIndex = (connectionIndex + offset) % connectionCount;
                auto& candidate = http3Connection(candidateIndex);
                if (candidate.accepting()) {
                    connection = &candidate;
                    selectedIndex = candidateIndex;
                    break;
                }
            }
            if (connection != nullptr) {
                break;
            }
            if (timeout.expired()) {
                throw HttpClientError(
                    HttpClientError::Code::kTimeout, "HTTP/3 connection rotation timed out");
            }
            if (stopToken.stopRequested()) {
                throw HttpClientError(
                    HttpClientError::Code::kCancelled, "HTTP/3 request cancelled");
            }

            WorkerTimerRegistration timer;
            if (const auto remaining = timeout.remaining()) {
                WorkerHandleAccess::scheduleTimer(worker_, timer,
                    workerTimerDeadlineAfter(*remaining), [this](WorkerTimerOutcome outcome) noexcept {
                        if (outcome == WorkerTimerOutcome::kExpired) {
                            http3GenerationSignal_.notify();
                        }
                    });
            }
            StopRegistration stopRegistration;
            const auto cancellationId = cancellationMailbox_->nextOperationId();
            http3PendingCancellations_.push_back({cancellationId, nullptr, 0});
            response.state_->cancellationId = cancellationId;
            CancellationGuard cancellation{
                http3PendingCancellations_, stopRegistration, *response.state_, cancellationId};
            if (stopToken.stoppable()) {
                stopToken.registerCallback(stopRegistration,
                    WorkerCancellationPost<HttpClientOperationCancellationMailbox>(
                        cancellationMailbox_, cancellationId));
            }
            if (stopToken.stopRequested()) {
                cancelOperationById(cancellationId);
            }
            co_await http3GenerationSignal_.wait();
            timer.cancel();
        }

        response.state_->connectionIndex = selectedIndex;
        const auto submission = connection->submit(std::move(*wire), *response.state_, absoluteDeadline);
        wire.reset();
        if (submission.outcome != Http3ClientConnection::Outcome::kPending || submission.id == 0) {
            throw HttpClientError(http3ClientErrorCode(submission.outcome),
                "HTTP/3 request could not be submitted");
        }
        try {
            connection->startIfNeeded();
        } catch (...) {
            connection->requestStop();
            throw;
        }

        Http3ClientConnection::Outcome outcome = Http3ClientConnection::Outcome::kPending;
        std::optional<Http3ClientConnection::RejectedRequest> rejected;
        {
            StopRegistration stopRegistration;
            const auto cancellationId = cancellationMailbox_->nextOperationId();
            http3PendingCancellations_.push_back(
                {cancellationId, connection, submission.id});
            response.state_->cancellationId = cancellationId;
            CancellationGuard cancellation{
                http3PendingCancellations_, stopRegistration, *response.state_, cancellationId};
            if (stopToken.stoppable()) {
                stopToken.registerCallback(stopRegistration,
                    WorkerCancellationPost<HttpClientOperationCancellationMailbox>(
                        cancellationMailbox_, cancellationId));
            }
            if (stopToken.stopRequested()) {
                cancelOperationById(cancellationId);
            }

            co_await connection->wait(submission.id);
            if (response.state_->http3Connection != connection) {
                // The public response may be abandoned and retired by its
                // waiter before this terminal notification is dispatched.
                http3GenerationSignal_.notify();
                co_return;
            }
            const auto* result = connection->result(submission.id);
            if (result == nullptr) {
                std::terminate();
            }
            outcome = result->outcome;
            if (outcome == Http3ClientConnection::Outcome::kRequestRejected) {
                if (!retriedRejectedRequest && !timeout.expired() && !stopToken.stopRequested()) {
                    auto handoff = connection->takeRejectedRequest(submission.id);
                    if (handoff) {
                        rejected.emplace(std::move(*handoff));
                    }
                }
            } else if (response.state_->http3Connection == connection &&
                       !connection->releaseResponseRequest(submission.id)) {
                std::terminate();
            }
        }
        http3GenerationSignal_.notify();

        if (rejected) {
            wire.emplace(std::move(rejected->request));
            absoluteDeadline = rejected->deadline;
            retriedRejectedRequest = true;
            connectionIndex = connectionCount == 1
                                  ? selectedIndex
                                  : (selectedIndex + 1) % connectionCount;
            if (timeout.expired()) {
                throw HttpClientError(HttpClientError::Code::kTimeout,
                    "HTTP/3 request retry exceeded its deadline");
            }
            if (stopToken.stopRequested()) {
                throw HttpClientError(HttpClientError::Code::kCancelled,
                    "HTTP/3 request cancelled before retry");
            }
            continue;
        }
        if (outcome == Http3ClientConnection::Outcome::kRequestRejected) {
            if (stopToken.stopRequested()) {
                throw HttpClientError(HttpClientError::Code::kCancelled,
                    "HTTP/3 request cancelled after peer rejection");
            }
            if (timeout.expired()) {
                throw HttpClientError(HttpClientError::Code::kTimeout,
                    "HTTP/3 request deadline expired after peer rejection");
            }
            throw HttpClientError(HttpClientError::Code::kProtocolError,
                "HTTP/3 peer rejected the request as unprocessed");
        }
        co_return;
    }
}

Task<HttpClientResponse> HttpClientPool::execute(
    HttpClientRequestStorage request, OperationOptions options) {
    // The request owns all data before transport work can outlive the caller.
    auto ownedRequest = std::move(request).intoResource(resource_);
    HttpClientResponse response(resource_, worker_, *this);
    auto* state = response.state_;
    state->bufferedLimit = config_.maxResponseBytes;
    backgroundTasks_.spawn(executeInto(std::move(ownedRequest), std::move(options), state));
    while (!state->headReady && !state->failure && !state->errorCode) {
        co_await state->headSignal.wait();
    }
    if (state->failure) {
        std::rethrow_exception(state->failure);
    }
    if (state->errorCode) {
        const auto code = static_cast<HttpClientError::Code>(*state->errorCode);
        throw HttpClientError(code, "HTTP client request failed before the response head");
    }
    if (state->bodyDecodeRequired) {
        if (state->http2DataCredit) {
            releaseResponseData(*state);
        }
        state->notifyProducerSpace();
        while (!state->complete) {
            co_await state->dataSignal.wait();
        }
        if (state->failure) {
            std::rethrow_exception(state->failure);
        }
        if (state->errorCode) {
            throw HttpClientError(static_cast<HttpClientError::Code>(*state->errorCode),
                "HTTP client encoded response failed");
        }
    }
    co_return response;
}

Task<void> HttpClientPool::executeInto(
    HttpClientRequestStorage request, OperationOptions options, HttpClientResponseState* state) {
    HttpClientResponse keepAlive(state, true);
    try {
        co_await executeRequestInto(std::move(request), std::move(options), state);
    } catch (const HttpClientError&) {
        state->failure = std::current_exception();
    } catch (...) {
        state->failure = std::current_exception();
    }
    state->complete = true;
    state->headSignal.notify();
    state->dataSignal.notify();
}

Task<void> HttpClientPool::executeRequestInto(
    HttpClientRequestStorage request, OperationOptions options, HttpClientResponseState* state) {
    HttpClientResponse response(state, true);
    const ruvia::OperationTimeout timeout(
        options.timeout.has_value() ? options.timeout : config_.requestTimeout);
    const auto acquireTimeout = timeout.constrainedBy(config_.acquireTimeout);
    if (requestsBuffered_ >= config_.maxBufferedRequests) {
        ++failedRequests_;
        throw HttpClientError(
            HttpClientError::Code::kQueueFull, "http client request buffer is full");
    }
    ++requestsBuffered_;
    std::size_t index = 0;
    try {
        index = co_await acquire(timeout, options.stopToken);
    } catch (...) {
        --requestsBuffered_;
        ++failedRequests_;
        throw;
    }
    --requestsBuffered_;
    ++requestsInFlight_;
    Lease lease(*this, index);
    if (config_.protocol == HttpClientProtocol::kHttp3Only) {
        state->connectionIndex = index % http3Connections_.size();
        try {
            co_await executeHttp3(
                state->connectionIndex, request, timeout, options.stopToken, response);
            if (state->headReady && !state->failure && !state->errorCode) {
                retainResponseCookies(request, response);
            }
            --requestsInFlight_;
            if (state->failure || state->errorCode) {
                ++failedRequests_;
            } else {
                ++completedRequests_;
            }
            co_return;
        } catch (...) {
            --requestsInFlight_;
            ++failedRequests_;
            throw;
        }
    }

    auto& connection = lease.connection();
    state->connectionIndex = index % connections_.size();
    bool discardConnection = true;
    try {
        co_await ensureConnected(connection, timeout, acquireTimeout, options.stopToken);
        if (connection.protocol == WireProtocol::kHttp2) {
            // This lease now shares a multiplexed session with background drivers
            // and possibly other requests. A request-local failure must not
            // discard that shared connection.
            discardConnection = false;
            co_await executeHttp2(connection, request, timeout, options.stopToken, response);
        } else {
            // A negotiated HTTP/1 connection can have several operations that
            // already hold outer HTTP/2-capacity slots. Waiting for this
            // connection's single exchange slot does not own its socket: a
            // timeout/cancellation here must not close the exchange currently
            // using it.
            discardConnection = false;
            auto& runtime = *connection.http2Runtime;
            const bool mustBuffer = runtime.http1Operations != 0;
            if (mustBuffer && requestsBuffered_ >= config_.maxBufferedRequests) {
                throw HttpClientError(
                    HttpClientError::Code::kQueueFull, "HTTP client request buffer is full");
            }
            bool retryAsHttp2 = false;
            {
                ++runtime.http1Operations;
                if (mustBuffer) {
                    --requestsInFlight_;
                    ++requestsBuffered_;
                }
                struct H1Operation final {
                    HttpClientPool& pool;
                    Http2Runtime& runtime;
                    bool buffered;
                    ~H1Operation() {
                        if (buffered) {
                            --pool.requestsBuffered_;
                            ++pool.requestsInFlight_;
                        }
                        if (runtime.http1Operations == 0) {
                            std::terminate();
                        }
                        --runtime.http1Operations;
                    }
                } h1Operation{*this, runtime, mustBuffer};
                auto h1Acquired = co_await connection.http2Runtime->http1Scheduler.acquire(
                    acquireTimeout.remaining(), options.stopToken, worker_);
                if (h1Operation.buffered) {
                    --requestsBuffered_;
                    ++requestsInFlight_;
                    h1Operation.buffered = false;
                }
                switch (h1Acquired.status()) {
                    case ruvia::PoolWaiterResult::Status::kAcquired:
                        break;
                    case ruvia::PoolWaiterResult::Status::kTimedOut:
                        throw HttpClientError(HttpClientError::Code::kTimeout,
                            "HTTP/1 connection acquire timed out");
                    case ruvia::PoolWaiterResult::Status::kCancelled:
                        throw HttpClientError(
                            HttpClientError::Code::kCancelled, "HTTP/1 request cancelled");
                    case ruvia::PoolWaiterResult::Status::kClosed:
                        throw HttpClientError(
                            HttpClientError::Code::kClosing, "HTTP client pool is closing");
                    default:
                        std::terminate();
                }
                const auto h1Slot = h1Acquired.index();
                struct H1Release final {
                    PoolLeaseScheduler& scheduler;
                    std::size_t slot;
                    ~H1Release() {
                        (void)scheduler.release(slot);
                    }
                } h1Release{connection.http2Runtime->http1Scheduler, h1Slot};

                // A previous HTTP/1 exchange may have closed this connection while
                // this request waited for the serial exchange slot. Reconnect only
                // after acquiring that slot, so the connection cannot be replaced
                // underneath an active HTTP/1 exchange.
                discardConnection = true;
                try {
                    co_await ensureConnected(
                        connection, timeout, acquireTimeout, options.stopToken);
                } catch (...) {
                    close(connection);
                    discardConnection = false;
                    throw;
                }
                if (connection.protocol == WireProtocol::kHttp2) {
                    retryAsHttp2 = true;
                } else {
                    connection.abortReason = AbortReason::kNone;
                    ++connection.generation;
                    if (connection.generation == 0) {
                        ++connection.generation;
                    }
                    struct H1CancellationGeneration final {
                        Connection& connection;
                        ~H1CancellationGeneration() {
                            ++connection.generation;
                        }
                    } cancellationGeneration{connection};
                    connection.cancellationId = 0;
                    std::uint64_t cancellationId = 0;
                    StopRegistration stopRegistration;
                    if (options.stopToken.stoppable()) {
                        cancellationId = cancellationMailbox_->nextOperationId();
                        connection.cancellationId = cancellationId;
                        state->cancellationId = cancellationId;
                        options.stopToken.registerCallback(stopRegistration,
                            WorkerCancellationPost<HttpClientOperationCancellationMailbox>(
                                cancellationMailbox_, cancellationId));
                    }
                    struct H1CancellationRegistrationGuard final {
                        Connection& connection;
                        std::uint64_t cancellationId;
                        StopRegistration& registration;

                        ~H1CancellationRegistrationGuard() {
                            if (connection.cancellationId == cancellationId) {
                                connection.cancellationId = 0;
                            }
                            registration.reset();
                        }
                    } cancellationRegistrationGuard{connection, cancellationId, stopRegistration};
                    if (options.stopToken.stopRequested()) {
                        cancelOperationById(cancellationId);
                    }
                    try {
                        co_await executeHttp1(connection, request, timeout, response);
                    } catch (...) {
                        // Release the serial exchange slot only after the failed
                        // request has invalidated its connection. Pool handoff
                        // resumes the next waiter synchronously.
                        close(connection);
                        discardConnection = false;
                        throw;
                    }
                }
            }
            if (retryAsHttp2) {
                discardConnection = false;
                co_await executeHttp2(connection, request, timeout, options.stopToken, response);
            }
        }
        retainResponseCookies(request, response);
        --requestsInFlight_;
        ++completedRequests_;
        co_return;
    } catch (...) {
        --requestsInFlight_;
        ++failedRequests_;
        if (discardConnection) {
            lease.discard();
        }
        throw;
    }
}

void HttpClientPool::abandonResponse(HttpClientResponseState& state) noexcept {
    if (state.abandoned) {
        return;
    }
    state.abandoned = true;
    switch (state.transport) {
        case HttpClientResponseTransport::kHttp2:
            if (state.connectionIndex < connections_.size() && state.requestId != 0) {
                cancelHttp2Stream(
                    connections_[state.connectionIndex], state.requestId, AbortReason::kCancelled);
            }
            break;
        case HttpClientResponseTransport::kHttp3:
            if (state.http3Connection == nullptr || state.http3RequestId == 0) {
                std::terminate();
            }
            state.http3Connection->abandonResponse(state.http3RequestId);
            break;
        case HttpClientResponseTransport::kUnassigned:
        case HttpClientResponseTransport::kHttp1:
            if (state.cancellationId != 0) {
                cancelOperationById(state.cancellationId);
            }
            break;
    }
    state.spaceSignal.notify();
}

void HttpClientPool::releaseResponseData(HttpClientResponseState& state) noexcept {
    if (!state.http2DataCredit) {
        return;
    }
    // The token returns credit to its original connection even if that session
    // has already retired. Its destructor also defers allocation failures.
    state.http2DataCredit.reset();
    if (state.connectionIndex < connections_.size()) {
        connections_[state.connectionIndex].http2Runtime->writeSignal.notify();
    }
}

}  // namespace ruvia::detail
