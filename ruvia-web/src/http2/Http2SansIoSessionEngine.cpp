#include "http2/Http2SansIoSessionEngine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/recycling_allocator.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/http2/message/Http2RequestBuilder.h"
#include "ruvia/http/detail/http2/message/Http2WebSocketHandshake.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/web/detail/http/error/HttpProtocolErrorInfo.h"
#include "ruvia/web/detail/http2/Http2SansIoRequestBody.h"
#include "ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
#include "ruvia/web/detail/http2/Http2SansIoRouteSelection.h"
#include "ruvia/web/detail/http2/Http2SansIoWsTransport.h"
#include "ruvia/web/detail/ratelimit/RateLimitDecision.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServerAccessLog.h"
#include "ruvia/web/detail/server/request/RequestBodyLimit.h"
#include "ruvia/web/detail/server/request/RequestMemoryArena.h"
#include "ruvia/web/detail/server/response/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/response/HttpStaticFileCompression.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamDispatch.h"
#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"

namespace ruvia::detail {

Http2SansIoSessionEngine::Http2SansIoSessionEngine(
    asio::any_io_executor executor,
    asio::ip::tcp::socket& socket,
    const RouteTable& routes,
    WorkerMemory& worker,
    Http2SansIoSessionContext session)
    : executor_(std::move(executor)),
      socket_(&socket),
      routes_(&routes),
      worker_(&worker),
      session_(session),
      remoteAddress_(session_.services().connInfo().remote().address()),
      connection_(worker.resource(), Http2Role::kServer),
      writeSignal_(session_.services().worker()),
      handlerFinished_(session_.services().worker()),
      writerFinished_(session_.services().worker()),
      streamRuntimes_(worker.resource(), termination_),
      bufferedResponseWriter_(connection_, streamRuntimes_, worker, writeSignal_) {}

void Http2SansIoSessionEngine::beginConnection() {
    connection_.beginConnection();
}

bool Http2SansIoSessionEngine::wantsWrite() const noexcept {
    return connection_.wantsWrite();
}

void Http2SansIoSessionEngine::takeOutput(std::pmr::string& output) {
    connection_.takeOutput(output);
}

bool Http2SansIoSessionEngine::writeFailed() const noexcept {
    return lifecycle_.writeFailed();
}

bool Http2SansIoSessionEngine::writerShouldExit() const noexcept {
    return lifecycle_.stopping() && activeHandlerTasks_ == 0;
}

Task<void> Http2SansIoSessionEngine::waitForWrite() {
    co_await writeSignal_.wait();
}

void Http2SansIoSessionEngine::writerWriteFailed(std::error_code error) noexcept {
    lifecycle_.markWriteFailed();
    terminate(error);
}

void Http2SansIoSessionEngine::writerCompleted(std::exception_ptr exception) noexcept {
    if (exception != nullptr) {
        terminate(std::make_error_code(std::errc::operation_canceled));
    }
    writerTaskDone_ = true;
    lifecycle_.markWriterDone();
    writerFinished_.notify();
}

bool Http2SansIoSessionEngine::connectionFailed() const noexcept {
    return connection_.connectionError().has_value();
}

bool Http2SansIoSessionEngine::terminated() const noexcept {
    return termination_.terminated();
}

bool Http2SansIoSessionEngine::headerBlockInProgress() const noexcept {
    return connection_.headerBlockInProgress();
}

std::size_t Http2SansIoSessionEngine::activeRuntimeCount() const noexcept {
    return streamRuntimes_.size();
}

bool Http2SansIoSessionEngine::workerRunning() const noexcept {
    return session_.workerRunning();
}

void Http2SansIoSessionEngine::setInactivityPhase() noexcept {
    session_.scannerEntry().setPhase(http2SansIoInactivityPhase(
        connection_.headerBlockInProgress(), streamRuntimes_.size()));
}

void Http2SansIoSessionEngine::touchActivity() noexcept {
    session_.scannerEntry().touch();
}

void Http2SansIoSessionEngine::wakeWriter() noexcept {
    writeSignal_.notify();
}

void Http2SansIoSessionEngine::terminate(std::error_code error) noexcept {
    if (!termination_.terminate(error)) {
        return;
    }
    std::error_code ignored;
    socket_->cancel(ignored);
    streamRuntimes_.forEach([](Http2SansIoStreamRuntime& runtime) {
        if (auto* signal = runtime.signal()) {
            signal->wake();
        }
    });
    writeSignal_.notify();
}

std::pmr::memory_resource* Http2SansIoSessionEngine::workerResource() const noexcept {
    return worker_->resource();
}

Task<void> Http2SansIoSessionEngine::dispatchOneInner(std::uint32_t streamId) {
    const auto requestStart = std::chrono::steady_clock::now();
    const auto& options = session_.options();
    auto& scannerEntry = session_.scannerEntry();
    const auto& baseServices = session_.services();

    std::array<std::byte, kRequestArenaStackBytes> arenaBlock;
    std::optional<RequestMemory> requestMemoryStorage;
    RequestMemory& requestMemory = emplaceRequestMemory(
        requestMemoryStorage, *worker_, std::span<std::byte>(arenaBlock));
    auto* streamState = connection_.stream(streamId);
    if (streamState == nullptr) {
        co_return;
    }
    auto* streamRuntime = streamRuntimes_.find(streamId);
    if (streamRuntime == nullptr) {
        (void)connection_.submitReset(streamId, Http2ErrorCode::kInternalError);
        wakeWriter();
        co_return;
    }
    auto* selectedRoute = streamRuntime->selectedRoute();
    if (selectedRoute == nullptr) {
        (void)connection_.submitReset(streamId, Http2ErrorCode::kInternalError);
        wakeWriter();
        co_return;
    }
    auto& requestBody = selectedRoute->body();
    auto* streamingBody = requestBody.streaming();
    const auto* bufferedBody = requestBody.buffered();
    auto* streamSignal = streamRuntime->signal();
    if (streamSignal == nullptr) {
        (void)connection_.submitReset(streamId, Http2ErrorCode::kInternalError);
        wakeWriter();
        co_return;
    }
    HttpRequest request = HttpRequestAccess::make();
    const auto requestBuild = Http2RequestBuilder::build(
        *streamState,
        request,
        requestMemory.resource(),
        bufferedBody == nullptr ? std::string_view{} : bufferedBody->bytes());
    if (const auto* failure = requestBuild.failure()) {
        auto response = co_await routes_->handleError(
            request,
            requestMemory,
            copyHttpProtocolErrorInfo(requestMemory.resource(), failure->protocolError()),
            baseServices);
        (void)co_await bufferedResponseWriter_.write(
            streamId,
            response,
            httpBufferedResponseWritePlan(streamState->requestKnownMethod(), response));
        co_return;
    }

    HttpResponse response(requestMemory.resource());
    // Request negotiation must retain precompressed static sidecars even when
    // this worker cannot create a runtime encoder. The capability is enforced
    // later by the selected response representation.
    const auto responseCodingNegotiation = httpResponseCodingFor(request);
    auto responseCodingPolicy = HttpResponseCodingPolicy::disabled();
    if (const auto* selection = responseCodingNegotiation.selected()) {
        responseCodingPolicy = HttpResponseCodingPolicy::selected(*selection);
    } else {
        // Buffered routes may still produce a representation-free 204/205/304.
        // Preserve the negotiation failure until the response status is known;
        // rejecting here would make those responses incorrectly become 406.
        responseCodingPolicy = HttpResponseCodingPolicy::noAcceptableCoding();
    }
    const auto responseCodingAvailability = options.compression.has_value() ? HttpResponseCodingAvailability::kIdentityAndCompression : HttpResponseCodingAvailability::kIdentityOnly;
    do {
        if (responseCodingPolicy.selection() == nullptr) {
            response = co_await routes_->handleError(
                request,
                requestMemory,
                HttpErrorInfo(
                    ruvia::http_status::kNotAcceptable,
                    "not_acceptable",
                    "no acceptable response content coding"),
                baseServices);
            break;
        }
        const auto expectationPlan =
            streamState->expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
        if (const auto* rejection = expectationPlan.rejection()) {
            response = co_await routes_->handleError(
                request,
                requestMemory,
                copyHttpProtocolErrorInfo(
                    requestMemory.resource(), rejection->protocolError()),
                baseServices);
            break;
        }
        const auto& resolution = selectedRoute->resolution();
        const auto* resolved = resolution.resolved();

        const auto appRateLimit =
            decideRequestRateLimit(baseServices.rateLimiter(), remoteAddress_);
        if (const auto* rejection = appRateLimit.rejection()) {
            response = co_await routes_->handleError(
                request, requestMemory, rateLimitRejectionError(), baseServices);
            applyRateLimitRejectionHeaders(response, *rejection);
            break;
        }
        std::optional<BodyReaderBinding<Http2SansIoRequestBodyReader>> bodyReaderStorage;
        if (streamingBody != nullptr && streamState->tunnel().pending() == nullptr) {
            bodyReaderStorage.emplace(
                connection_,
                streamId,
                streamingBody->queue(),
                *streamSignal,
                writeSignal_);
        }
        auto dispatchServices = baseServices;
        if (bodyReaderStorage) {
            dispatchServices =
                dispatchServices.withStreamingRequestBody(bodyReaderStorage->facade());
        }

        const auto* webSocketEndpoint =
            resolved == nullptr ? nullptr : resolved->route().endpoint().webSocket();
        const auto* responseStreamEndpoint =
            resolved == nullptr ? nullptr : resolved->route().endpoint().responseStream();
        if (responseCodingPolicy.negotiationFailed() && (webSocketEndpoint != nullptr || responseStreamEndpoint != nullptr)) {
            // Streaming and upgrade routes commit before a buffered response
            // status can be inspected, so retain the pre-commit 406 boundary.
            response = co_await routes_->handleError(
                request,
                requestMemory,
                HttpErrorInfo(
                    ruvia::http_status::kNotAcceptable,
                    "not_acceptable",
                    "no acceptable response content coding"),
                baseServices);
            break;
        }
        if (webSocketEndpoint != nullptr) {
            const auto handshakeValidation =
                validateHttp2WebSocketHandshake(*streamState, request);
            if (handshakeValidation.accepted() != nullptr) {
                if (streamingBody == nullptr) {
                    (void)connection_.submitReset(
                        streamId, Http2ErrorCode::kInternalError);
                    wakeWriter();
                    co_return;
                }
                using WsTransport = Http2SansIoWsTransport<asio::any_io_executor>;
                using WsConnection = WebSocketConnection<WsTransport>;
                std::optional<WsConnection> webSocketConnection;
                auto upgradeAndRun = [&](Context& context) -> Task<void> {
                    auto negotiation = makeWebSocketServerNegotiation(
                        request,
                        webSocketEndpoint->subprotocols(),
                        requestMemory.resource());
                    const auto handshakeResult = connection_.submitWebSocketHandshake(
                        streamId, std::move(negotiation));
                    const auto* submittedHandshake = handshakeResult.submitted();
                    if (submittedHandshake == nullptr) {
                        co_return;
                    }
                    wakeWriter();
                    webSocketConnection.emplace(
                        WsTransport(
                            connection_,
                            streamId,
                            streamingBody->queue(),
                            *streamSignal,
                            writeSignal_,
                            executor_),
                        baseServices.worker(),
                        scannerEntry,
                        webSocketEndpoint->lifecycle(),
                        ProtocolByteLimit::limited(options.maxWebSocketMessageBytes),
                        requestMemory.resource(),
                        std::string_view{},
                        submittedHandshake->deflate());
                    co_await invokeWebSocketHandler(
                        *webSocketConnection,
                        scannerEntry,
                        webSocketEndpoint->handler(),
                        context);
                };
                const auto terminal = makeCallableRef<void, Context&>(upgradeAndRun);
                std::optional<HttpResponse> buffered;
                std::exception_ptr exception;
                try {
                    buffered = co_await routes_->dispatchWebSocket(
                        request,
                        *resolved,
                        requestMemory,
                        terminal,
                        dispatchServices);
                } catch (...) {
                    exception = std::current_exception();
                }
                if (webSocketConnection.has_value()) {
                    co_await finishWebSocketSession(
                        *webSocketConnection,
                        exception,
                        options.connectionFailure,
                        remoteAddress_);
                    co_return;
                }
                if (exception != nullptr) {
                    std::rethrow_exception(exception);
                }
                if (!buffered.has_value()) {
                    co_return;
                }
                response = std::move(*buffered);
                break;
            }
            const auto* failure = handshakeValidation.failure();
            if (failure == nullptr) {
                throw std::logic_error(
                    "HTTP/2 WebSocket validation returned no outcome");
            }
            response = co_await routes_->handleError(
                request,
                requestMemory,
                copyHttpProtocolErrorInfo(
                    requestMemory.resource(), failure->protocolError()),
                baseServices);
            failure->applyRequiredResponseHeaders(response);
        } else if (responseStreamEndpoint != nullptr) {
            Http2SansIoResponseStreamSink sink(
                connection_,
                streamId,
                responseStreamEndpoint->kind(),
                baseServices.worker(),
                writeSignal_,
                *streamSignal,
                workerResource(),
                request.knownMethod(),
                *responseCodingPolicy.selection(),
                responseCodingAvailability);
            auto result = co_await dispatchResponseStreamWith(
                sink,
                *routes_,
                request,
                *resolved,
                requestMemory,
                dispatchServices,
                [this, streamId]() noexcept {
                    auto* stream = connection_.stream(streamId);
                    return stream == nullptr || stream->isAborted();
                });
            if (result.peerAbortedBeforeCommit() != nullptr) {
                co_return;
            }
            if (const auto committedStatus = result.committedStatus()) {
                if (const auto* failed = result.failedAfterCommit()) {
                    (void)connection_.submitReset(
                        streamId, Http2ErrorCode::kInternalError);
                    wakeWriter();
                    options.connectionFailure.invoke(
                        remoteAddress_, failed->exception());
                }
                recordHttpAccess(
                    options.accessLog,
                    request,
                    remoteAddress_,
                    *committedStatus,
                    requestStart);
                co_return;
            }
            if (auto* routeResponse = result.routeResponse()) {
                response = std::move(*routeResponse).takeResponse();
            } else if (auto* recovered = result.recoveredFailure()) {
                response = std::move(*recovered).takeResponse();
            } else {
                throw std::logic_error(
                    "response stream dispatch returned no HTTP/2 terminal alternative");
            }
        } else {
            auto bufferedResult = co_await routes_->dispatchBufferedResponse(
                request,
                resolution,
                requestMemory,
                options.documentRoot.binding(),
                dispatchServices,
                baseServices.deferredStaticFileCompression() ? StaticFileSelectionMode::kAllowDeferredCompression : StaticFileSelectionMode::kStrict);
            response = std::move(bufferedResult).takeResponse();
        }

        if (baseServices.deferredStaticFileCompression() && responseBody(response).file().has_value()) {
            const auto compressionResult = co_await tryCompressStaticFileResponse(
                response,
                *responseCodingPolicy.selection(),
                request.knownMethod(),
                *options.compression,
                options.documentRoot.runtimeOptions.onDemandCompressionMaxBytes,
                options.blockingPool,
                baseServices.worker());
            if (!compressionResult.compressed() && httpResponseNeedsNotAcceptable(responseCodingPolicy, request, response) && responseBody(response).file().has_value()) {
                response = co_await routes_->handleError(
                    request,
                    requestMemory,
                    httpStaticFileCompressionError(compressionResult),
                    baseServices);
            }
        }
    } while (false);

    auto preparation = prepareBufferedHttpResponse(request, responseCodingPolicy, response, options);
    if (const auto error = httpBufferedResponsePreparationError(responseCodingPolicy, request, response, preparation.compressionResult())) {
        response = co_await routes_->handleError(
            request,
            requestMemory,
            *error,
            baseServices);
        preparation = prepareBufferedHttpResponse(request, responseCodingPolicy, response, options);
        if (httpBufferedResponsePreparationError(responseCodingPolicy, request, response, preparation.compressionResult()).has_value()) {
            // The negotiated coding could not be installed even on the
            // generated terminal error. This is an explicit terminal error
            // representation, not a silent identity fallback of the original
            // application response.
            responseCodingPolicy = HttpResponseCodingPolicy::disabled();
            preparation = prepareBufferedHttpResponse(request, responseCodingPolicy, response, options);
        }
    }
    const auto writePlan = preparation.writePlan();
    const auto result =
        co_await bufferedResponseWriter_.write(streamId, response, writePlan);
    if (const auto committedStatus = result.committedStatus()) {
        recordHttpAccess(
            options.accessLog,
            request,
            remoteAddress_,
            *committedStatus,
            requestStart);
    }
}

Task<void> Http2SansIoSessionEngine::dispatchOne(std::uint32_t streamId) {
    try {
        co_await dispatchOneInner(streamId);
    } catch (...) {
        auto* live = connection_.stream(streamId);
        if (live != nullptr && !live->isAborted()) {
            (void)connection_.submitReset(streamId, Http2ErrorCode::kInternalError);
        }
    }
    (void)streamRuntimes_.remove(streamId);
    connection_.unpinStream(streamId);
    wakeWriter();
}

bool Http2SansIoSessionEngine::admitStream(std::uint32_t streamId) {
    auto* signal =
        streamRuntimes_.beginDispatch(streamId, session_.services().worker());
    if (signal == nullptr) {
        return false;
    }
    connection_.pinStream(streamId);
    ++activeHandlerTasks_;
    try {
        asio::co_spawn(
            executor_,
            taskAsAwaitable(dispatchOne(streamId)),
            asio::bind_allocator(
                asio::recycling_allocator<void>(),
                [this](std::exception_ptr exception) noexcept {
                    if (exception != nullptr) {
                        terminate(
                            std::make_error_code(std::errc::operation_canceled));
                    }
                    --activeHandlerTasks_;
                    if (activeHandlerTasks_ == 0) {
                        handlerFinished_.notify();
                    }
                    writeSignal_.notify();
                }));
    } catch (...) {
        --activeHandlerTasks_;
        connection_.unpinStream(streamId);
        (void)streamRuntimes_.remove(streamId);
        return false;
    }
    return true;
}

void Http2SansIoSessionEngine::drainEvents() {
    const auto& options = session_.options();
    std::array<std::uint32_t, Http2LocalSettings::kMaxConcurrentStreams>
        copiedBodyStreams{};
    std::size_t copiedBodyStreamCount = 0;
    const auto markBufferedBodyCopied = [&](std::uint32_t streamId) {
        const auto copied =
            std::span(copiedBodyStreams).first(copiedBodyStreamCount);
        if (std::ranges::find(copied, streamId) == copied.end()) {
            if (copiedBodyStreamCount == copiedBodyStreams.size()) {
                return false;
            }
            copiedBodyStreams[copiedBodyStreamCount++] = streamId;
        }
        return true;
    };
    const auto unmarkBufferedBodyCopied = [&](std::uint32_t streamId) {
        const auto copied =
            std::span(copiedBodyStreams).first(copiedBodyStreamCount);
        const auto found = std::ranges::find(copied, streamId);
        if (found == copied.end()) {
            return;
        }
        --copiedBodyStreamCount;
        *found = copiedBodyStreams[copiedBodyStreamCount];
    };
    const auto resetEventStream = [&](std::uint32_t streamId, Http2ErrorCode error) {
        unmarkBufferedBodyCopied(streamId);
        auto* signal = streamRuntimes_.signalFor(streamId);
        (void)connection_.submitReset(streamId, error);
        if (signal != nullptr) {
            signal->wake();
        } else {
            (void)streamRuntimes_.remove(streamId);
        }
        wakeWriter();
    };
    const auto resolveStreamRoute = [&](Http2StreamState& streamState) {
        return http2SelectStreamRoute(*routes_, streamRuntimes_, streamState);
    };
    const auto onMessageHead = [&](const auto* messageHead) {
        const auto streamId = messageHead->streamId();
        ++acceptedRequestHeads_;
        if (!connection_.draining() && options.keepaliveRequests.has_value() &&
            acceptedRequestHeads_ >= *options.keepaliveRequests) {
            connection_.beginDrain();
            wakeWriter();
        }
        auto* streamState = connection_.stream(streamId);
        if (streamState == nullptr) {
            return;
        }
        auto* streamRuntime = resolveStreamRoute(*streamState);
        if (streamRuntime == nullptr) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
            return;
        }
        const auto expectationPlan =
            streamState->expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
        if (expectationPlan.sendContinue() != nullptr) {
            const auto status = connection_.submitInterimResponseHead(
                streamId, HttpInterimResponseHead(ruvia::http_status::kContinue));
            if (status == Http2SubmitStatus::kAccepted) {
                wakeWriter();
            } else {
                if (status != Http2SubmitStatus::kClosed) {
                    resetEventStream(streamId, Http2ErrorCode::kInternalError);
                } else {
                    (void)streamRuntimes_.remove(streamId);
                }
                return;
            }
        }
        const bool connectRequest = streamState->tunnel().pending() != nullptr;
        const auto* selectedRoute = streamRuntime->selectedRoute();
        const bool streamingBody =
            !connectRequest && selectedRoute != nullptr &&
            selectedRoute->body().streaming() != nullptr &&
            streamState->remoteReceive().contentOpen() != nullptr;
        if (expectationPlan.rejection() != nullptr || connectRequest ||
            streamingBody) {
            if (!admitStream(streamId)) {
                resetEventStream(streamId, Http2ErrorCode::kInternalError);
            }
        }
    };
    const auto onBodyChunk = [&](const auto* bodyChunk) {
        const auto streamId = bodyChunk->streamId();
        auto* streamState = connection_.stream(streamId);
        auto* streamRuntime = streamRuntimes_.find(streamId);
        if (streamState == nullptr || streamRuntime == nullptr) {
            if (streamState != nullptr) {
                resetEventStream(streamId, Http2ErrorCode::kInternalError);
            }
            return;
        }
        auto* selectedRoute = streamRuntime->selectedRoute();
        if (selectedRoute == nullptr) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
            return;
        }
        auto& requestBody = selectedRoute->body();
        const auto totalLimit = requestBodyByteLimit(
            requestBody.mode(),
            options.maxStreamBodyBytes,
            options.maxBufferedBodyBytes);
        const auto stored = requestBody.store(
            bodyChunk->bytes(), totalLimit, options.maxBufferedBodyBytes);
        if (stored.stored() == nullptr) {
            const bool knownRejection = stored.protocolFailure() != nullptr ||
                                        stored.backlogOverflow() != nullptr;
            resetEventStream(
                streamId,
                knownRejection ? Http2ErrorCode::kCancel
                               : Http2ErrorCode::kInternalError);
            return;
        }
        if (requestBody.streaming() != nullptr) {
            auto* signal = streamRuntime->signal();
            if (signal == nullptr) {
                resetEventStream(streamId, Http2ErrorCode::kInternalError);
                return;
            }
            signal->wake();
        } else if (!markBufferedBodyCopied(streamId)) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
        }
    };
    const auto onTunnelData = [&](const auto* tunnelData) {
        const auto streamId = tunnelData->streamId();
        auto* streamState = connection_.stream(streamId);
        auto* streamRuntime = streamRuntimes_.find(streamId);
        auto* signal =
            streamRuntime != nullptr ? streamRuntime->signal() : nullptr;
        if (streamState == nullptr || streamRuntime == nullptr || signal == nullptr) {
            if (streamState != nullptr) {
                resetEventStream(streamId, Http2ErrorCode::kInternalError);
            }
            return;
        }
        auto* selectedRoute = streamRuntime->selectedRoute();
        auto* streamingBody =
            selectedRoute != nullptr ? selectedRoute->body().streaming() : nullptr;
        if (streamingBody == nullptr) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
            return;
        }
        streamingBody->queue().enqueue(tunnelData->bytes());
        signal->wake();
    };
    const auto onTunnelEnd = [&](const auto* tunnelEnd) {
        if (auto* signal = streamRuntimes_.signalFor(tunnelEnd->streamId())) {
            signal->wake();
        }
    };
    const auto onMessageEnd = [&](const auto* messageEnd) {
        const auto streamId = messageEnd->streamId();
        if (connection_.stream(streamId) == nullptr) {
            return;
        }
        auto* streamRuntime = streamRuntimes_.find(streamId);
        if (streamRuntime == nullptr) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
            return;
        }
        if (auto* signal = streamRuntime->signal()) {
            signal->wake();
        } else if (!admitStream(streamId)) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
        }
    };
    const auto onStreamClosed = [&](const auto* streamClosed) {
        const auto streamId = streamClosed->streamId();
        unmarkBufferedBodyCopied(streamId);
        auto* streamRuntime = streamRuntimes_.find(streamId);
        auto* signal =
            streamRuntime != nullptr ? streamRuntime->signal() : nullptr;
        if (signal != nullptr) {
            signal->wake();
        } else {
            (void)streamRuntimes_.remove(streamId);
        }
    };

    for (;;) {
        const auto event = connection_.nextEvent();
        if (!event.has_value()) {
            break;
        }
        if (const auto* messageHead = event->messageHead()) {
            onMessageHead(messageHead);
        } else if (const auto* bodyChunk = event->messageBodyChunk()) {
            onBodyChunk(bodyChunk);
        } else if (const auto* tunnelData = event->tunnelData()) {
            onTunnelData(tunnelData);
        } else if (const auto* tunnelEnd = event->tunnelEnd()) {
            onTunnelEnd(tunnelEnd);
        } else if (const auto* messageEnd = event->messageEnd()) {
            onMessageEnd(messageEnd);
        } else if (const auto* streamClosed = event->streamClosed()) {
            onStreamClosed(streamClosed);
        }
    }
    for (std::size_t i = 0; i < copiedBodyStreamCount; ++i) {
        connection_.releaseReceivedData(copiedBodyStreams[i]);
    }
    if (copiedBodyStreamCount != 0) {
        wakeWriter();
    }
    for (const auto streamId : connection_.takeDrainedDataStreams()) {
        if (auto* signal = streamRuntimes_.signalFor(streamId)) {
            signal->wake();
        }
    }
}

Http2FeedResult Http2SansIoSessionEngine::feedAndDrain(std::string_view bytes) {
    for (;;) {
        const auto result = connection_.feed(bytes);
        drainEvents();
        if (result != Http2FeedResult::kEventsPending) {
            return result;
        }
    }
}

Task<void> Http2SansIoSessionEngine::finish() {
    lifecycle_.beginStopping();
    wakeWriter();
    while (activeHandlerTasks_ != 0) {
        co_await handlerFinished_.wait();
    }
    wakeWriter();
    while (!writerTaskDone_) {
        co_await writerFinished_.wait();
    }
}

}  // namespace ruvia::detail
