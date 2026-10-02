#include <algorithm>
#include <array>

#include <asio/write.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/WorkerCancellationPost.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpRequestTarget.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseDecoding.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::pmr::string http2Authority(
    const HttpClientConfigStorage& config, std::pmr::memory_resource* resource) {
    auto authority = clientUriHost(config.host, resource);
    const auto port = httpClientPort(config);
    const auto defaultPort = config.scheme == HttpScheme::kHttps ? 443 : 80;
    if (port != defaultPort) {
        authority.push_back(':');
        ClientPortTextBuffer portBuffer{};
        authority.append(formatClientPort(port, portBuffer));
    }
    return authority;
}

}  // namespace

Task<void> HttpClientPool::initializeHttp2(
    Connection& connection, const ruvia::OperationTimeout& timeout) {
    connection.http2 = makePmrObject<::ruvia::Http2Connection>(
        resource_, ::ruvia::Http2Connection::client({.resource = resource_, .enablePush = config_.push.enabled, .receiveOriginAdvertisements = config_.advertisements.receiveOrigins}));
    while (connection.http2->wantsWrite()) {
        const auto output = connection.http2->pendingOutput();
        co_await write(connection, output, timeout);
        (void)connection.http2->consumeOutput(output.size());
    }
    std::array<char, 16384> input{};
    while (!connection.http2->receivedPeerSettings()) {
        const auto bytes = co_await readSome(connection, input, timeout);
        if (bytes == 0) {
            throw HttpClientError(
                HttpClientError::Code::kIoError, "upstream closed during HTTP/2 preface");
        }
        const auto status = connection.http2->feed(std::string_view(input.data(), bytes));
        while (auto event = connection.http2->nextEvent()) {
            retainHttp2Advertisement(connection, *event);
        }
        if (status == Http2FeedResult::kProtocolFailure || connection.http2->connectionError()) {
            throw HttpClientError(
                HttpClientError::Code::kProtocolError, "invalid HTTP/2 connection preface");
        }
        while (connection.http2->wantsWrite()) {
            const auto output = connection.http2->pendingOutput();
            co_await write(connection, output, timeout);
            (void)connection.http2->consumeOutput(output.size());
        }
    }
    auto& runtime = *connection.http2Runtime;
    auto generation = ++runtime.generation;
    if (generation == 0) {
        generation = ++runtime.generation;
    }
    runtime.running = true;
    runtime.draining = false;
    runtime.failed = false;
    runtime.sessionTasks = 0;
    try {
        ++runtime.sessionTasks;
        try {
            backgroundTasks_.spawn(runHttp2Reader(connection, generation));
        } catch (...) {
            --runtime.sessionTasks;
            throw;
        }
        ++runtime.sessionTasks;
        try {
            backgroundTasks_.spawn(runHttp2Writer(connection, generation));
        } catch (...) {
            --runtime.sessionTasks;
            throw;
        }
    } catch (...) {
        const auto failure = std::current_exception();
        failHttp2Session(connection, generation, {}, failure);
        std::rethrow_exception(failure);
    }
}

void HttpClientPool::retainHttp2Advertisement(Connection& connection, const Http2Event& event) {
    const auto slot = static_cast<std::size_t>(&connection - connections_.data());
    if (const auto* origins = event.originAdvertisement()) {
        (void)advertisements_.retain(slot, HttpProtocolVersion::kHttp2, *origins);
    } else if (const auto* service = event.alternativeServiceAdvertisement()) {
        (void)advertisements_.retain(slot, *service);
    }
}

HttpClientPool::Http2PushDriver::Http2PushDriver(HttpClientPool& pool, Connection& conn, HttpClientResponse value)
    : owner(pool),
      connection(conn),
      response(std::move(value)),
      timeout(pool.config_.push.timeout),
      pending(pool.worker_, response) {}
HttpClientPool::Http2PushDriver::~Http2PushDriver() {
    if (registered) {
        owner.removeHttp2Pending(connection, pending);
        --owner.activePushes_;
    }
}

