#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <array>
#include <algorithm>

#include <asio/write.hpp>
#include <asio/ssl/error.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/core/detail/worker/WorkerCancellationPost.h"
#include "client/HttpClientResponseState.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::pmr::string http2Authority(const HttpClientConfigStorage& config, std::pmr::memory_resource* resource) {
    auto authority = httpClientWireHost(config, resource);
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

Task<void> HttpClientPool::initializeHttp2(Connection& connection, const OperationTimeout& timeout) {
    connection.http2 = makePmrObject<Http2Connection>(resource_, resource_, Http2Role::kClient);
    connection.http2->beginConnection();
    while (connection.http2->wantsWrite()) {
        const auto output = connection.http2->pendingOutput();
        co_await write(connection, output, timeout);
        (void)connection.http2->consumeOutput(output.size());
    }
    std::array<char, 16384> input{};
    while (!connection.http2->receivedPeerSettings()) {
        const auto bytes = co_await readSome(connection, input, timeout);
        if (bytes == 0) {
            throw HttpClientError(HttpClientError::Code::kIoError, "upstream closed during HTTP/2 preface");
        }
        const auto status = connection.http2->feed(std::string_view(input.data(), bytes));
        while (connection.http2->nextEvent()) {
        }
        if (status == Http2FeedResult::kProtocolFailure || connection.http2->connectionError()) {
            throw HttpClientError(HttpClientError::Code::kProtocolError, "invalid HTTP/2 connection preface");
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

void HttpClientPool::drainHttp2Events(Connection& connection) {
    if (!connection.http2) {
        return;
    }
    auto& runtime = *connection.http2Runtime;
    const auto findPending = [&runtime](std::uint32_t streamId) -> Http2PendingStream* {
        const auto match = std::ranges::find_if(runtime.pending, [streamId](const Http2PendingStream* pending) { return pending->streamId == streamId; });
        return match == runtime.pending.end() ? nullptr : *match;
    };
    const auto failPending = [this, &connection](Http2PendingStream& pending, std::uint32_t streamId, std::exception_ptr failure, bool resetStream) noexcept {
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
        if (const auto* head = event->messageHead()) {
            auto* pending = findPending(head->streamId());
            auto* stream = connection.http2->stream(head->streamId());
            if (pending == nullptr || stream == nullptr || pending->complete || pending->failed()) {
                continue;
            }
            if (!stream->responseStatus()) {
                continue;  // Validated informational response.
            }
            const auto responseHeaderCount = stream->remoteInitialHeaderCount().value_or(stream->remoteHeaderCount());
            try {
                pending->response->state_->status = *stream->responseStatus();
                pending->response->state_->protocolVersion = HttpProtocolVersion::kHttp2;
                pending->response->state_->headers.clear();
                pending->response->state_->headers.reserve(responseHeaderCount);
                for (std::size_t i = 0; i < responseHeaderCount; ++i) {
                    const auto header = stream->remoteHeaderAt(i);
                    pending->response->state_->headers.push_back(HttpClientResponseHeaderAccess::make(header.name, header.value, pending->response->state_->resource));
                }
                pending->responseHeaderCount = responseHeaderCount;
                pending->response->state_->headReady = true;
                pending->response->state_->headSignal.notify();
            } catch (...) {
                failPending(*pending, head->streamId(), std::current_exception(), true);
            }
        } else if (const auto* chunk = event->messageBodyChunk()) {
            auto* pending = findPending(chunk->streamId());
            if (pending != nullptr && !pending->complete && !pending->failed()) {
                const auto retained = pending->response->state_->collectAll ? pending->response->state_->buffered.size() - pending->response->state_->offset + pending->response->state_->pending.size() : pending->response->state_->pending.size();
                if (chunk->bytes().size() > config_.maxResponseBytes - std::min(retained, config_.maxResponseBytes)) {
                    pending->error = HttpClientError::Code::kResponseTooLarge;
                    submitHttp2Reset(connection, chunk->streamId());
                    pending->signal.notify();
                } else {
                    try {
                        pending->response->state_->pending.append(chunk->bytes());
                        pending->response->state_->dataSignal.notify();
                    } catch (...) {
                        failPending(*pending, chunk->streamId(), std::current_exception(), true);
                    }
                }
            }
            if (pending != nullptr && pending->response->state_->collectAll) {
                connection.http2->releaseAllReceivedData(chunk->streamId());
                releasedData = true;
            } else if (pending != nullptr) {
                pending->response->state_->http2DataPending = true;
            }
        } else if (const auto* end = event->messageEnd()) {
            if (auto* pending = findPending(end->streamId()); pending != nullptr && !pending->failed()) {
                try {
                    bool contentSemanticsPresent = true;
                    if (auto* stream = connection.http2->stream(end->streamId())) {
                        contentSemanticsPresent = stream->remoteContent().metadataOnlyWithoutLength() == nullptr && stream->remoteContent().metadataOnlyKnownLength() == nullptr;
                        pending->response->state_->trailers.clear();
                        pending->response->state_->trailers.reserve(stream->remoteHeaderCount() - std::min(stream->remoteHeaderCount(), pending->responseHeaderCount));
                        for (std::size_t i = pending->responseHeaderCount; i < stream->remoteHeaderCount(); ++i) {
                            const auto trailer = stream->remoteHeaderAt(i);
                            pending->response->state_->trailers.push_back(HttpClientResponseHeaderAccess::make(trailer.name, trailer.value, pending->response->state_->resource));
                        }
                    }
                    decodeResponseContentEncoding(*pending->response, contentSemanticsPresent, config_.maxResponseBytes, pending->response->state_->resource);
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
            if (auto* pending = findPending(closed->streamId()); pending != nullptr && !pending->complete && !pending->failed() && !pending->retryable) {
                pending->error = HttpClientError::Code::kProtocolError;
                pending->signal.notify();
            }
            runtime.stateSignal.notify();
        } else if (const auto* unprocessed = event->requestUnprocessed()) {
            if (auto* pending = findPending(unprocessed->streamId()); pending != nullptr && !pending->complete && !pending->failed()) {
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
    if (releasedData || connection.http2->wantsWrite()) {
        runtime.writeSignal.notify();
    }
}

void HttpClientPool::failHttp2Session(Connection& connection, std::uint64_t generation, std::error_code transportError, const std::exception_ptr& failure) noexcept {
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
            } else if (config_.scheme == HttpScheme::kHttps && (transportError.category() == asio::error::get_ssl_category() || transportError == asio::ssl::error::stream_truncated)) {
                pendingError = HttpClientError::Code::kTlsFailed;
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

void HttpClientPool::finishHttp2SessionTask(Connection& connection, std::uint64_t generation) noexcept {
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
            AsioCompletion<std::size_t> completion = config_.scheme == HttpScheme::kHttps ? co_await asyncAsio<std::size_t>([&connection, &input](auto handler) mutable { connection.stream.async_read_some(asio::buffer(input), std::move(handler)); }) : co_await asyncAsio<std::size_t>([&connection, &input](auto handler) mutable { connection.stream.next_layer().async_read_some(asio::buffer(input), std::move(handler)); });
            if (completion.errorCode() || completion.result() == 0) {
                failHttp2Session(connection, generation, completion.errorCode() ? completion.errorCode() : std::make_error_code(std::errc::connection_reset));
                co_return;
            }
            bytesReceived_ += completion.result();
            const auto bytes = std::string_view(input.data(), completion.result());
            for (;;) {
                const auto status = connection.http2->feed(bytes);
                drainHttp2Events(connection);
                if (status == Http2FeedResult::kProtocolFailure || connection.http2->connectionError()) {
                    failHttp2Session(connection, generation, std::make_error_code(std::errc::protocol_error));
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
            while (runtime.generation == generation && !runtime.failed && connection.http2 && connection.http2->wantsWrite()) {
                const auto pending = connection.http2->pendingOutput();
                output.assign(pending);
                (void)connection.http2->consumeOutput(pending.size());
                const OperationTimeout writeTimeout(config_.writeTimeout);
                if (!armDeadline(connection, writeTimeout, DeadlineKind::kSocket)) {
                    failHttp2Session(connection, generation, std::make_error_code(std::errc::timed_out));
                    co_return;
                }
                AsioCompletion<std::size_t> completion = config_.scheme == HttpScheme::kHttps ? co_await asyncAsio<std::size_t>([&connection, &output](auto handler) mutable { asio::async_write(connection.stream, asio::buffer(output), std::move(handler)); }) : co_await asyncAsio<std::size_t>([&connection, &output](auto handler) mutable { asio::async_write(connection.stream.next_layer(), asio::buffer(output), std::move(handler)); });
                const bool timedOut = clearDeadline(connection) || writeTimeout.expired();
                if (timedOut) {
                    failHttp2Session(connection, generation, std::make_error_code(std::errc::timed_out));
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

void HttpClientPool::cancelHttp2Stream(Connection& connection, std::uint64_t requestId, AbortReason reason) noexcept {
    auto& runtime = *connection.http2Runtime;
    const auto match = std::ranges::find_if(runtime.pending, [requestId](const Http2PendingStream* pending) { return pending->requestId == requestId; });
    if (match == runtime.pending.end()) {
        return;
    }
    auto& pending = **match;
    if (pending.complete || pending.failed() || pending.retryable) {
        return;
    }
    pending.error = reason == AbortReason::kTimeout ? HttpClientError::Code::kTimeout : reason == AbortReason::kCancelled ? HttpClientError::Code::kCancelled : HttpClientError::Code::kClosing;
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

void HttpClientPool::removeHttp2Pending(Connection& connection, Http2PendingStream& pending) noexcept {
    auto& runtime = *connection.http2Runtime;
    if (pending.streamId != 0 && connection.http2) {
        try {
            connection.http2->unpinStream(pending.streamId);
        } catch (...) {
            failHttp2Session(connection, runtime.generation, {}, std::current_exception());
        }
    }
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

Task<void> HttpClientPool::waitForHttp2SessionStop(Connection& connection, const OperationTimeout& timeout, StopToken stopToken) {
    auto& runtime = *connection.http2Runtime;
    WorkerTimerRegistration deadlineTimer;
    if (const auto remaining = timeout.remaining()) {
        if (remaining->count() == 0) {
            throw HttpClientError(HttpClientError::Code::kTimeout, "HTTP/2 session shutdown wait timed out");
        }
        WorkerHandleAccess::scheduleTimer(worker_, deadlineTimer, workerTimerDeadlineAfter(*remaining), [&runtime](WorkerTimerOutcome outcome) noexcept {
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
        stopToken.registerCallback(stopRegistration, WorkerCancellationPost<HttpClientOperationCancellationMailbox>(cancellationMailbox_, cancellationId));
    }
    struct CancellationRegistrationGuard final {
        Http2Runtime& runtime;
        std::uint64_t cancellationId;
        StopRegistration& registration;

        ~CancellationRegistrationGuard() {
            if (cancellationId != 0) {
                if (runtime.stateCancellationWaiters == 0 || runtime.stateCancellationId != cancellationId) {
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
            throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP/2 session shutdown wait cancelled");
        }
        if (timeout.expired()) {
            throw HttpClientError(HttpClientError::Code::kTimeout, "HTTP/2 session shutdown wait timed out");
        }
        co_await runtime.stateSignal.wait();
    }
}

Task<void> HttpClientPool::executeHttp2(Connection& connection, const HttpClientRequestStorage& request, const OperationTimeout& timeout, StopToken stopToken, HttpClientResponse& response) {
    std::pmr::vector<HttpHeaderView> headers(resource_);
    auto source = HttpClientRequestStorageAccess::view(request, headers);
    std::pmr::string cookieHeader(resource_);
    appendAutomaticHeaders(request, headers, cookieHeader);
    source.headers = std::span<const HttpHeaderView>(headers);
    auto authority = http2Authority(config_, resource_);
    const auto* body = source.content.borrowedBytes();
    const auto content = body ? Http2RequestContent::knownLength(body->value().size()) : Http2RequestContent::none();

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
                throw HttpClientError(HttpClientError::Code::kProtocolUnavailable, "upstream no longer negotiated HTTP/2");
            }
        }

        Http2PendingStream pending(worker_, response);
        pending.requestId = ++runtime.nextRequestId;
        if (pending.requestId == 0) {
            pending.requestId = ++runtime.nextRequestId;
        }
        response.state_->http2 = true;
        response.state_->connectionIndex = static_cast<std::size_t>(&connection - connections_.data());
        response.state_->requestId = pending.requestId;
        runtime.pending.push_back(&pending);
        Http2PendingRegistration pendingRegistration(*this, connection, pending);
        WorkerTimerRegistration deadlineTimer;
        if (const auto remaining = timeout.remaining()) {
            WorkerHandleAccess::scheduleTimer(worker_, deadlineTimer, workerTimerDeadlineAfter(*remaining), [this, &connection, requestId = pending.requestId](WorkerTimerOutcome outcome) noexcept {
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
            stopToken.registerCallback(stopRegistration, WorkerCancellationPost<HttpClientOperationCancellationMailbox>(cancellationMailbox_, cancellationId));
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
            const auto submitted = connection.http2->submitRegularRequestHead(std::string_view(source.method), config_.scheme == HttpScheme::kHttps ? "https" : "http", authority, std::string_view(source.target), headers, content);
            if (const auto* accepted = submitted.submitted()) {
                pending.streamId = accepted->streamId();
                response.state_->streamId = pending.streamId;
                connection.http2->pinStream(pending.streamId);
                if (body && !body->value().empty()) {
                    const auto status = connection.http2->submitData(pending.streamId, body->value(), Http2EndStream::kEndStream);
                    if (status != Http2DataSubmitStatus::kAccepted && status != Http2DataSubmitStatus::kQueued) {
                        pending.error = HttpClientError::Code::kProtocolError;
                    }
                }
                runtime.writeSignal.notify();
                break;
            }
            const auto error = submitted.failure()->error();
            if (error == Http2RequestHeadSubmitError::kPeerStreamLimitReached || error == Http2RequestHeadSubmitError::kLocalStreamCapacityReached) {
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

        while (!pending.complete && !pending.failed() && !pending.retryable) {
            co_await pending.signal.wait();
        }
        deadlineTimer.cancel();
        const bool retryable = pending.retryable;
        const auto error = pending.error;
        const auto failure = pending.failure;
        if (pending.complete && response.state_->http2DataPending) {
            releaseResponseData(*response.state_);
        }
        pendingRegistration.reset();
        if (retryable && attempt == 0 && !timeout.expired()) {
            continue;
        }
        if (retryable) {
            throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP/2 request was not processed after GOAWAY");
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
