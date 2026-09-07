#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <ranges>
#include <system_error>
#include <utility>

#include <asio/connect.hpp>
#include <asio/ssl/error.hpp>
#include <asio/write.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/io/TcpSocketOptions.h"
#include "ruvia/core/detail/worker/WorkerCancellationPost.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/client/HttpClientContentEncoding.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include "client/HttpClientResponseState.h"

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
    }
    std::terminate();
}

std::size_t httpClientSchedulerSlots(const HttpClientConfigStorage& config) noexcept {
    if (config.protocol == HttpClientProtocol::kHttp1Only) {
        return config.connectionCount;
    }
    return config.connectionCount * config.maxConcurrentHttp2StreamsPerConnection;
}

}  // namespace

static_assert(workerCancellationPostIsInline<HttpClientOperationCancellationMailbox>);

HttpClientPool::Connection::Connection(asio::io_context& ioContext, asio::ssl::context& tlsContext,
    const WorkerHandle& worker, std::pmr::memory_resource* resource)
    : resolver(ioContext),
      stream(ioContext, tlsContext),
      readBuffer(pmrResourceOrDefault(resource)),
      writeBuffer(pmrResourceOrDefault(resource)),
      http2(nullptr, PmrObjectDeleter<Http2Connection>{pmrResourceOrDefault(resource)}),
      http2Runtime(makePmrObject<Http2Runtime>(resource, worker, resource)),
      deadlineTimer(makePmrObject<WorkerTimerRegistration>(resource)) {
    readBuffer.reserve(kConnectionReadBufferInitialBytes);
}

HttpClientPool::Connection::~Connection() = default;
HttpClientPool::Connection::Connection(Connection&&) noexcept = default;