void HttpClientPool::acceptHttp2Push(Connection& connection, const Http2PushPromiseEvent& promise) {
    const auto authority = http2Authority(config_, resource_);
    if (!config_.push.enabled || pushes_.size() >= config_.push.maxQueuedPushes ||
        activePushes_ >= config_.push.maxConcurrentPushes ||
        !httpAsciiEqualsIgnoreCase(promise.request.scheme, config_.scheme == HttpScheme::kHttps ? "https" : "http") ||
        !httpAuthoritiesEqual(BorrowedText(promise.request.authority), BorrowedText(std::string_view(authority)),
            config_.scheme == HttpScheme::kHttps ? 443 : 80)) {
        ++rejectedPushes_;
        submitHttp2Reset(connection, promise.promisedStreamId);
        return;
    }
    try {
        HttpClientResponse response(*this);
        auto& state = *response.state_;
        state.bufferedLimit = config_.maxResponseBytes;
        state.transport = HttpClientResponseTransport::kHttp2;
        state.requestMethod = classifyHttpMethod(promise.request.method);
        state.connectionIndex = static_cast<std::size_t>(&connection - connections_.data());
        state.streamId = promise.promisedStreamId;
        state.requestId = ++connection.http2Runtime->nextRequestId;
        if (state.requestId == 0) {
            state.requestId = ++connection.http2Runtime->nextRequestId;
        }
        state.promisedRequest.emplace(state.resource);
        auto& request = *state.promisedRequest;
        request.method = promise.request.method;
        request.scheme = promise.request.scheme;
        request.authority = promise.request.authority;
        request.path = promise.request.path;
        request.headers.reserve(promise.request.headers.size());
        for (const auto& field : promise.request.headers) {
            request.headers.push_back(HttpHeader::copyOf(field.name(), field.value(), state.resource));
        }
        auto driver = makePmrObject<Http2PushDriver>(resource_, *this, connection, HttpClientResponse(response.state_, true));
        driver->pending.timeout = &driver->timeout;
        driver->pending.requestId = state.requestId;
        driver->pending.streamId = promise.promisedStreamId;
        connection.http2Runtime->pending.push_back(&driver->pending);
        driver->registered = true;
        ++activePushes_;
        // Register before the reader drains another event. The driver is address
        // stable and its destructor unregisters on cold spawn failure as well.
        backgroundTasks_.spawn(runHttp2Push(std::move(driver)));
        pushes_.push_back(HttpClientPush(std::move(response)));
        ++receivedPushes_;
    } catch (...) {
        ++rejectedPushes_;
        submitHttp2Reset(connection, promise.promisedStreamId);
        // A resource admission failure is local to this push, not its parent.
    }
}
Task<void> HttpClientPool::runHttp2Push(std::unique_ptr<Http2PushDriver, PmrObjectDeleter<Http2PushDriver>> driver) {
    auto& pending = driver->pending;
    auto& state = *driver->response.state_;
    WorkerTimerRegistration timer;
    try {
        if (const auto remaining = driver->timeout.remaining()) {
            WorkerHandleAccess::scheduleTimer(worker_, timer, workerTimerDeadlineAfter(*remaining),
                [this, raw = driver.get()](WorkerTimerOutcome outcome) noexcept {
                    if (outcome == WorkerTimerOutcome::kExpired) {
                        cancelHttp2Stream(raw->connection, raw->pending.requestId, AbortReason::kTimeout);
                    }
                });
        }
        while (!pending.complete && !pending.failed()) {
            co_await pending.signal.wait();
        }
        timer.cancel();
        state.failure = pending.failure;
        if (pending.error) {
            state.errorCode = static_cast<std::uint8_t>(*pending.error);
        }
    } catch (...) {
        state.failure = std::current_exception();
        submitHttp2Reset(driver->connection, pending.streamId);
    }
    state.complete = true;
    state.headSignal.notify();
    state.dataSignal.notify();
    state.spaceSignal.notify();
}

