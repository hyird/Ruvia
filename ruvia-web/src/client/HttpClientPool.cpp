#include "client/HttpClientPool.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
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
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpRequestTarget.h"

#include "client/ClientTransport.h"
#include "client/HttpClientResponseState.h"
#include "client/HttpClientResultBudget.h"
#include "http3/Http3ClientConnection.h"

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

std::size_t httpClientSchedulerSlots(const HttpClientConfigStorage& config) noexcept {
    if (config.protocol == HttpClientProtocol::kHttp1Only) {
        return config.connectionCount;
    }
    if (config.protocol == HttpClientProtocol::kHttp3Only) {
        return config.connectionCount * client_quic_connections::requests_per_connection;
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

static_assert(worker_cancellation_post_is_inline<http_client_cancellation_target>);

HttpClientPool::Connection::Connection(asio::io_context& ioContext, asio::ssl::context& tlsContext,
    const WorkerHandle& worker, const HttpClientConfigStorage& config, client_wire_counters& counters,
    std::pmr::memory_resource* resource)
    : transport(ioContext, tlsContext, worker, config, counters, resource),
      readBuffer(pmrResourceOrDefault(resource)),
      writeBuffer(pmrResourceOrDefault(resource)),
      http2(nullptr, PmrObjectDeleter<::ruvia::Http2Connection>{pmrResourceOrDefault(resource)}),
      http2Runtime(makePmrObject<Http2Runtime>(resource, worker, resource)) {
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
      policy_(config_, resource_),
      advertisements_(worker_, config_.advertisements, resource_),
      pushes_(resource_),
      resultBudgetDomain_(requireHttpClientResultBudgetDomain(resultBudgetDomain)),
      tlsContext_(asio::ssl::context::tls_client),
      connections_(resource_),
      scheduler_(httpClientSchedulerSlots(config_), worker_, resource_),
      backgroundTasks_(worker_, {.resource = resource_}),

      quic_(ioContext_, worker_, backgroundTasks_, config_, advertisements_,
          config_.push.enabled ? Http3ClientPushObserver{
                                     .context = this,
                                     .config = config_.push,
                                     .receive = [](void* raw, std::size_t slot, Http3ClientConnection& connection, std::uint64_t id, const Http3MessageHead& head) { return static_cast<HttpClientPool*>(raw)->acceptHttp3Push(slot, connection, id, head); },
                                     .finished = [](void* raw) noexcept { --static_cast<HttpClientPool*>(raw)->activePushes_; },
                                 }
                               : Http3ClientPushObserver{},
          resource_) {
    if (config_.protocol != HttpClientProtocol::kHttp3Only) {
        if (config_.scheme == HttpScheme::kHttps) {
            configure_client_tls_context(*tlsContext_.native_handle(), config_.transport.view());
        }
        connections_.reserve(config_.connectionCount);
        for (std::size_t i = 0; i < config_.connectionCount; ++i) {
            connections_.emplace_back(ioContext_, tlsContext_, worker_, config_, wire_counters_, resource_);
        }
    }
    cancellation_target_ = make_worker_cancellation_target(*this, worker_);
    responseMemory_ = HttpClientResponseMemoryDomain::create(worker_, resultBudgetDomain_);
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
    connection.transport.stop_output();
    auto& runtime = *connection.http2Runtime;
    if (runtime.running) {
        // A multiplexed connection owns background reader/writer tasks. Route
        // every terminal close through their shared failure transition so both
        // drivers and all pending streams are woken before join().
        failHttp2Session(
            connection, runtime.generation, std::make_error_code(std::errc::operation_canceled));
        return;
    }
    connection.transport.close();
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
    cancellation_target_->detach(*this);
    if (!scheduler_.close()) {
        return;
    }
    quic_.request_stop();
    backgroundTasks_.requestStop();
    for (auto& connection : connections_) {
        connection.transport.abort_output(client_abort_reason::closing);
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
    std::exception_ptr failure;
    try {
        co_await backgroundTasks_.join();
    } catch (...) {
        // A child failure is observable only after join retired every child.
        // If join itself could not start, its live drivers still borrow the
        // transport owners below: this is a terminal ownership violation.
        if (backgroundTasks_.size() != 0) {
            std::terminate();
        }
        failure = std::current_exception();
    }
    // Consumers own their response storage independently of transport. Retire
    // every client borrow, including completed responses, before its owners.
    responseMemory_->detachTransportBindings(*this);
    pushes_.clear();
    advertisements_.retire();
    responseMemory_.reset();
    // Destroy QUIC SSL/socket/session owners while the worker loop and its PMR
    // owner are still alive. HttpClientPool itself is later destroyed by the
    // App lifecycle thread after the worker has joined.
    quic_.retire();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

ruvia::quic_path_migration HttpClientPool::start_quic_path_migration(const asio::ip::udp::endpoint& endpoint) {
    if (backgroundJoined_) {
        return {.status = ruvia::quic_migration_status::rejected};
    }
    return quic_.start_path_migration(endpoint);
}
std::optional<ruvia::quic_path_migration> HttpClientPool::path_migration(std::uint64_t id) const noexcept {
    return quic_.path_migration(id);
}
ruvia::quic_operation_status HttpClientPool::cancel_quic_path_migration(std::uint64_t id) {
    return quic_.cancel_path_migration(id);
}

HttpClientStats HttpClientPool::stats() const noexcept {
    return {requestsBuffered_, requestsInFlight_, completedRequests_, failedRequests_, wire_counters_.sent,
        wire_counters_.received, advertisements_.dropped(), receivedPushes_, rejectedPushes_};
}

std::uint16_t HttpClientPool::port() const noexcept {
    return httpClientPort(config_);
}

Task<std::size_t> HttpClientPool::acquire(const ruvia::OperationTimeout& timeout, StopToken stopToken) {
    auto result = co_await scheduler_.acquire(
        timeout.constrainedBy(config_.acquireTimeout).remaining(), std::move(stopToken));
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
    if (quic_.cancel(cancellationId)) {
        return;
    }
    for (std::size_t index = 0; index < connections_.size(); ++index) {
        auto& connection = connections_[index];
        if (connection.cancellationId == cancellationId) {
            connection.cancellationId = 0;
            cancelOperation(index, connection.generation, client_abort_reason::cancelled);
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
            cancelHttp2Stream(connection, (*pending)->requestId, client_abort_reason::cancelled);
            return;
        }
    }
}

void HttpClientPool::cancelOperation(
    std::size_t index, std::uint64_t generation, client_abort_reason reason) noexcept {
    if (connections_.empty()) {
        return;
    }
    auto& connection = connections_[index % connections_.size()];
    if (connection.generation != generation) {
        return;
    }
    connection.transport.cancel(reason);
    connection.connected = false;
    connection.protocol = WireProtocol::kUnknown;
    connection.http2Runtime->writeSignal.notify();
    connection.http2Runtime->stateSignal.notify();
}

Task<void> HttpClientPool::ensureConnected(Connection& connection,
    const ruvia::OperationTimeout& operationTimeout, const ruvia::OperationTimeout& acquireTimeout,
    StopToken stopToken) {
    auto& runtime = *connection.http2Runtime;
    auto acquired =
        co_await runtime.connectScheduler.acquire(acquireTimeout.remaining(), stopToken);
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
    connection.transport.reset_abort();
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
    worker_cancellation_registration cancellation(cancellation_target_, connection.cancellationId);
    cancellation.arm(stopToken);
    while (runtime.sessionTasks != 0 || !runtime.pending.empty()) {
        connection.transport.throw_if_aborted();
        co_await runtime.stateSignal.wait();
    }
    connection.transport.throw_if_aborted();
    connection.transport.prepare_reconnect(tlsContext_);
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
    if (!connection.transport.arm_deadline(timeout, client_deadline_kind::resolve)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client resolve timed out");
    }
    auto resolve = co_await ruvia::asyncAsio<asio::ip::tcp::resolver::results_type>(
        [&connection, this, port](auto handler) mutable {
            connection.transport.resolver().async_resolve(config_.host, port, std::move(handler));
        });
    const bool resolveTimedOut = connection.transport.clear_deadline() || timeout.expired();
    connection.transport.throw_if_aborted();
    if (resolveTimedOut) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client resolve timed out");
    }
    if (resolve.errorCode()) {
        throw HttpClientError(HttpClientError::Code::kResolveFailed, resolve.errorCode().message());
    }
    auto endpoints = std::move(resolve).takeResult();

    if (!connection.transport.arm_deadline(timeout, client_deadline_kind::socket)) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client connect timed out");
    }
    auto connected = co_await ruvia::asyncAsio([&connection, &endpoints](auto handler) mutable {
        asio::async_connect(connection.transport.stream().lowest_layer(), endpoints, std::move(handler));
    });
    const bool connectTimedOut = connection.transport.clear_deadline() || timeout.expired();
    connection.transport.throw_if_aborted();
    if (connectTimedOut) {
        throw HttpClientError(HttpClientError::Code::kTimeout, "http client connect timed out");
    }
    if (connected.errorCode()) {
        throw HttpClientError(
            HttpClientError::Code::kConnectFailed, connected.errorCode().message());
    }
    const auto transport = config_.transport.view();
    ruvia::applyTcpSocketPolicies(
        connection.transport.stream().next_layer(), transport.tcpNoDelay, transport.tcpKeepAlive);

    if (config_.scheme == HttpScheme::kHttps) {
        connection.transport.mark_tls_started();
        const auto tlsSetup = prepareClientTlsStream(
            connection.transport.stream(), config_.host, transport, clientAlpnMode(config_.protocol));
        if (tlsSetup != ClientTlsSetupError::kNone) {
            throw HttpClientError(
                HttpClientError::Code::kTlsFailed, clientTlsSetupErrorMessage(tlsSetup));
        }
        if (!connection.transport.arm_deadline(timeout, client_deadline_kind::socket)) {
            throw HttpClientError(
                HttpClientError::Code::kTimeout, "http client TLS handshake timed out");
        }
        auto handshake = co_await ruvia::asyncAsio([&connection](auto handler) mutable {
            connection.transport.stream().async_handshake(asio::ssl::stream_base::client, std::move(handler));
        });
        const bool handshakeTimedOut = connection.transport.clear_deadline() || timeout.expired();
        connection.transport.throw_if_aborted();
        if (handshakeTimedOut) {
            throw HttpClientError(
                HttpClientError::Code::kTimeout, "http client TLS handshake timed out");
        }
        if (handshake.errorCode()) {
            throw HttpClientError(
                HttpClientError::Code::kTlsFailed, handshake.errorCode().message());
        }
        const auto alpn = selectedClientAlpn(connection.transport.stream().native_handle());
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
    policy_.append_headers(request, headers, cookieHeader);

    HttpClientRequestStorage wire(source.method.view(), source.target.view(), resource_);
    wire.set_replay_safe(source.replay_safe);
    for (const auto& header : headers) {
        wire.appendHeader(header.name(), header.value());
    }
    if (const auto* bytes = source.content.borrowedBytes()) {
        wire.setBody(bytes->value());
    }
    if (request.upload() != nullptr) {
        wire.bindUpload(*request.upload());
    }
    if (request.isTunnel()) {
        wire.setTunnel(request.tunnelAuthority(), request.tunnelProtocol());
    }
    if (request.tunnel() != nullptr) {
        wire.bindTunnel(*request.tunnel());
    }
    return wire;
}

HttpClientResponseState* HttpClientPool::acceptHttp3Push(std::size_t slot, Http3ClientConnection& connection,
    std::uint64_t requestId, const Http3MessageHead& head) noexcept {
    try {
        const auto origin = HttpOriginView::https({.host = config_.host, .port = config_.port});
        const auto authority = makeHttpOriginAuthority(origin, resource_);
        if (pushes_.size() >= config_.push.maxQueuedPushes || activePushes_ >= config_.push.maxConcurrentPushes ||
            !httpAsciiEqualsIgnoreCase(head.scheme, "https") ||
            !httpAuthoritiesEqual(BorrowedText(std::string_view(head.authority)), BorrowedText(std::string_view(authority)), 443)) {
            ++rejectedPushes_;
            return nullptr;
        }
        HttpClientResponse response(*this);
        auto& state = *response.state_;
        state.bufferedLimit = config_.maxResponseBytes;
        state.requestMethod = classifyHttpMethod(head.method);
        state.protocolVersion = HttpProtocolVersion::kHttp3;
        state.connectionIndex = slot;
        state.promisedRequest.emplace(state.resource);
        auto& request = *state.promisedRequest;
        request.method = head.method;
        request.scheme = head.scheme;
        request.authority = head.authority;
        request.path = head.path;
        request.headers.reserve(head.headers.size());
        for (const auto& field : head.headers) {
            request.headers.push_back(HttpHeader::copyOf(field.name, field.value, state.resource));
        }
        // Publish only after every borrowed promise field has been copied. If
        // queue allocation fails, the still-unbound local response owns cleanup.
        pushes_.push_back(HttpClientPush(std::move(response)));
        state.transport = HttpClientResponseTransport::kHttp3;
        state.http3Connection = &connection;
        state.http3RequestId = requestId;
        state.requestId = requestId;
        state.retainReference();
        ++activePushes_;
        ++receivedPushes_;
        return &state;
    } catch (...) {
        ++rejectedPushes_;
        return nullptr;
    }
}

Task<void> HttpClientPool::executeHttp3(std::size_t connectionIndex,
    const HttpClientRequestStorage& request, const ruvia::OperationTimeout& timeout,
    StopToken stopToken, HttpClientResponse& response) {
    struct CancellationGuard final {
        client_quic_connections& connections;
        worker_cancellation_registration<http_client_cancellation_target>& registration;
        std::uint64_t id;

        ~CancellationGuard() {
            registration.reset();
            connections.unregister_cancellation(id);
        }
    };

    std::optional<HttpClientRequestStorage> wire;
    wire.emplace(makeHttp3Request(request));
    auto absoluteDeadline = timeout.deadline();
    bool retriedRejectedRequest = false;
    const auto connectionCount = quic_.size();
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
                auto& candidate = quic_.acquire(candidateIndex);
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
                (worker_).schedule_timer(timer, workerTimerDeadlineAfter(*remaining), [this](WorkerTimerOutcome outcome) noexcept {
                    if (outcome == WorkerTimerOutcome::kExpired) {
                        quic_.generation_signal().notify();
                    }
                });
            }
            worker_cancellation_registration registration(cancellation_target_, response.state_->cancellationId);
            const auto cancellationId = registration.id();
            quic_.register_cancellation(cancellationId, nullptr, 0);
            CancellationGuard cancellation{quic_, registration, cancellationId};
            registration.arm(stopToken);
            co_await quic_.generation_signal().wait();
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
            worker_cancellation_registration registration(cancellation_target_, response.state_->cancellationId);
            const auto cancellationId = registration.id();
            quic_.register_cancellation(cancellationId, connection, submission.id);
            CancellationGuard cancellation{quic_, registration, cancellationId};
            registration.arm(stopToken);

            co_await connection->wait(submission.id);
            if (response.state_->http3Connection != connection) {
                // The public response may be abandoned and retired by its
                // waiter before this terminal notification is dispatched.
                quic_.generation_signal().notify();
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
        quic_.generation_signal().notify();

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

void HttpClientPool::reprioritize(HttpClientResponseState& state, HttpPriority priority) {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    if (state.transport == HttpClientResponseTransport::kHttp3 && state.http3Connection != nullptr) {
        if (!state.http3Connection->reprioritize(state.http3RequestId, priority)) {
            throw HttpClientError(HttpClientError::Code::kQueueFull, "HTTP/3 priority update could not be queued");
        }
        return;
    }
    if (state.transport != HttpClientResponseTransport::kHttp2 || state.connectionIndex >= connections_.size()) {
        throw HttpClientError(HttpClientError::Code::kProtocolUnavailable, "priority updates require HTTP/2 or HTTP/3");
    }
    auto& connection = connections_[state.connectionIndex];
    if (!connection.http2 || !connection.http2Runtime->running || connection.http2Runtime->failed) {
        throw HttpClientError(HttpClientError::Code::kClosing, "HTTP/2 response connection is retired");
    }
    const auto status = connection.http2->submitPriorityUpdate(static_cast<std::uint32_t>(state.streamId),
        {.urgency = priority.urgency, .incremental = priority.incremental});
    if (status != Http2SubmitStatus::kAccepted) {
        throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP/2 priority update rejected");
    }
    connection.http2Runtime->writeSignal.notify();
}

std::optional<HttpClientPush> HttpClientPool::nextPush() {
    if (!worker_.isCurrent()) {
        throw std::logic_error("push requires its owner worker");
    }
    if (pushes_.empty()) {
        return std::nullopt;
    }
    auto result = std::move(pushes_.front());
    pushes_.pop_front();
    return result;
}

Task<HttpClientTunnelResult> HttpClientPool::openTunnel(HttpClientRequestStorage request, HttpClientTunnelConfig config, OperationOptions options) {
    if (!responseMemory_) {
        throw HttpClientError(HttpClientError::Code::kClosing, "HTTP client pool is retired");
    }
    auto ownedRequest = std::move(request).intoResource(resource_);
    HttpClientResponse response(*this);
    auto* state = response.state_;
    state->bufferedLimit = config_.maxResponseBytes;
    state->tunnel.emplace(state->memoryDomain()->worker(), state->resource, config);
    state->tunnel->udp = ownedRequest.tunnelProtocol() == "connect-udp";
    if (state->tunnel->udp) {
        state->tunnel->config.datagrams = true;
    }
    ownedRequest.bindTunnel(*state->tunnel);
    backgroundTasks_.spawn(executeInto(std::move(ownedRequest), std::move(options), state));
    while (!state->headReady && !state->failure && !state->errorCode) {
        co_await state->headSignal.wait();
    }
    if (state->failure) {
        std::rethrow_exception(state->failure);
    }
    if (state->errorCode) {
        throw HttpClientError(static_cast<HttpClientError::Code>(*state->errorCode), "CONNECT handshake failed");
    }
    if (state->tunnel->accepted) {
        co_return HttpClientTunnelResult(HttpClientTunnel(std::move(response)));
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
            throw HttpClientError(static_cast<HttpClientError::Code>(*state->errorCode), "CONNECT rejection body failed");
        }
    }
    co_return HttpClientTunnelResult(std::move(response));
}

Task<HttpClientExchange> HttpClientPool::openRequest(HttpClientRequestStorage request, HttpClientUploadConfig upload, OperationOptions options) {
    if (!responseMemory_) {
        throw HttpClientError(HttpClientError::Code::kClosing, "HTTP client pool is retired");
    }
    auto ownedRequest = std::move(request).intoResource(resource_);
    HttpClientResponse response(*this);
    auto* state = response.state_;
    state->bufferedLimit = config_.maxResponseBytes;
    state->upload.emplace(state->memoryDomain()->worker(), state->resource, upload);
    state->upload->contentReleased = upload.expectation == HttpClientRequestExpectation::kNone;
    ownedRequest.bindUpload(*state->upload);
    backgroundTasks_.spawn(executeInto(std::move(ownedRequest), std::move(options), state));
    co_return HttpClientExchange(std::move(response));
}

Task<HttpClientResponse> HttpClientPool::execute(
    HttpClientRequestStorage request, OperationOptions options) {
    if (!responseMemory_) {
        throw HttpClientError(HttpClientError::Code::kClosing, "HTTP client pool is retired");
    }
    // The request owns all data before transport work can outlive the caller.
    auto ownedRequest = std::move(request).intoResource(resource_);
    HttpClientResponse response(*this);
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
        if (state->abandoned) {
            throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP request abandoned before transport admission");
        }
        co_await executeRequestInto(std::move(request), std::move(options), state);
    } catch (const HttpClientError&) {
        state->failure = std::current_exception();
    } catch (...) {
        state->failure = std::current_exception();
    }
    state->complete = true;
    if (auto* output = state->output()) {
        output->stop();
    }
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
        state->connectionIndex = index % quic_.size();
        try {
            co_await executeHttp3(
                state->connectionIndex, request, timeout, options.stopToken, response);
            if (state->headReady && !state->failure && !state->errorCode) {
                policy_.retain_response_cookies(request, response.headers());
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
        // A cancelled HTTP/1 producer still owns its SSL stream until its
        // serialized exchange unwinds. Join that exchange before reconnecting,
        // including when negotiated HTTP/2-capacity leases share the pool slot.
        if (connection.connected || connection.http2Runtime->http1Operations == 0) {
            co_await ensureConnected(connection, timeout, acquireTimeout, options.stopToken);
        }
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
                    acquireTimeout.remaining(), options.stopToken);
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
                    connection.transport.reset_abort();
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
                    worker_cancellation_registration cancellation(cancellation_target_, connection.cancellationId);
                    state->cancellationId = cancellation.id();
                    cancellation.arm(options.stopToken);
                    connection.transport.bind_response(state);
                    struct ActiveHttp1ResponseGuard final {
                        Connection& connection;
                        ~ActiveHttp1ResponseGuard() {
                            connection.transport.bind_response(nullptr);
                        }
                    } activeHttp1ResponseGuard{connection};
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
        policy_.retain_response_cookies(request, response.headers());
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
                    connections_[state.connectionIndex], state.requestId, client_abort_reason::cancelled);
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
            } else if (state.connectionIndex < connections_.size() && connections_[state.connectionIndex].transport.response() == &state) {
                close(connections_[state.connectionIndex]);
            }
            break;
    }
    if (auto* output = state.output()) {
        output->stop();
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