HttpClientPool::HttpClientPool(asio::io_context& ioContext, const WorkerHandle& worker,
    HttpClientConfigStorage config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      worker_(worker),
      resource_(pmrResourceOrDefault(resource)),
      config_(std::move(config)),
      tlsContext_(asio::ssl::context::tls_client),
      connections_(resource_),
      scheduler_(httpClientSchedulerSlots(config_), worker_, resource_),
      backgroundTasks_(worker_, {.resource = resource_}),
      cookies_(resource_) {
    for (const auto& [name, value] : config_.cookies) {
        addCookie(name, value);
    }
    if (config_.scheme == HttpScheme::kHttps) {
        configureClientTlsContext(tlsContext_, config_.transport.view());
    }
    connections_.reserve(config_.connectionCount);
    for (std::size_t i = 0; i < config_.connectionCount; ++i) {
        connections_.emplace_back(ioContext_, tlsContext_, worker_, resource_);
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
}

HttpClientStats HttpClientPool::stats() const noexcept {
    return {requestsBuffered_, requestsInFlight_, completedRequests_, failedRequests_, bytesSent_,
        bytesReceived_};
}

std::uint16_t HttpClientPool::port() const noexcept {
    return httpClientPort(config_);
}

Task<std::size_t> HttpClientPool::acquire(const OperationTimeout& timeout, StopToken stopToken) {
    auto result = co_await scheduler_.acquire(
        timeout.constrainedBy(config_.acquireTimeout).remaining(), std::move(stopToken), worker_);
    if (result.timedOut()) {
        throw HttpClientError(
            HttpClientError::Code::kTimeout, "http client connection pool acquire timed out");
    }
    if (result.cancelled()) {
        throw HttpClientError(HttpClientError::Code::kCancelled, "http client request cancelled");
    }
    if (result.closed()) {
        throw HttpClientError(HttpClientError::Code::kClosing, "http client pool is closing");
    }
    if (!result.acquired()) {
        std::terminate();
    }
    co_return result.acquired()->index();
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
    Connection& connection, const OperationTimeout& timeout, DeadlineKind kind) {
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
    Connection& connection, std::string_view bytes, const OperationTimeout& timeout) {
    if (bytes.empty()) {
        co_return;
    }
    const auto writeTimeout = timeout.constrainedBy(config_.writeTimeout);
    if (!armDeadline(connection, writeTimeout, DeadlineKind::kSocket)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client write timed out");
    }
    AsioCompletion<std::size_t> completion =
        config_.scheme == HttpScheme::kHttps
            ? co_await asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable {
                  asio::async_write(connection.stream, asio::buffer(bytes), std::move(handler));
              })
            : co_await asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable {
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
    Connection& connection, std::span<char> bytes, const OperationTimeout& timeout, bool allowEof) {
    if (!armDeadline(connection, timeout, DeadlineKind::kSocket)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client request timed out");
    }
    AsioCompletion<std::size_t> completion =
        config_.scheme == HttpScheme::kHttps
            ? co_await asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable {
                  connection.stream.async_read_some(
                      asio::buffer(bytes.data(), bytes.size()), std::move(handler));
              })
            : co_await asyncAsio<std::size_t>([&connection, bytes](auto handler) mutable {
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
    const OperationTimeout& operationTimeout, const OperationTimeout& acquireTimeout,
    StopToken stopToken) {
    auto& runtime = *connection.http2Runtime;
    auto acquired =
        co_await runtime.connectScheduler.acquire(acquireTimeout.remaining(), stopToken, worker_);
    if (acquired.timedOut()) {
        throw HttpClientError(
            HttpClientError::Code::kTimeout, "HTTP client connect wait timed out");
    }
    if (acquired.cancelled()) {
        throw HttpClientError(
            HttpClientError::Code::kCancelled, "HTTP client connect wait cancelled");
    }
    if (!acquired.acquired()) {
        throw HttpClientError(HttpClientError::Code::kClosing, "HTTP client pool is closing");
    }
    const auto connectSlot = acquired.acquired()->index();
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
    auto resolve = co_await asyncAsio<asio::ip::tcp::resolver::results_type>(
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
    auto connected = co_await asyncAsio([&connection, &endpoints](auto handler) mutable {
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
    configureTcpSocketOptions(
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
        auto handshake = co_await asyncAsio([&connection](auto handler) mutable {
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

Task<HttpClientResponse> HttpClientPool::execute(
    HttpClientRequestStorage request, OperationOptions options) {
    // The public request is copied from request-local borrowed storage. Both
    // request and response state use worker-owned storage because transport
    // work may continue beyond the handler frame.
    std::pmr::vector<HttpHeaderView> requestHeaders(resource_);
    const auto requestView = HttpClientRequestStorageAccess::view(request, requestHeaders);
    HttpClientRequestStorage ownedRequest(
        requestView.method.view(), requestView.target.view(), resource_);
    for (const auto& header : requestView.headers) {
        ownedRequest.appendHeader(header.name(), header.value());
    }
    if (const auto* bytes = requestView.content.borrowedBytes()) {
        ownedRequest.setBody(bytes->value());
    }
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
    const auto contentCoding = httpClientContentCodingOf(state->headers);
    if (contentCoding.coding() == nullptr ||
        *contentCoding.coding() != HttpContentCoding::kIdentity) {
        state->collectAll = true;
        if (state->http2DataPending) {
            releaseResponseData(*state);
        }
        state->spaceSignal.notify();
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
    const OperationTimeout timeout(
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
                if (h1Acquired.timedOut()) {
                    throw HttpClientError(
                        HttpClientError::Code::kTimeout, "HTTP/1 connection acquire timed out");
                }
                if (h1Acquired.cancelled()) {
                    throw HttpClientError(
                        HttpClientError::Code::kCancelled, "HTTP/1 request cancelled");
                }
                if (!h1Acquired.acquired()) {
                    throw HttpClientError(
                        HttpClientError::Code::kClosing, "HTTP client pool is closing");
                }
                const auto h1Slot = h1Acquired.acquired()->index();
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
    if (state.http2) {
        if (state.connectionIndex < connections_.size() && state.requestId != 0) {
            cancelHttp2Stream(
                connections_[state.connectionIndex], state.requestId, AbortReason::kCancelled);
        }
    } else if (state.cancellationId != 0) {
        cancelOperationById(state.cancellationId);
    }
    state.spaceSignal.notify();
}

void HttpClientPool::releaseResponseData(HttpClientResponseState& state) noexcept {
    if (!state.http2DataPending || state.connectionIndex >= connections_.size() ||
        state.streamId == 0) {
        return;
    }
    auto& connection = connections_[state.connectionIndex];
    if (connection.http2) {
        connection.http2->releaseAllReceivedData(state.streamId);
        connection.http2Runtime->writeSignal.notify();
    }
    state.http2DataPending = false;
}

}  // namespace ruvia::detail