void HttpClientPool::drainHttp2Events(Connection& connection) {
    if (!connection.http2) {
        return;
    }
    auto& runtime = *connection.http2Runtime;
    const auto findPending = [&runtime](std::uint32_t streamId) -> Http2PendingStream* {
        const auto match =
            std::ranges::find_if(runtime.pending, [streamId](const Http2PendingStream* pending) {
                return pending->streamId == streamId;
            });
        return match == runtime.pending.end() ? nullptr : *match;
    };
    const auto failPending = [this, &connection](Http2PendingStream& pending,
                                 std::uint32_t streamId, std::exception_ptr failure,
                                 bool resetStream) noexcept {
        if (pending.complete || pending.failed()) {
            return;
        }
        pending.failure = std::move(failure);
        if (resetStream) {
            submitHttp2Reset(connection, streamId);
        }
        pending.signal.notify();
    };
    bool releasedData = false;
    while (auto event = connection.http2->nextEvent()) {
        retainHttp2Advertisement(connection, *event);
        if (const auto* push = event->pushPromise()) {
            acceptHttp2Push(connection, *push);
        } else if (const auto* interim = event->informationalHead()) {
            auto* pending = findPending(interim->streamId());
            if (pending == nullptr || pending->complete || pending->failed()) {
                continue;
            }
            try {
                auto& state = *pending->response->state_;
                std::pmr::vector<HttpHeaderView> fields(state.resource);
                for (const auto& field : interim->head().headers()) {
                    fields.emplace_back(field.name(), field.value());
                }
                state.retainInformational(interim->head().status(), fields);
                if (state.upload && interim->requestContentSignal() == HttpClientRequestContentSignal::kContinue) {
                    state.upload->contentReleased = true;
                    pending->signal.notify();
                }
            } catch (...) {
                failPending(*pending, interim->streamId(), std::current_exception(), true);
            }
        } else if (auto* head = event->responseHead()) {
            auto* pending = findPending(head->streamId());
            if (pending == nullptr || pending->complete || pending->failed()) {
                continue;
            }
            try {
                auto responseHead = std::move(*head).takeHead();
                auto& state = *pending->response->state_;
                if (state.upload && !state.upload->ended) {
                    state.upload->stop();
                }
                state.status = responseHead.status();
                state.protocolVersion = responseHead.protocolVersion();
                state.responseBodyPlan = planHttpResponseBody(state.requestMethod, state.status);
                auto responseHeaders = std::move(responseHead).takeHeaders();
                state.headers.clear();
                state.headers.reserve(responseHeaders.size());
                for (const auto& header : responseHeaders) {
                    state.headers.push_back(HttpHeader::copyOf(
                        header.name(), header.value(), state.resource));
                }
                if (state.tunnel) {
                    state.tunnel->accepted = state.responseBodyPlan->contentSemantics() == HttpResponseContentSemantics::kConnectTunnel;
                    if (state.tunnel->accepted && state.tunnel->udp) {
                        std::pmr::vector<HttpHeaderView> fields(state.resource);
                        for (const auto& field : state.headers) {
                            fields.emplace_back(field.name(), field.value());
                        }
                        if (!validateHttpConnectUdpResponse(state.protocolVersion, state.status.value(), fields)) {
                            throw HttpClientError(HttpClientError::Code::kProtocolError, "invalid CONNECT-UDP response head");
                        }
                    }
                    if (!state.tunnel->accepted) {
                        state.tunnel->stop();
                    }
                    pending->signal.notify();
                }
                if (!state.tunnel || !state.tunnel->accepted) {
                    configureHttpClientResponseDecoding(state);
                }
                state.headReady = true;
                pending->response->state_->headSignal.notify();
            } catch (...) {
                failPending(*pending, head->streamId(), std::current_exception(), true);
            }
        } else if (event->messageBodyChunk() != nullptr || event->tunnelData() != nullptr) {
            const auto consume = [&](auto* chunk) {
                auto* pending = findPending(chunk->streamId());
                if (pending != nullptr && !pending->complete && !pending->failed()) {
                    auto& state = *pending->response->state_;
                    const auto retained =
                        state.buffered.size() - state.offset + state.pending.size();
                    if (state.collectAll && chunk->bytes().size() >
                                                config_.maxResponseBytes - std::min(retained, config_.maxResponseBytes)) {
                        pending->error = HttpClientError::Code::kResponseTooLarge;
                        submitHttp2Reset(connection, chunk->streamId());
                        pending->signal.notify();
                    } else {
                        try {
                            state.pending.append(chunk->bytes());
                            state.dataSignal.notify();
                        } catch (...) {
                            failPending(*pending, chunk->streamId(), std::current_exception(), true);
                        }
                    }
                }
                if (pending != nullptr && !pending->complete && !pending->failed() &&
                    !pending->response->state_->collectAll) {
                    auto credit = chunk->takeCredit();
                    if (credit.valid()) {
                        auto& retained = pending->response->state_->http2DataCredit;
                        if (retained) {
                            if (retained->merge(std::move(credit)) !=
                                Http2ReceivedDataCreditMergeStatus::kMerged) {
                                std::terminate();
                            }
                        } else {
                            retained.emplace(std::move(credit));
                        }
                    }
                }
                // Unretained event credits return on destruction, including failed
                // or cancelled streams. Wake the writer to flush WINDOW_UPDATE.
                releasedData = true;
            };
            if (auto* chunk = event->messageBodyChunk()) {
                consume(chunk);
            } else {
                consume(event->tunnelData());
            }
        } else if (const auto* tunnelEnd = event->tunnelEnd()) {
            if (auto* pending = findPending(tunnelEnd->streamId()); pending != nullptr && !pending->failed()) {
                auto& state = *pending->response->state_;
                if (!state.tunnel || !state.tunnel->accepted) {
                    pending->error = HttpClientError::Code::kProtocolError;
                } else {
                    state.tunnel->receiveEnded = true;
                    state.dataSignal.notify();
                }
                pending->signal.notify();
            }
            runtime.stateSignal.notify();
        } else if (auto* end = event->messageEnd()) {
            if (auto* pending = findPending(end->streamId());
                pending != nullptr && !pending->failed()) {
                try {
                    const bool contentSemanticsPresent =
                        end->contentSemantics() == Http2MessageContentSemantics::kContent;
                    auto responseTrailers = std::move(*end).takeTrailers();
                    auto& state = *pending->response->state_;
                    state.trailers.clear();
                    state.trailers.reserve(responseTrailers.size());
                    for (const auto& trailer : responseTrailers) {
                        state.trailers.push_back(HttpHeader::copyOf(
                            trailer.name(), trailer.value(), state.resource));
                    }
                    if (pending->timeout == nullptr) {
                        std::terminate();
                    }
                    if (pending->timeout->expired()) {
                        throw HttpClientError(HttpClientError::Code::kTimeout,
                            "HTTP/2 response body decoding timed out");
                    }
                    decodeHttpClientResponseContentEncoding(
                        *pending->response->state_, contentSemanticsPresent,
                        config_.maxResponseBytes, pending->response->state_->resource);
                    if (pending->timeout->expired()) {
                        throw HttpClientError(HttpClientError::Code::kTimeout,
                            "HTTP/2 response body decoding timed out");
                    }
                    pending->complete = true;
                } catch (...) {
                    failPending(*pending, end->streamId(), std::current_exception(), false);
                }
                if (pending->complete) {
                    pending->signal.notify();
                }
            }
            runtime.stateSignal.notify();
        } else if (const auto* closed = event->streamClosed()) {
            if (auto* pending = findPending(closed->streamId());
                pending != nullptr && !pending->complete && !pending->failed() &&
                !pending->retryable) {
                const auto& state = *pending->response->state_;
                if (!state.tunnel || !state.tunnel->accepted || !state.tunnel->receiveEnded || !state.tunnel->endRequested) {
                    pending->error = HttpClientError::Code::kProtocolError;
                    pending->signal.notify();
                }
            }
            runtime.stateSignal.notify();
        } else if (const auto* unprocessed = event->requestUnprocessed()) {
            if (auto* pending = findPending(unprocessed->streamId());
                pending != nullptr && !pending->complete && !pending->failed()) {
                pending->retryable = true;
                pending->signal.notify();
            }
            runtime.draining = true;
            runtime.stateSignal.notify();
        } else if (event->goaway() != nullptr) {
            runtime.draining = true;
            runtime.stateSignal.notify();
        }
    }
    for (const auto streamId : connection.http2->takeDrainedDataStreams()) {
        if (auto* pending = findPending(streamId); pending != nullptr) {
            pending->signal.notify();
        }
    }
    if (releasedData || connection.http2->wantsWrite()) {
        runtime.writeSignal.notify();
    }
}

