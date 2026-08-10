#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <array>
#include <algorithm>
#include <charconv>
#include <system_error>

#include <asio/write.hpp>
#include <asio/ssl/error.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::pmr::string http2Authority(const HttpClientConfigStorage& config, std::pmr::memory_resource* resource) {
    auto authority = httpClientWireHost(config, resource);
    const auto port = httpClientPort(config);
    const auto defaultPort = config.scheme == HttpScheme::kHttps ? 443 : 80;
    if (port != defaultPort) {
        authority.push_back(':');
        std::array<char, 8> bytes{};
        const auto [end, ec] = std::to_chars(bytes.data(), bytes.data() + bytes.size(), port);
        if (ec != std::errc{}) throw HttpClientError(HttpClientError::Code::kInvalidRequest, "invalid HTTP origin port");
        authority.append(bytes.data(), end);
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
        if (bytes == 0) throw HttpClientError(HttpClientError::Code::kIoError, "upstream closed during HTTP/2 preface");
        const auto status = connection.http2->feed(std::string_view(input.data(), bytes));
        while (connection.http2->nextEvent()) {}
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
    if (generation == 0) generation = ++runtime.generation;
    runtime.running = true;
    runtime.draining = false;
    runtime.failed = false;
    runtime.terminalError.clear();
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
        failHttp2Session(connection, generation, std::make_error_code(std::errc::not_enough_memory));
        throw;
    }
}