void HttpClientPool::failHttp2Session(Connection& connection, std::uint64_t generation,
    std::error_code transportError, const std::exception_ptr& failure) noexcept {
    auto& runtime = *connection.http2Runtime;
    if (runtime.generation != generation || runtime.failed) {
        return;
    }
    runtime.failed = true;
    runtime.draining = true;
    connection.connected = false;
    connection.deadlineTimer->cancel();
    connection.deadline.reset();
    std::error_code ignored;
    connection.resolver.cancel();
    (void)connection.stream.lowest_layer().cancel(ignored);
    (void)connection.stream.lowest_layer().close(ignored);
    auto pendingError = HttpClientError::Code::kIoError;
    switch (connection.abortReason) {
        case AbortReason::kTimeout:
            pendingError = HttpClientError::Code::kTimeout;
            break;
        case AbortReason::kCancelled:
            pendingError = HttpClientError::Code::kCancelled;
            break;
        case AbortReason::kClosing:
            pendingError = HttpClientError::Code::kClosing;
            break;
        case AbortReason::kNone:
            if (transportError == std::errc::timed_out) {
                pendingError = HttpClientError::Code::kTimeout;
            } else if (transportError == std::errc::protocol_error) {
                pendingError = HttpClientError::Code::kProtocolError;
            } else {
                pendingError = transportErrorCode(transportError);
            }
            break;
    }
    for (auto* pending : runtime.pending) {
        if (!pending->complete && !pending->failed() && !pending->retryable) {
            if (failure != nullptr) {
                pending->failure = failure;
            } else {
                pending->error = pendingError;
            }
            pending->signal.notify();
        }
    }
    runtime.writeSignal.notify();
    runtime.stateSignal.notify();
}

void HttpClientPool::finishHttp2SessionTask(
    Connection& connection, std::uint64_t generation) noexcept {
    auto& runtime = *connection.http2Runtime;
    if (runtime.generation != generation || runtime.sessionTasks == 0) {
        std::terminate();
    }
    --runtime.sessionTasks;
    if (runtime.sessionTasks == 0) {
        runtime.running = false;
    }
    runtime.stateSignal.notify();
}

Task<void> HttpClientPool::runHttp2Reader(Connection& connection, std::uint64_t generation) {
    struct Finish final {
        HttpClientPool& pool;
        Connection& connection;
        std::uint64_t generation;
        ~Finish() {
            pool.finishHttp2SessionTask(connection, generation);
        }
    } finish{*this, connection, generation};
    std::array<char, 16384> input{};
    try {
        auto& runtime = *connection.http2Runtime;
        while (runtime.generation == generation && !runtime.failed) {
            AsioCompletion<std::size_t> completion =
                config_.scheme == HttpScheme::kHttps
                    ? co_await ruvia::asyncAsio<std::size_t>([&connection, &input](auto handler) mutable {
                          connection.stream.async_read_some(
                              asio::buffer(input), std::move(handler));
                      })
                    : co_await ruvia::asyncAsio<std::size_t>([&connection, &input](auto handler) mutable {
                          connection.stream.next_layer().async_read_some(
                              asio::buffer(input), std::move(handler));
                      });
            if (completion.errorCode() || completion.result() == 0) {
                failHttp2Session(connection, generation,
                    completion.errorCode() ? completion.errorCode()
                                           : std::make_error_code(std::errc::connection_reset));
                co_return;
            }
            bytesReceived_ += completion.result();
            const auto bytes = std::string_view(input.data(), completion.result());
            for (;;) {
                const auto status = connection.http2->feed(bytes);
                drainHttp2Events(connection);
                if (status == Http2FeedResult::kProtocolFailure ||
                    connection.http2->connectionError()) {
                    failHttp2Session(
                        connection, generation, std::make_error_code(std::errc::protocol_error));
                    co_return;
                }
                if (status != Http2FeedResult::kEventsPending) {
                    break;
                }
            }
        }
    } catch (...) {
        failHttp2Session(connection, generation, {}, std::current_exception());
    }
}

Task<void> HttpClientPool::runHttp2Writer(Connection& connection, std::uint64_t generation) {
    struct Finish final {
        HttpClientPool& pool;
        Connection& connection;
        std::uint64_t generation;
        ~Finish() {
            pool.finishHttp2SessionTask(connection, generation);
        }
    } finish{*this, connection, generation};
    std::pmr::string output(resource_);
    try {
        auto& runtime = *connection.http2Runtime;
        for (;;) {
            while (runtime.generation == generation && !runtime.failed && connection.http2 &&
                   connection.http2->wantsWrite()) {
                const auto pending = connection.http2->pendingOutput();
                output.assign(pending);
                (void)connection.http2->consumeOutput(pending.size());
                const ruvia::OperationTimeout writeTimeout(config_.writeTimeout);
                if (!armDeadline(connection, writeTimeout, DeadlineKind::kSocket)) {
                    failHttp2Session(
                        connection, generation, std::make_error_code(std::errc::timed_out));
                    co_return;
                }
                AsioCompletion<std::size_t> completion =
                    config_.scheme == HttpScheme::kHttps
                        ? co_await ruvia::asyncAsio<std::size_t>(
                              [&connection, &output](auto handler) mutable {
                                  asio::async_write(
                                      connection.stream, asio::buffer(output), std::move(handler));
                              })
                        : co_await ruvia::asyncAsio<std::size_t>(
                              [&connection, &output](auto handler) mutable {
                                  asio::async_write(connection.stream.next_layer(),
                                      asio::buffer(output), std::move(handler));
                              });
                const bool timedOut = clearDeadline(connection) || writeTimeout.expired();
                if (timedOut) {
                    failHttp2Session(
                        connection, generation, std::make_error_code(std::errc::timed_out));
                    co_return;
                }
                if (completion.errorCode()) {
                    failHttp2Session(connection, generation, completion.errorCode());
                    co_return;
                }
                bytesSent_ += completion.result();
            }
            if (runtime.generation != generation || runtime.failed) {
                co_return;
            }
            co_await runtime.writeSignal.wait();
        }
    } catch (...) {
        failHttp2Session(connection, generation, {}, std::current_exception());
    }
}

void HttpClientPool::submitHttp2Reset(Connection& connection, std::uint32_t streamId) noexcept {
    if (streamId == 0 || !connection.http2) {
        return;
    }
    auto& runtime = *connection.http2Runtime;
    try {
        (void)connection.http2->submitReset(streamId, Http2ErrorCode::kCancel);
        runtime.writeSignal.notify();
    } catch (...) {
        failHttp2Session(connection, runtime.generation, {}, std::current_exception());
    }
}

void HttpClientPool::cancelHttp2Stream(
    Connection& connection, std::uint64_t requestId, AbortReason reason) noexcept {
    auto& runtime = *connection.http2Runtime;
    const auto match = std::ranges::find_if(runtime.pending,
        [requestId](const Http2PendingStream* pending) { return pending->requestId == requestId; });
    if (match == runtime.pending.end()) {
        return;
    }
    auto& pending = **match;
    if (pending.complete || pending.failed() || pending.retryable) {
        return;
    }
    pending.error = reason == AbortReason::kTimeout     ? HttpClientError::Code::kTimeout
                    : reason == AbortReason::kCancelled ? HttpClientError::Code::kCancelled
                                                        : HttpClientError::Code::kClosing;
    submitHttp2Reset(connection, pending.streamId);
    pending.signal.notify();
    runtime.stateSignal.notify();
}

void HttpClientPool::Http2PendingRegistration::reset() noexcept {
    if (!active_) {
        return;
    }
    active_ = false;
    pool_.removeHttp2Pending(connection_, pending_);
}