void HttpClientPool::drainHttp2Events(Connection& connection) {
    if (!connection.http2) return;
    auto& runtime = *connection.http2Runtime;
    const auto findPending = [&runtime](std::uint32_t streamId) -> Http2PendingStream* {
        const auto match = std::ranges::find_if(runtime.pending, [streamId](const Http2PendingStream* pending) {
            return pending->streamId == streamId;
        });
        return match == runtime.pending.end() ? nullptr : *match;
    };
    bool releasedData = false;
    while (auto event = connection.http2->nextEvent()) {
        if (const auto* head = event->messageHead()) {
            auto* pending = findPending(head->streamId());
            auto* stream = connection.http2->stream(head->streamId());
            if (pending == nullptr || stream == nullptr || pending->complete || pending->error) continue;
            if (!stream->responseStatus()) continue;  // Validated informational response.
            const auto responseHeaderCount = stream->responseHeaderCount().value_or(stream->requestHeaderCount());
            pending->response.status_ = *stream->responseStatus();
            pending->response.protocolVersion_ = HttpProtocolVersion::kHttp2;
            pending->response.headers_.clear();
            pending->response.headers_.reserve(responseHeaderCount);
            for (std::size_t i = 0; i < responseHeaderCount; ++i) {
                const auto header = stream->requestHeaderAt(i);
                pending->response.headers_.push_back(HttpClientHeaderAccess::make(
                    header.name, header.value, pending->response.headers_.get_allocator().resource()));
            }
            pending->responseHeaderCount = responseHeaderCount;
        } else if (const auto* chunk = event->messageBodyChunk()) {
            auto* pending = findPending(chunk->streamId());
            if (pending != nullptr && !pending->complete && !pending->error) {
                if (chunk->bytes().size() > config_.maxResponseBytes - std::min(pending->response.body_.size(), config_.maxResponseBytes)) {
                    pending->error = HttpClientError::Code::kResponseTooLarge;
                    submitHttp2Reset(connection, chunk->streamId());
                    pending->signal.notify();
                } else {
                    pending->response.body_.append(chunk->bytes());
                }
            }
            connection.http2->releaseReceivedData(chunk->streamId());
            releasedData = true;
        } else if (const auto* end = event->messageEnd()) {
            if (auto* pending = findPending(end->streamId()); pending != nullptr && !pending->error) {
                if (auto* stream = connection.http2->stream(end->streamId())) {
                    pending->response.trailers_.clear();
                    pending->response.trailers_.reserve(stream->requestHeaderCount() - std::min(stream->requestHeaderCount(), pending->responseHeaderCount));
                    for (std::size_t i = pending->responseHeaderCount; i < stream->requestHeaderCount(); ++i) {
                        const auto trailer = stream->requestHeaderAt(i);
                        pending->response.trailers_.push_back(HttpClientHeaderAccess::make(
                            trailer.name, trailer.value, pending->response.trailers_.get_allocator().resource()));
                    }
                }
                pending->complete = true;
                pending->signal.notify();
            }
            runtime.stateSignal.notify();
        } else if (const auto* closed = event->streamClosed()) {
            if (auto* pending = findPending(closed->streamId()); pending != nullptr && !pending->complete && !pending->error && !pending->retryable) {
                pending->error = HttpClientError::Code::kProtocolError;
                pending->signal.notify();
            }
            runtime.stateSignal.notify();
        } else if (const auto* unprocessed = event->requestUnprocessed()) {
            if (auto* pending = findPending(unprocessed->streamId()); pending != nullptr && !pending->complete && !pending->error) {
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
    if (releasedData || connection.http2->wantsWrite()) runtime.writeSignal.notify();
}

void HttpClientPool::failHttp2Session(Connection& connection, std::uint64_t generation, std::error_code error) noexcept {
    auto& runtime = *connection.http2Runtime;
    if (runtime.generation != generation || runtime.failed) return;
    runtime.failed = true;
    runtime.draining = true;
    runtime.terminalError = error;
    connection.connected = false;
    connection.deadlineTimer->cancel();
    connection.deadline.reset();
    std::error_code ignored;
    connection.resolver.cancel();
    connection.stream.lowest_layer().cancel(ignored);
    connection.stream.lowest_layer().close(ignored);
    auto pendingError = HttpClientError::Code::kIoError;
    switch (connection.abortReason) {
        case AbortReason::kTimeout: pendingError = HttpClientError::Code::kTimeout; break;
        case AbortReason::kCancelled: pendingError = HttpClientError::Code::kCancelled; break;
        case AbortReason::kClosing: pendingError = HttpClientError::Code::kClosing; break;
        case AbortReason::kNone:
            if (error == std::errc::timed_out) {
                pendingError = HttpClientError::Code::kTimeout;
            } else if (error == std::errc::protocol_error) {
                pendingError = HttpClientError::Code::kProtocolError;
            } else if (config_.scheme == HttpScheme::kHttps &&
                (error.category() == asio::error::get_ssl_category() ||
                    error == asio::ssl::error::stream_truncated)) {
                pendingError = HttpClientError::Code::kTlsFailed;
            }
            break;
    }
    for (auto* pending : runtime.pending) {
        if (!pending->complete && !pending->error && !pending->retryable) {
            pending->error = pendingError;
            pending->signal.notify();
        }
    }
    runtime.writeSignal.notify();
    runtime.stateSignal.notify();
}

void HttpClientPool::finishHttp2SessionTask(Connection& connection, std::uint64_t generation) noexcept {
    auto& runtime = *connection.http2Runtime;
    if (runtime.generation != generation || runtime.sessionTasks == 0) std::terminate();
    --runtime.sessionTasks;
    if (runtime.sessionTasks == 0) runtime.running = false;
    runtime.stateSignal.notify();
}

Task<void> HttpClientPool::runHttp2Reader(Connection& connection, std::uint64_t generation) {
    struct Finish final {
        HttpClientPool& pool;
        Connection& connection;
        std::uint64_t generation;
        ~Finish() { pool.finishHttp2SessionTask(connection, generation); }
    } finish{*this, connection, generation};
    std::array<char, 16384> input{};
    try {
        auto& runtime = *connection.http2Runtime;
        while (runtime.generation == generation && !runtime.failed) {
            AsioCompletion<std::size_t> completion = config_.scheme == HttpScheme::kHttps
                ? co_await asyncAsio<std::size_t>([&connection, &input](auto handler) mutable {
                      connection.stream.async_read_some(asio::buffer(input), std::move(handler));
                  })
                : co_await asyncAsio<std::size_t>([&connection, &input](auto handler) mutable {
                      connection.stream.next_layer().async_read_some(asio::buffer(input), std::move(handler));
                  });
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
                if (status != Http2FeedResult::kEventsPending) break;
            }
        }
    } catch (...) {
        failHttp2Session(connection, generation, std::make_error_code(std::errc::io_error));
    }
}

Task<void> HttpClientPool::runHttp2Writer(Connection& connection, std::uint64_t generation) {
    struct Finish final {
        HttpClientPool& pool;
        Connection& connection;
        std::uint64_t generation;
        ~Finish() { pool.finishHttp2SessionTask(connection, generation); }
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
                AsioCompletion<std::size_t> completion = config_.scheme == HttpScheme::kHttps
                    ? co_await asyncAsio<std::size_t>([&connection, &output](auto handler) mutable {
                          asio::async_write(connection.stream, asio::buffer(output), std::move(handler));
                      })
                    : co_await asyncAsio<std::size_t>([&connection, &output](auto handler) mutable {
                          asio::async_write(connection.stream.next_layer(), asio::buffer(output), std::move(handler));
                      });
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
            if (runtime.generation != generation || runtime.failed) co_return;
            co_await runtime.writeSignal.wait();
        }
    } catch (...) {
        failHttp2Session(connection, generation, std::make_error_code(std::errc::io_error));
    }
}

void HttpClientPool::submitHttp2Reset(Connection& connection, std::uint32_t streamId) noexcept {
    if (streamId == 0 || !connection.http2) return;
    auto& runtime = *connection.http2Runtime;
    try {
        (void)connection.http2->submitReset(streamId, Http2ErrorCode::kCancel);
        runtime.writeSignal.notify();
    } catch (...) {
        failHttp2Session(connection, runtime.generation, std::make_error_code(std::errc::io_error));
    }
}

void HttpClientPool::cancelHttp2Stream(Connection& connection, std::uint64_t requestId, AbortReason reason) noexcept {
    auto& runtime = *connection.http2Runtime;
    const auto match = std::ranges::find_if(runtime.pending, [requestId](const Http2PendingStream* pending) {
        return pending->requestId == requestId;
    });
    if (match == runtime.pending.end()) return;
    auto& pending = **match;
    if (pending.complete || pending.error || pending.retryable) return;
    pending.error = reason == AbortReason::kTimeout ? HttpClientError::Code::kTimeout
        : reason == AbortReason::kCancelled ? HttpClientError::Code::kCancelled
        : HttpClientError::Code::kClosing;
    submitHttp2Reset(connection, pending.streamId);
    pending.signal.notify();
    runtime.stateSignal.notify();
}

void HttpClientPool::Http2PendingRegistration::reset() noexcept {
    if (!active_) return;
    active_ = false;
    pool_.removeHttp2Pending(connection_, pending_);
}

void HttpClientPool::removeHttp2Pending(Connection& connection, Http2PendingStream& pending) noexcept {
    auto& runtime = *connection.http2Runtime;
    if (pending.streamId != 0 && connection.http2) {
        try {
            connection.http2->unpinStream(pending.streamId);
        } catch (...) {
            failHttp2Session(connection, runtime.generation, std::make_error_code(std::errc::io_error));
        }
    }
    const auto match = std::ranges::find(runtime.pending, &pending);
    if (match == runtime.pending.end()) std::terminate();
    runtime.pending.erase(match);
    runtime.stateSignal.notify();
    if (runtime.draining && runtime.pending.empty()) {
        std::error_code ignored;
        connection.stream.lowest_layer().cancel(ignored);
        connection.stream.lowest_layer().close(ignored);
        connection.connected = false;
        runtime.writeSignal.notify();
    }
}

Task<void> HttpClientPool::waitForHttp2SessionStop(
    Connection& connection,
    const OperationTimeout& timeout,
    StopToken stopToken) {
    auto& runtime = *connection.http2Runtime;
    WorkerTimerRegistration deadlineTimer;
    if (const auto remaining = timeout.remaining()) {
        if (remaining->count() == 0) {
            throw HttpClientError(HttpClientError::Code::kTimeout,
                "HTTP/2 session shutdown wait timed out");
        }
        WorkerHandleAccess::scheduleTimer(worker_, deadlineTimer, workerTimerDeadlineAfter(*remaining),
            [&runtime](WorkerTimerOutcome outcome) noexcept {
                if (outcome == WorkerTimerOutcome::kExpired) runtime.stateSignal.notify();
            });
    }
    auto stopRegistration = stopToken.registerCallback([this, &runtime] {
        WorkerHandleAccess::deferOrTerminate(worker_, [&runtime] { runtime.stateSignal.notify(); });
    });
    while (runtime.sessionTasks != 0 || !runtime.pending.empty()) {
        if (stopToken.stopRequested()) {
            throw HttpClientError(HttpClientError::Code::kCancelled,
                "HTTP/2 session shutdown wait cancelled");
        }
        if (timeout.expired()) {
            throw HttpClientError(HttpClientError::Code::kTimeout,
                "HTTP/2 session shutdown wait timed out");
        }
        co_await runtime.stateSignal.wait();
    }
}

Task<HttpClientResponse> HttpClientPool::executeHttp2(Connection& connection, std::size_t index, const HttpClientRequest& request, const OperationTimeout& timeout, StopToken stopToken, std::pmr::memory_resource* responseResource) {
    std::pmr::vector<HttpHeaderView> headers(resource_);
    auto source = HttpClientRequestAccess::view(request, headers);
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
                connection.stream.lowest_layer().cancel(ignored);
                connection.stream.lowest_layer().close(ignored);
                connection.connected = false;
                runtime.writeSignal.notify();
            }
            co_await waitForHttp2SessionStop(connection, timeout, stopToken);
            co_await ensureConnected(connection, index, timeout, timeout, stopToken);
            if (connection.protocol != WireProtocol::kHttp2) {
                throw HttpClientError(HttpClientError::Code::kProtocolUnavailable, "upstream no longer negotiated HTTP/2");
            }
        }

        Http2PendingStream pending(worker_, responseResource);
        pending.requestId = ++runtime.nextRequestId;
        if (pending.requestId == 0) pending.requestId = ++runtime.nextRequestId;
        runtime.pending.push_back(&pending);
        Http2PendingRegistration pendingRegistration(*this, connection, pending);
        WorkerTimerRegistration deadlineTimer;
        if (const auto remaining = timeout.remaining()) {
            WorkerHandleAccess::scheduleTimer(worker_, deadlineTimer, workerTimerDeadlineAfter(*remaining),
                [this, &connection, requestId = pending.requestId](WorkerTimerOutcome outcome) noexcept {
                    if (outcome == WorkerTimerOutcome::kExpired) cancelHttp2Stream(connection, requestId, AbortReason::kTimeout);
                });
        }
        auto stopRegistration = stopToken.registerCallback([this, &connection, requestId = pending.requestId] {
            WorkerHandleAccess::deferOrTerminate(worker_, [this, &connection, requestId] {
                cancelHttp2Stream(connection, requestId, AbortReason::kCancelled);
            });
        });
        if (stopToken.stopRequested()) cancelHttp2Stream(connection, pending.requestId, AbortReason::kCancelled);

        for (;;) {
            if (pending.error) break;
            if (timeout.expired()) {
                pending.error = HttpClientError::Code::kTimeout;
                break;
            }
            const auto submitted = connection.http2->submitRegularRequestHead(std::string_view(source.method),
                config_.scheme == HttpScheme::kHttps ? "https" : "http", authority, std::string_view(source.target), headers, content);
            if (const auto* accepted = submitted.submitted()) {
                pending.streamId = accepted->streamId();
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
                if (pending.error) break;
                continue;
            }
            if (error == Http2RequestHeadSubmitError::kConnectionUnavailable) {
                pending.retryable = true;
            } else {
                pending.error = HttpClientError::Code::kInvalidRequest;
            }
            break;
        }

        while (!pending.complete && !pending.error && !pending.retryable) co_await pending.signal.wait();
        deadlineTimer.cancel();
        stopRegistration.reset();
        const bool retryable = pending.retryable;
        const auto error = pending.error;
        auto response = std::move(pending.response);
        pendingRegistration.reset();
        if (retryable && attempt == 0 && !timeout.expired()) continue;
        if (retryable) throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP/2 request was not processed after GOAWAY");
        if (error) {
            switch (*error) {
                case HttpClientError::Code::kTimeout: throw HttpClientError(*error, "HTTP/2 request timed out");
                case HttpClientError::Code::kCancelled: throw HttpClientError(*error, "HTTP/2 request cancelled");
                case HttpClientError::Code::kResponseTooLarge: throw HttpClientError(*error, "HTTP/2 response exceeds configured byte limit");
                case HttpClientError::Code::kClosing: throw HttpClientError(*error, "HTTP client pool is closing");
                case HttpClientError::Code::kTlsFailed: throw HttpClientError(*error, "HTTP/2 TLS connection failed");
                case HttpClientError::Code::kIoError: throw HttpClientError(*error, "HTTP/2 connection failed");
                case HttpClientError::Code::kProtocolError: throw HttpClientError(*error, "HTTP/2 protocol failed");
                default: throw HttpClientError(*error, "HTTP/2 stream failed");
            }
        }
        co_return response;
    }
    throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP/2 request retry exhausted");
}

}  // namespace ruvia::detail