void HttpClientPool::removeHttp2Pending(
    Connection& connection, Http2PendingStream& pending) noexcept {
    auto& runtime = *connection.http2Runtime;
    if (!pending.complete && !pending.retryable) {
        submitHttp2Reset(connection, pending.streamId);
    }
    releaseResponseData(*pending.response->state_);
    const auto match = std::ranges::find(runtime.pending, &pending);
    if (match == runtime.pending.end()) {
        std::terminate();
    }
    runtime.pending.erase(match);
    runtime.stateSignal.notify();
    if (runtime.draining && runtime.pending.empty()) {
        std::error_code ignored;
        (void)connection.stream.lowest_layer().cancel(ignored);
        (void)connection.stream.lowest_layer().close(ignored);
        connection.connected = false;
        runtime.writeSignal.notify();
    }
}

Task<void> HttpClientPool::waitForHttp2SessionStop(
    Connection& connection, const ruvia::OperationTimeout& timeout, StopToken stopToken) {
    auto& runtime = *connection.http2Runtime;
    WorkerTimerRegistration deadlineTimer;
    if (const auto remaining = timeout.remaining()) {
        if (remaining->count() == 0) {
            throw HttpClientError(
                HttpClientError::Code::kTimeout, "HTTP/2 session shutdown wait timed out");
        }
        WorkerHandleAccess::scheduleTimer(worker_, deadlineTimer,
            workerTimerDeadlineAfter(*remaining), [&runtime](WorkerTimerOutcome outcome) noexcept {
                if (outcome == WorkerTimerOutcome::kExpired) {
                    runtime.stateSignal.notify();
                }
            });
    }
    std::uint64_t cancellationId = 0;
    StopRegistration stopRegistration;
    if (stopToken.stoppable()) {
        if (runtime.stateCancellationWaiters++ == 0) {
            runtime.stateCancellationId = cancellationMailbox_->nextOperationId();
        }
        cancellationId = runtime.stateCancellationId;
        stopToken.registerCallback(
            stopRegistration, WorkerCancellationPost<HttpClientOperationCancellationMailbox>(
                                  cancellationMailbox_, cancellationId));
    }
    struct CancellationRegistrationGuard final {
        Http2Runtime& runtime;
        std::uint64_t cancellationId;
        StopRegistration& registration;

        ~CancellationRegistrationGuard() {
            if (cancellationId != 0) {
                if (runtime.stateCancellationWaiters == 0 ||
                    runtime.stateCancellationId != cancellationId) {
                    std::terminate();
                }
                if (--runtime.stateCancellationWaiters == 0) {
                    runtime.stateCancellationId = 0;
                }
            }
            registration.reset();
        }
    } cancellationRegistrationGuard{runtime, cancellationId, stopRegistration};
    while (runtime.sessionTasks != 0 || !runtime.pending.empty()) {
        if (stopToken.stopRequested()) {
            throw HttpClientError(
                HttpClientError::Code::kCancelled, "HTTP/2 session shutdown wait cancelled");
        }
        if (timeout.expired()) {
            throw HttpClientError(
                HttpClientError::Code::kTimeout, "HTTP/2 session shutdown wait timed out");
        }
        co_await runtime.stateSignal.wait();
    }
}

Task<void> HttpClientPool::executeHttp2(Connection& connection,
    const HttpClientRequestStorage& request, const ruvia::OperationTimeout& timeout, StopToken stopToken,
    HttpClientResponse& response) {
    std::pmr::vector<HttpHeaderView> headers(resource_);
    auto source = HttpClientRequestStorageAccess::view(request, headers);
    std::pmr::string cookieHeader(resource_);
    appendAutomaticHeaders(request, headers, cookieHeader);
    source.headers = std::span<const HttpHeaderView>(headers);
    auto authority = http2Authority(config_, resource_);
    const auto* body = source.content.borrowedBytes();
    const auto content = request.upload() != nullptr ? ::ruvia::Http2RequestContent::streaming(request.upload()->config.contentLength) : body ? ::ruvia::Http2RequestContent::knownLength(body->value().size())
                                                                                                                                              : ::ruvia::Http2RequestContent::none();

    for (int attempt = 0; attempt < 2; ++attempt) {
        auto& runtime = *connection.http2Runtime;
        if (!connection.http2 || runtime.failed || runtime.draining) {
            if (runtime.draining && runtime.pending.empty()) {
                std::error_code ignored;
                (void)connection.stream.lowest_layer().cancel(ignored);
                (void)connection.stream.lowest_layer().close(ignored);
                connection.connected = false;
                runtime.writeSignal.notify();
            }
            co_await waitForHttp2SessionStop(connection, timeout, stopToken);
            co_await ensureConnected(connection, timeout, timeout, stopToken);
            if (connection.protocol != WireProtocol::kHttp2) {
                throw HttpClientError(HttpClientError::Code::kProtocolUnavailable,
                    "upstream no longer negotiated HTTP/2");
            }
        }

        Http2PendingStream pending(worker_, response);
        pending.timeout = &timeout;
        pending.requestId = ++runtime.nextRequestId;
        if (pending.requestId == 0) {
            pending.requestId = ++runtime.nextRequestId;
        }
        response.state_->transport = HttpClientResponseTransport::kHttp2;
        response.state_->requestMethod = classifyHttpMethod(request.method());
        response.state_->connectionIndex =
            static_cast<std::size_t>(&connection - connections_.data());
        response.state_->requestId = pending.requestId;
        runtime.pending.push_back(&pending);
        Http2PendingRegistration pendingRegistration(*this, connection, pending);
        WorkerTimerRegistration deadlineTimer;
        if (const auto remaining = timeout.remaining()) {
            WorkerHandleAccess::scheduleTimer(worker_, deadlineTimer,
                workerTimerDeadlineAfter(*remaining),
                [this, &connection, requestId = pending.requestId](
                    WorkerTimerOutcome outcome) noexcept {
                    if (outcome == WorkerTimerOutcome::kExpired) {
                        cancelHttp2Stream(connection, requestId, AbortReason::kTimeout);
                    }
                });
        }
        std::uint64_t cancellationId = 0;
        StopRegistration stopRegistration;
        if (stopToken.stoppable()) {
            cancellationId = cancellationMailbox_->nextOperationId();
            pending.cancellationId = cancellationId;
            response.state_->cancellationId = cancellationId;
            stopToken.registerCallback(
                stopRegistration, WorkerCancellationPost<HttpClientOperationCancellationMailbox>(
                                      cancellationMailbox_, cancellationId));
        }
        struct StreamCancellationRegistrationGuard final {
            Http2PendingStream& pending;
            std::uint64_t cancellationId;
            StopRegistration& registration;

            ~StreamCancellationRegistrationGuard() {
                if (pending.cancellationId == cancellationId) {
                    pending.cancellationId = 0;
                }
                registration.reset();
            }
        } cancellationRegistrationGuard{pending, cancellationId, stopRegistration};
        if (stopToken.stopRequested()) {
            cancelOperationById(cancellationId);
        }

        for (;;) {
            if (pending.failed()) {
                break;
            }
            if (timeout.expired()) {
                pending.error = HttpClientError::Code::kTimeout;
                break;
            }
            const auto submitted = request.isTunnel()
                                       ? (request.tunnelProtocol().empty()
                                                 ? connection.http2->submitRequestHead(Http2ConnectRequestHeadView{.authority = BorrowedText(request.tunnelAuthority()), .headers = headers})
                                                 : connection.http2->submitRequestHead(Http2ExtendedConnectRequestHeadView{.protocol = BorrowedText(request.tunnelProtocol()), .scheme = config_.scheme == HttpScheme::kHttps ? "https" : "http", .authority = BorrowedText(request.tunnelAuthority()), .target = source.target, .headers = headers}))
                                       : connection.http2->submitRequestHead(::ruvia::Http2RegularRequestHeadView{
                                             .method = source.method,
                                             .scheme = config_.scheme == HttpScheme::kHttps ? "https" : "http",
                                             .authority = BorrowedText(std::string_view(authority)),
                                             .target = source.target,
                                             .headers = headers,
                                             .content = content,
                                             .expectation = request.upload() != nullptr ? request.upload()->config.expectation : HttpClientRequestExpectation::kNone});
            if (const auto* accepted = submitted.submitted()) {
                pending.streamId = accepted->streamId();
                response.state_->streamId = pending.streamId;
                if (body && !body->value().empty()) {
                    const auto status = connection.http2->submitData(
                        pending.streamId, body->value(), Http2EndStream::kEndStream);
                    if (status != Http2DataSubmitStatus::kAccepted &&
                        status != Http2DataSubmitStatus::kQueued) {
                        pending.error = HttpClientError::Code::kProtocolError;
                    }
                }
                runtime.writeSignal.notify();
                break;
            }
            const auto error = submitted.failure()->error();
            if (error == Http2RequestHeadSubmitError::kPeerStreamLimitReached ||
                error == Http2RequestHeadSubmitError::kLocalStreamCapacityReached) {
                co_await runtime.stateSignal.wait();
                if (pending.failed()) {
                    break;
                }
                continue;
            }
            if (error == Http2RequestHeadSubmitError::kConnectionUnavailable) {
                pending.retryable = true;
            } else {
                pending.error = HttpClientError::Code::kInvalidRequest;
            }
            break;
        }

        if (auto* upload = request.output(); upload != nullptr && pending.streamId != 0 && !pending.failed() && !pending.retryable) {
            upload->wakeTarget = &pending.signal;
            upload->wake = [](void* target) noexcept { static_cast<WorkerSignal*>(target)->notify(); };
            struct UploadWakeGuard {
                HttpClientOutputQueue& upload;
                ~UploadWakeGuard() {
                    upload.wake = nullptr;
                    upload.wakeTarget = nullptr;
                }
            } wakeGuard{*upload};
            WorkerTimerRegistration continueTimer;
            if (request.upload() != nullptr && !request.upload()->contentReleased) {
                WorkerHandleAccess::scheduleTimer(worker_, continueTimer, workerTimerDeadlineAfter(request.upload()->config.continueTimeout), [upload, policy = request.upload()](WorkerTimerOutcome outcome) noexcept {
                    if (outcome == WorkerTimerOutcome::kExpired && !upload->stopped) {
                        policy->contentReleased = true;
                        upload->notifyData();
                    }
                });
            }
            while (!upload->ended && !upload->stopped && !pending.complete && !pending.failed() && !pending.retryable) {
                if (request.isTunnel() ? !request.tunnel()->accepted : !request.upload()->contentReleased) {
                    co_await pending.signal.wait();
                    continue;
                }
                continueTimer.cancel();
                if (!request.isTunnel()) {
                    const auto released = connection.http2->releaseRequestContent(pending.streamId);
                    if (released == Http2RequestContentReleaseStatus::kClosed) {
                        break;
                    }
                }
                if (upload->chunkReady) {
                    const auto submitted = connection.http2->submitData(pending.streamId, upload->chunk, Http2EndStream::kKeepOpen);
                    if (submitted == Http2DataSubmitStatus::kBackpressured || submitted == Http2DataSubmitStatus::kExpectationPending) {
                        co_await pending.signal.wait();
                        continue;
                    }
                    if (submitted != Http2DataSubmitStatus::kAccepted && submitted != Http2DataSubmitStatus::kQueued) {
                        pending.error = HttpClientError::Code::kInvalidRequest;
                        submitHttp2Reset(connection, pending.streamId);
                        break;
                    }
                    runtime.writeSignal.notify();
                    while (connection.http2->hasQueuedData(pending.streamId) && !pending.failed() && !pending.retryable && !upload->stopped) {
                        co_await pending.signal.wait();
                    }
                    if (!pending.failed() && !pending.retryable && !upload->stopped) {
                        upload->acknowledgeChunk();
                    }
                } else if (upload->endRequested) {
                    std::pmr::vector<HttpHeaderView> trailers(response.state_->resource);
                    bool finishAccepted{};
                    if (request.isTunnel()) {
                        const auto finished = connection.http2->submitData(pending.streamId, {}, Http2EndStream::kEndStream);
                        finishAccepted = finished == Http2DataSubmitStatus::kAccepted || finished == Http2DataSubmitStatus::kQueued;
                    } else {
                        for (const auto& field : request.upload()->trailers) {
                            trailers.emplace_back(field.name(), field.value());
                        }
                        const auto finished = connection.http2->finishRequest(pending.streamId, trailers);
                        finishAccepted = finished == Http2FinishRequestStatus::kAccepted || finished == Http2FinishRequestStatus::kQueued;
                    }
                    if (!finishAccepted) {
                        pending.error = HttpClientError::Code::kInvalidRequest;
                        submitHttp2Reset(connection, pending.streamId);
                        break;
                    }
                    runtime.writeSignal.notify();
                    while (connection.http2->hasQueuedData(pending.streamId) && !pending.failed() && !pending.retryable && !upload->stopped) {
                        co_await pending.signal.wait();
                    }
                    if (!pending.failed() && !pending.retryable && !upload->stopped) {
                        upload->finish();
                    }
                } else {
                    co_await pending.signal.wait();
                }
            }
        }
        while (!pending.complete && !pending.failed() && !pending.retryable) {
            if (request.tunnel() != nullptr && request.tunnel()->accepted && request.tunnel()->receiveEnded && request.tunnel()->ended) {
                pending.complete = true;
                break;
            }
            co_await pending.signal.wait();
        }
        deadlineTimer.cancel();
        const bool retryable = pending.retryable;
        const auto error = pending.error;
        const auto failure = pending.failure;
        pendingRegistration.reset();
        if (retryable && request.output() == nullptr && attempt == 0 && !timeout.expired()) {
            continue;
        }
        if (retryable) {
            throw HttpClientError(HttpClientError::Code::kProtocolError,
                "HTTP/2 request was not processed after GOAWAY");
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        if (error) {
            switch (*error) {
                case HttpClientError::Code::kTimeout:
                    throw HttpClientError(*error, "HTTP/2 request timed out");
                case HttpClientError::Code::kCancelled:
                    throw HttpClientError(*error, "HTTP/2 request cancelled");
                case HttpClientError::Code::kResponseTooLarge:
                    throw HttpClientError(*error, "HTTP/2 response exceeds configured byte limit");
                case HttpClientError::Code::kClosing:
                    throw HttpClientError(*error, "HTTP client pool is closing");
                case HttpClientError::Code::kTlsFailed:
                    throw HttpClientError(*error, "HTTP/2 TLS connection failed");
                case HttpClientError::Code::kIoError:
                    throw HttpClientError(*error, "HTTP/2 connection failed");
                case HttpClientError::Code::kProtocolError:
                    throw HttpClientError(*error, "HTTP/2 protocol failed");
                default:
                    throw HttpClientError(*error, "HTTP/2 stream failed");
            }
        }
        co_return;
    }
    throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP/2 request retry exhausted");
}

}  // namespace ruvia::detail
