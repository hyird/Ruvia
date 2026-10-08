#include "http2/Http2SansIoSessionEngine.h"

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

#include "ruvia/core/Async.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/http/HttpRequestTarget.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpResponseServer.h"

#include "body/HttpRequestBodyFacade.h"
#include "context/HttpConnectionAdvertisementOutput.h"
#include "context/HttpInterimResponseOutput.h"
#include "context/HttpPushOutput.h"
#include "http/HttpProtocolErrorInfo.h"
#include "http/HttpTunnelSession.h"
#include "http2/Http2SansIoRequestBody.h"
#include "http2/Http2SansIoResponseStreamSink.h"
#include "http2/Http2SansIoRouteSelection.h"
#include "http2/Http2SansIoWsTransport.h"
#include "ratelimit/RateLimitDecision.h"
#include "router/RouteResolution.h"
#include "router/RouteTable.h"
#include "server/HttpBufferedResponse.h"
#include "server/HttpResponseStreamDispatch.h"
#include "server/HttpServerAccessLog.h"
#include "server/RequestBodyLimit.h"
#include "server/RequestMemoryArena.h"
#include "websocket/HttpWebSocketConnection.h"
#include "websocket/HttpWebSocketSession.h"
#include "websocket/WebSocketResponseHeaders.h"

namespace ruvia::detail {

Http2SansIoSessionEngine::Http2SansIoSessionEngine(asio::any_io_executor executor,
    asio::ip::tcp::socket& socket, const RouteTable& routes, WorkerMemory& worker,
    Http2SansIoSessionContext session)
    : executor_(std::move(executor)),
      socket_(socket),
      routes_(routes),
      worker_(worker),
      session_(std::move(session)),
      inbound_buffers_(session_.options().inbound_buffer_pool != nullptr
                           ? session_.options().inbound_buffer_pool
                           : worker.resource(),
          session_.options().max_inbound_buffer_bytes_per_connection),
      remoteAddress_(session_.services().connInfo().remote().address()),
      connection_(ruvia::Http2Connection::server({.resource = worker.resource()})),
      writeSignal_(session_.services().worker()),
      outputBudget_(session_.services().worker()),
      handlerFinished_(session_.services().worker()),
      writerFinished_(session_.services().worker()),
      streamRuntimes_(worker.resource(), termination_, &inbound_buffers_),
      bufferedResponseWriter_(connection_, streamRuntimes_, worker, writeSignal_, outputBudget_) {}

bool Http2SansIoSessionEngine::wantsWrite() const noexcept {
    return connection_.wantsWrite();
}

void Http2SansIoSessionEngine::takeOutput(std::pmr::string& output) {
    // Preserve assign() semantics when the caller reuses its scratch string. The
    // core batch operation only consumes output and invokes its observer after the
    // copy succeeds, so allocation failure leaves protocol bytes and budget intact.
    output.clear();
    const auto result = connection_.takeOutputBatch(kHttp2DataOutputCreditBytes, output, [](void* context, std::uint32_t streamId, std::size_t payloadBytes) noexcept { static_cast<Http2DataOutputBudget*>(context)->noteDataOutput(streamId, payloadBytes); }, &outputBudget_);
    if (result.status == Http2OutputBatchStatus::kUnaligned) {
        // This driver exclusively consumes complete batches; an unaligned cursor
        // would mean another consumer violated that ownership contract.
        std::terminate();
    }
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

void Http2SansIoSessionEngine::outputWriteCompleted() noexcept {
    outputBudget_.reconcile(connection_, true);
}

void Http2SansIoSessionEngine::writerWriteFailed(std::error_code error) noexcept {
    lifecycle_.markWriteFailed();
    terminate(error);
}

void Http2SansIoSessionEngine::writer_submitting() noexcept {
    lifecycle_.mark_writer_submitted();
}

void Http2SansIoSessionEngine::writer_launch_failed() noexcept {
    lifecycle_.mark_writer_launch_failed();
}

void Http2SansIoSessionEngine::writerCompleted(std::exception_ptr exception) noexcept {
    if (exception != nullptr) {
        lifecycle_.recordWriterFailure(std::move(exception));
        terminate(std::make_error_code(std::errc::operation_canceled));
    }
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
        connection_.headerBlockInProgress(), streamRuntimes_.size(),
        streamRuntimes_.tunnelCount() != 0));
}

void Http2SansIoSessionEngine::removeStreamRuntime(std::uint32_t streamId) noexcept {
    if (streamRuntimes_.remove(streamId)) {
        setInactivityPhase();
    }
}

void Http2SansIoSessionEngine::touchActivity() noexcept {
    session_.scannerEntry().touch();
}

void Http2SansIoSessionEngine::wakeWriter() noexcept {
    outputBudget_.wake();
    writeSignal_.notify();
}

void Http2SansIoSessionEngine::terminate(std::error_code error) noexcept {
    if (!termination_.terminate(error)) {
        return;
    }
    outputBudget_.wake();
    std::error_code ignored;
    socket_.cancel(ignored);
    streamRuntimes_.forEach([](Http2SansIoStreamRuntime& runtime) {
        if (auto* signal = runtime.signal()) {
            signal->wake();
        }
    });
    writeSignal_.notify();
}

void Http2SansIoSessionEngine::resetStreamNoThrow(
    std::uint32_t streamId, Http2ErrorCode error) noexcept {
    try {
        (void)connection_.submitReset(streamId, error);
    } catch (...) {
        terminate(std::make_error_code(std::errc::not_enough_memory));
    }
    outputBudget_.releaseAndReconcile(streamId, connection_);
    outputBudget_.wake();
}

std::pmr::memory_resource* Http2SansIoSessionEngine::workerResource() const noexcept {
    return worker_.resource();
}

Task<bool> Http2SansIoSessionEngine::pushRequest(std::uint32_t associatedStreamId, HttpPushRequestView request) {
    auto* parent = streamRuntimes_.find(associatedStreamId);
    const auto* head = parent != nullptr ? parent->requestHead() : nullptr;
    if (head == nullptr || (associatedStreamId & 1U) == 0) {
        co_return false;
    }
    const auto& original = head->request();
    if (!httpAsciiEqualsIgnoreCase(original.scheme(), request.scheme) ||
        !httpAuthoritiesEqual(BorrowedText(original.authority()), BorrowedText(request.authority),
            original.scheme() == "https" ? 443 : 80)) {
        throw std::invalid_argument("push request must use its associated request origin");
    }
    auto promised = connection_.submitPushRequest(associatedStreamId, request);
    if (!promised) {
        if (promised.error() == Http2PushSubmitError::kInvalidRequest) {
            throw std::invalid_argument("invalid HTTP/2 push request");
        }
        co_return false;
    }
    const auto id = promised->streamId();
    try {
        const auto requestView = connection_.server_request_view(id);
        auto* runtime = requestView ? http2SelectStreamRoute(routes_, *requestView, streamRuntimes_, id) : nullptr;
        if (runtime == nullptr || !runtime->holdRequestHead(std::move(*promised))) {
            throw std::logic_error("HTTP/2 push dispatch admission failed");
        }
    } catch (...) {
        resetStreamNoThrow(id, Http2ErrorCode::kCancel);
        removeStreamRuntime(id);
        wakeWriter();
        throw;
    }
    streamRuntimes_.find(id)->bindPushParent(associatedStreamId);
    if (!admitStream(id)) {
        resetStreamNoThrow(id, Http2ErrorCode::kCancel);
        wakeWriter();
        co_return false;
    }
    wakeWriter();
    co_return true;
}

Task<void> Http2SansIoSessionEngine::dispatchOneInner(std::uint32_t streamId) {
    const auto requestStart = std::chrono::steady_clock::now();
    const auto& options = session_.options();
    auto& scannerEntry = session_.scannerEntry();
    const auto baseServices = session_.services().with_inbound_buffer_pool(inbound_buffers_);

    std::array<std::byte, kRequestArenaStackBytes> arenaBlock;
    std::optional<RequestMemory> requestMemoryStorage;
    RequestMemory& requestMemory =
        emplaceRequestMemory(requestMemoryStorage, worker_, std::span<std::byte>(arenaBlock));
    auto* streamRuntime = streamRuntimes_.find(streamId);
    auto* requestHead = streamRuntime != nullptr ? streamRuntime->requestHead() : nullptr;
    if (requestHead == nullptr) {
        co_return;
    }
    const auto requestMethod = requestHead->request().knownMethod();
    auto* selectedRoute = streamRuntime->selectedRoute();
    if (selectedRoute == nullptr) {
        resetStreamNoThrow(streamId, Http2ErrorCode::kInternalError);
        wakeWriter();
        co_return;
    }
    auto& requestBody = selectedRoute->body();
    auto* streamingBody = requestBody.streaming();
    const auto* bufferedBody = requestBody.buffered();
    auto* streamSignal = streamRuntime->signal();
    if (streamSignal == nullptr) {
        resetStreamNoThrow(streamId, Http2ErrorCode::kInternalError);
        wakeWriter();
        co_return;
    }
    auto requestBuild = makeHttp2ServerRequest(connection_, streamId, requestMemory.resource(),
        bufferedBody == nullptr ? std::string_view{} : bufferedBody->bytes());
    if (!requestBuild) {
        auto request = makeParsedHttpRequest(
            "GET", "/", {}, {}, requestMemory.resource())
                           .first;
        auto response = co_await routes_.handleError(request, requestMemory,
            copyHttpProtocolErrorInfo(requestMemory.resource(), requestBuild.error()),
            baseServices);
        (void)co_await bufferedResponseWriter_.write(streamId, response,
            planBufferedHttpResponseWrite(requestMethod, response));
        co_return;
    }

    HttpRequest request = std::move(*requestBuild);
    HttpResponse response({.resource = requestMemory.resource()});
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
    const auto responseCodingAvailability =
        options.compression.has_value() ? HttpResponseCodingAvailability::kIdentityAndCompression
                                        : HttpResponseCodingAvailability::kIdentityOnly;
    struct ResponseOutputTarget {
        ::ruvia::Http2Connection& connection;
        WorkerSignal& writer;
        std::uint32_t streamId;
    } outputTarget{connection_, writeSignal_, streamId};
    HttpInterimResponseOutput interimOutput(workerResource(), &outputTarget, [](void* raw, const HttpInterimResponseHead& head) -> Task<void> {
        auto& target = *static_cast<ResponseOutputTarget*>(raw);
        const auto status = target.connection.submitInterimResponseHead(target.streamId, head);
        if (status != Http2SubmitStatus::kAccepted) {
            throw std::invalid_argument("HTTP/2 interim response rejected");
        }
        target.writer.notify();
        co_return;
    });
    HttpConnectionAdvertisementOutput advertisements(workerResource(), &outputTarget, [](void* raw, std::span<const std::string_view> origins) -> Task<void> {
            auto& target = *static_cast<ResponseOutputTarget*>(raw);
            if (target.connection.submitOriginAdvertisement(origins) != Http2SubmitStatus::kAccepted) {
                throw std::invalid_argument("HTTP/2 ORIGIN advertisement rejected");
            }
            target.writer.notify();
            co_return; }, [](void* raw, std::string_view value) -> Task<void> {
            auto& target = *static_cast<ResponseOutputTarget*>(raw);
            if (target.connection.submitAlternativeServiceAdvertisement(target.streamId, {}, value) != Http2SubmitStatus::kAccepted) {
                throw std::invalid_argument("HTTP/2 ALTSVC advertisement rejected");
            }
            target.writer.notify();
            co_return; });
    struct PushTarget {
        Http2SansIoSessionEngine& owner;
        std::uint32_t streamId;
    } pushTarget{*this, streamId};
    HttpPushOutput pushOutput(workerResource(), &pushTarget, [](void* raw, HttpPushRequestView promisedRequest) -> Task<bool> {
        auto& target = *static_cast<PushTarget*>(raw);
        co_return co_await target.owner.pushRequest(target.streamId, promisedRequest);
    });
    auto requestServices = baseServices.withPushOutput(pushOutput).withRequestTrailers(streamRuntime->trailers()).withRequestPriorityUpdate(streamRuntime->priorityUpdate()).withInterimOutput(interimOutput).withConnectionAdvertisements(advertisements);
    do {
        const auto& resolution = selectedRoute->resolution();
        const auto* resolved = resolution.resolved();

        // Armed on the stream's own state so concurrent requests on one
        // connection each get their own clock. It happens before every
        // server-layer rejection below: custom onError/middleware is a handler
        // too and must see the request stop token.
        const auto handlerDeadline = effectiveHandlerDeadline(
            options.deadline ? std::optional{options.deadline->handler} : std::nullopt,
            resolved != nullptr ? resolved->route().deadlineMs() : 0);
        if (handlerDeadline > std::chrono::milliseconds::zero()) {
            selectedRoute->armDeadline(
                baseServices.worker(), baseServices.stopToken(), handlerDeadline);
            requestServices = requestServices.withRequestDeadline(*selectedRoute->deadline());
        }

        const auto expectationPlan =
            requestHead->expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
        if (const auto* rejection = expectationPlan.rejection()) {
            response = co_await routes_.handleError(request, requestMemory,
                copyHttpProtocolErrorInfo(requestMemory.resource(), rejection->protocolError()),
                requestServices);
            break;
        }

        // Resolved per request: one HTTP/2 connection multiplexes many, each
        // carrying its own forwarding headers.
        const auto clientAddress = baseServices.resolveConnInfo(request).client().address();
        const auto appRateLimit = decideRequestRateLimit(baseServices.rateLimiter(), clientAddress);
        if (const auto* rejection = appRateLimit.rejection()) {
            response = co_await routes_.handleError(
                request, requestMemory, rateLimitRejectionError(), requestServices);
            applyRateLimitRejectionHeaders(response, *rejection);
            break;
        }
        std::optional<BodyReaderBinding<Http2SansIoRequestBodyReader>> bodyReaderStorage;
        if (streamingBody != nullptr && !requestHead->snapshot().connectPending) {
            bodyReaderStorage.emplace(
                connection_, streamId, streamingBody->queue(), *streamSignal);
        }
        auto dispatchServices = requestServices;
        if (bodyReaderStorage) {
            dispatchServices =
                dispatchServices.withStreamingRequestBody(bodyReaderStorage->facade());
        }

        const auto* webSocketEndpoint =
            resolved == nullptr ? nullptr : resolved->route().endpoint().webSocket();
        const auto* responseStreamEndpoint =
            resolved == nullptr ? nullptr : resolved->route().endpoint().responseStream();
        if (responseCodingPolicy.negotiationFailed() && responseStreamEndpoint != nullptr) {
            // Streaming routes commit before a buffered response status can be
            // inspected. WebSocket Extended CONNECT does not select an HTTP
            // response representation.
            response = co_await routes_.handleError(request, requestMemory,
                HttpErrorInfo({
                    .status = ruvia::http_status::kNotAcceptable,
                    .code = "not_acceptable",
                    .message = "no acceptable response content coding",
                }),
                requestServices);
            break;
        }
        if (const auto* tunnelEndpoint = resolved == nullptr ? nullptr : resolved->route().endpoint().tunnel()) {
            const bool udp = tunnelEndpoint->protocol() == "connect-udp";
            if (udp && !validateHttpConnectUdpRequest(request)) {
                response = co_await routes_.handleError(request, requestMemory,
                    HttpErrorInfo({.status = http_status::kBadRequest, .message = "invalid CONNECT-UDP request head"}), requestServices);
                break;
            }
            if (streamingBody == nullptr || !requestHead->snapshot().connectPending) {
                resetStreamNoThrow(streamId, Http2ErrorCode::kInternalError);
                wakeWriter();
                co_return;
            }
            using Transport = Http2SansIoTunnelTransport<asio::any_io_executor>;
            std::optional<HttpTunnelSession<Transport>> tunnelSession;
            auto establishAndRun = [&](Context& context) -> Task<void> {
                auto head = ContextAccess::streamingHead(context);
                if (udp) {
                    auto negotiated = prepareHttpConnectUdpResponse(std::move(head), HttpProtocolVersion::kHttp2);
                    if (!negotiated) {
                        throw std::invalid_argument("invalid CONNECT-UDP response metadata");
                    }
                    head = std::move(*negotiated);
                }
                const auto committed = connection_.submitConnectResponseHead(streamId, head);
                if (committed != Http2SubmitStatus::kAccepted) {
                    throw std::invalid_argument("HTTP/2 CONNECT response head rejected");
                }
                if (!streamRuntimes_.markTunnel(streamId)) {
                    std::terminate();
                }
                setInactivityPhase();
                ContextAccess::markTunnelHandshakeStarted(context);
                wakeWriter();
                tunnelSession.emplace(Transport(connection_, streamId, streamingBody->queue(), *streamSignal,
                                          writeSignal_, outputBudget_, executor_),
                    baseServices.worker(), *context.pool());
                co_await invokeTunnelHandler(*tunnelSession, scannerEntry, tunnelEndpoint->handler(), context);
            };
            const auto terminal = makeCallableRef<void, Context&>(establishAndRun);
            std::optional<HttpResponse> buffered;
            std::exception_ptr exception;
            try {
                buffered = co_await routes_.dispatchTunnel(request, *resolved, requestMemory, terminal, dispatchServices);
            } catch (...) {
                exception = std::current_exception();
            }
            if (tunnelSession.has_value()) {
                co_await finishTunnelSession(*tunnelSession, exception, options.connectionFailure, remoteAddress_, scannerEntry, tunnelEndpoint->config().peerTransportFinTimeout);
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
        if (webSocketEndpoint != nullptr) {
            const auto handshakeValidation = ruvia::validateHttp2WebSocketHandshake(
                connection_, streamId, request);
            if (handshakeValidation.accepted() != nullptr) {
                if (streamingBody == nullptr) {
                    resetStreamNoThrow(streamId, Http2ErrorCode::kInternalError);
                    wakeWriter();
                    co_return;
                }
                using WsTransport = Http2SansIoWsTransport<asio::any_io_executor>;
                using WsConnection = WebSocketConnection<WsTransport>;
                std::optional<WsConnection> webSocketConnection;
                auto upgradeAndRun = [&](Context& context) -> Task<void> {
                    const auto responseHeaders = webSocketResponseHeaders(context);
                    const auto handshakeResult = connection_.submitWebSocketHandshake(
                        streamId, request, handshakeValidation,
                        {.supportedSubprotocols = webSocketEndpoint->subprotocols(),
                            .responseHeaders = responseHeaders});
                    const auto* submittedHandshake = handshakeResult.submitted();
                    if (submittedHandshake == nullptr) {
                        co_return;
                    }
                    if (!streamRuntimes_.markTunnel(streamId)) {
                        std::terminate();
                    }
                    setInactivityPhase();
                    ContextAccess::markWebSocketHandshakeStarted(context);
                    wakeWriter();
                    webSocketConnection.emplace(
                        WsTransport(connection_, streamId, streamingBody->queue(), *streamSignal,
                            writeSignal_, outputBudget_, executor_),
                        baseServices.worker(), scannerEntry, webSocketEndpoint->lifecycle(),
                        ProtocolByteLimit::limited(options.max_web_socket_message_bytes),
                        &inbound_buffers_, std::string_view{},
                        submittedHandshake->compression(), webSocketEndpoint->deflate().compressionLevel);
                    co_await invokeWebSocketHandler(
                        *webSocketConnection, scannerEntry, webSocketEndpoint->handler(), context);
                };
                const auto terminal = makeCallableRef<void, Context&>(upgradeAndRun);
                std::optional<HttpResponse> buffered;
                std::exception_ptr exception;
                try {
                    buffered = co_await routes_.dispatchWebSocket(
                        request, *resolved, requestMemory, terminal, dispatchServices);
                } catch (...) {
                    exception = std::current_exception();
                }
                if (webSocketConnection.has_value()) {
                    co_await finishWebSocketSession(
                        *webSocketConnection, exception, options.connectionFailure, remoteAddress_);
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
                throw std::logic_error("HTTP/2 WebSocket validation returned no outcome");
            }
            response = co_await routes_.handleError(request, requestMemory,
                copyHttpProtocolErrorInfo(requestMemory.resource(), failure->protocolError()),
                requestServices);
            failure->applyRequiredResponseHeaders(response);
        } else if (responseStreamEndpoint != nullptr) {
            Http2SansIoResponseStreamSink sink(connection_, streamId,
                responseStreamEndpoint->kind(), writeSignal_, *streamSignal, outputBudget_,
                workerResource(),
                request.knownMethod(), *responseCodingPolicy.selection(),
                responseCodingAvailability);
            auto result = co_await dispatchResponseStreamWith(sink, routes_, request, *resolved,
                requestMemory, dispatchServices,
                [this, streamId]() noexcept { return connection_.streamAborted(streamId); });
            if (result.peerAbortedBeforeCommit() != nullptr) {
                co_return;
            }
            if (const auto committedStatus = result.committedStatus()) {
                if (const auto* failed = result.failedAfterCommit()) {
                    resetStreamNoThrow(streamId, Http2ErrorCode::kInternalError);
                    wakeWriter();
                    options.connectionFailure.invoke(remoteAddress_, failed->exception());
                }
                recordHttpAccess(options.accessLog, request,
                    baseServices.resolveConnInfo(request).client().address(), *committedStatus,
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
            response = co_await routes_.dispatchBufferedResponse(request, resolution, requestMemory,
                options.documentRoot.binding(), dispatchServices,
                baseServices.precompressedStaticFiles() ? StaticFileSelectionMode::kPrecompressed
                                                        : StaticFileSelectionMode::kIdentityOnly);
        }

    } while (false);

    const auto preparation = co_await prepare_application_response(
        request, responseCodingPolicy, response, options, routes_, requestMemory, requestServices);
    if (!preparation) {
        resetStreamNoThrow(streamId, Http2ErrorCode::kCancel);
        wakeWriter();
        co_return;
    }
    const auto writePlan = preparation->write_plan;
    const auto result = co_await bufferedResponseWriter_.write(streamId, response, writePlan);
    if (const auto committedStatus = result.committedStatus()) {
        recordHttpAccess(options.accessLog, request,
            baseServices.resolveConnInfo(request).client().address(), *committedStatus,
            requestStart);
    }
}

Task<void> Http2SansIoSessionEngine::dispatchOne(std::uint32_t streamId) {
    try {
        co_await dispatchOneInner(streamId);
    } catch (...) {
        const auto failure = std::current_exception();
        if (!connection_.streamAborted(streamId)) {
            resetStreamNoThrow(streamId, Http2ErrorCode::kInternalError);
        }
        session_.options().connectionFailure.invoke(remoteAddress_, failure);
    }
    if (auto* runtime = streamRuntimes_.find(streamId)) {
        if (auto* requestHead = runtime->requestHead()) {
            (void)connection_.release(std::move(*requestHead));
        }
    }
    removeStreamRuntime(streamId);
    wakeWriter();
}

bool Http2SansIoSessionEngine::admitStream(std::uint32_t streamId) {
    auto* signal = streamRuntimes_.beginDispatch(streamId, session_.services().worker());
    if (signal == nullptr) {
        return false;
    }
    bool counted = false;
    try {
        ++activeHandlerTasks_;
        counted = true;
        asio::co_spawn(executor_, ruvia::asAwaitable(dispatchOne(streamId)),
            asio::bind_allocator(
                asio::recycling_allocator<void>(), [this](std::exception_ptr exception) noexcept {
                    if (exception != nullptr) {
                        terminate(std::make_error_code(std::errc::operation_canceled));
                    }
                    --activeHandlerTasks_;
                    if (activeHandlerTasks_ == 0) {
                        handlerFinished_.notify();
                    }
                    writeSignal_.notify();
                }));
    } catch (...) {
        if (counted) {
            --activeHandlerTasks_;
        }
        removeStreamRuntime(streamId);
        return false;
    }
    return true;
}

void Http2SansIoSessionEngine::drainEvents() {
    const auto& options = session_.options();
    const auto resetEventStream = [&](std::uint32_t streamId, Http2ErrorCode error) {
        auto* signal = streamRuntimes_.signalFor(streamId);
        resetStreamNoThrow(streamId, error);
        if (signal != nullptr) {
            signal->wake();
        } else {
            removeStreamRuntime(streamId);
        }
        wakeWriter();
    };
    const auto resolveStreamRoute = [&](std::uint32_t streamId) {
        const auto requestView = connection_.server_request_view(streamId);
        if (!requestView.has_value()) {
            return static_cast<Http2SansIoStreamRuntime*>(nullptr);
        }
        return http2SelectStreamRoute(routes_, *requestView, streamRuntimes_, streamId);
    };
    const auto onMessageHead = [&](Http2RequestHeadEvent* messageHead) {
        const auto streamId = messageHead->streamId();
        ++acceptedRequestHeads_;
        if (!connection_.draining() && options.max_requests_per_connection.has_value() &&
            acceptedRequestHeads_ >= *options.max_requests_per_connection) {
            connection_.beginDrain();
            wakeWriter();
        }
        const auto expectationPlan =
            messageHead->expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
        const auto snapshot = messageHead->snapshot();
        auto* streamRuntime = resolveStreamRoute(streamId);
        if (streamRuntime == nullptr || !streamRuntime->holdRequestHead(std::move(*messageHead))) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
            return;
        }
        if (expectationPlan.sendContinue() != nullptr) {
            const auto status = connection_.submitInterimResponseHead(
                streamId, HttpInterimResponseHead(ruvia::http_status::kContinue));
            if (status == Http2SubmitStatus::kAccepted) {
                wakeWriter();
            } else {
                if (status != Http2SubmitStatus::kClosed) {
                    resetEventStream(streamId, Http2ErrorCode::kInternalError);
                } else {
                    removeStreamRuntime(streamId);
                }
                return;
            }
        }
        const bool connectRequest = snapshot.connectPending;
        const auto* selectedRoute = streamRuntime->selectedRoute();
        const bool streamingBody = !connectRequest && selectedRoute != nullptr &&
                                   selectedRoute->body().streaming() != nullptr &&
                                   snapshot.bodyOpen;
        if (expectationPlan.rejection() != nullptr || connectRequest || streamingBody) {
            if (!admitStream(streamId)) {
                resetEventStream(streamId, Http2ErrorCode::kInternalError);
            }
        }
    };
    const auto onBodyChunk = [&](auto* bodyChunk) {
        const auto streamId = bodyChunk->streamId();
        auto* streamRuntime = streamRuntimes_.find(streamId);
        if (streamRuntime == nullptr) {
            return;
        }
        auto* selectedRoute = streamRuntime->selectedRoute();
        if (selectedRoute == nullptr) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
            return;
        }
        auto& requestBody = selectedRoute->body();
        const auto* resolvedRoute = selectedRoute->resolution().resolved();
        const auto totalLimit = requestBodyByteLimit(requestBody.mode(), options.max_stream_body_bytes,
            options.max_buffered_body_bytes,
            resolvedRoute != nullptr ? resolvedRoute->route().maxRequestBodyBytes() : 0);
        auto stored = [&] {
            if (requestBody.streaming() != nullptr) {
                return requestBody.store(bodyChunk->bytes(), totalLimit,
                    options.max_buffered_body_bytes, std::move(bodyChunk->takeCredit()));
            }
            return requestBody.store(
                bodyChunk->bytes(), totalLimit, options.max_buffered_body_bytes);
        }();
        if (stored.stored() == nullptr) {
            const bool knownRejection =
                stored.protocolFailure() != nullptr || stored.backlogOverflow() != nullptr;
            resetEventStream(streamId,
                knownRejection ? Http2ErrorCode::kCancel : Http2ErrorCode::kInternalError);
            return;
        }
        if (requestBody.streaming() != nullptr) {
            auto* signal = streamRuntime->signal();
            if (signal == nullptr) {
                resetEventStream(streamId, Http2ErrorCode::kInternalError);
                return;
            }
            signal->wake();
        } else {
            (void)connection_.acknowledge(std::move(bodyChunk->takeCredit()));
            wakeWriter();
        }
    };
    const auto onTunnelData = [&](auto* tunnelData) {
        const auto streamId = tunnelData->streamId();
        auto* streamRuntime = streamRuntimes_.find(streamId);
        auto* signal = streamRuntime != nullptr ? streamRuntime->signal() : nullptr;
        if (streamRuntime == nullptr || signal == nullptr) {
            return;
        }
        auto* selectedRoute = streamRuntime->selectedRoute();
        auto* streamingBody =
            selectedRoute != nullptr ? selectedRoute->body().streaming() : nullptr;
        if (streamingBody == nullptr) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
            return;
        }
        if (!streamingBody->queue().enqueueBounded(tunnelData->bytes(),
                std::move(tunnelData->takeCredit()), options.max_buffered_body_bytes)) {
            resetEventStream(streamId, Http2ErrorCode::kCancel);
            return;
        }
        signal->wake();
        wakeWriter();
    };
    const auto onTunnelEnd = [&](const auto* tunnelEnd) {
        if (auto* signal = streamRuntimes_.signalFor(tunnelEnd->streamId())) {
            signal->wake();
        }
    };
    const auto onMessageEnd = [&](const auto* messageEnd) {
        const auto streamId = messageEnd->streamId();
        auto* streamRuntime = streamRuntimes_.find(streamId);
        if (streamRuntime == nullptr) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
            return;
        }
        for (const auto& field : messageEnd->trailers()) {
            if (!streamRuntime->trailers().append(field.name(), field.value())) {
                throw std::logic_error("HTTP/2 decoder published invalid request trailers");
            }
        }
        if (auto* signal = streamRuntime->signal()) {
            signal->wake();
        } else if (!admitStream(streamId)) {
            resetEventStream(streamId, Http2ErrorCode::kInternalError);
        }
    };
    const auto onStreamClosed = [&](const auto* streamClosed) {
        const auto streamId = streamClosed->streamId();
        streamRuntimes_.forEach([&](Http2SansIoStreamRuntime& child) {
            if (child.pushParent() == streamId) {
                resetStreamNoThrow(child.streamId(), Http2ErrorCode::kCancel);
                if (auto* childSignal = child.signal()) {
                    childSignal->wake();
                }
            }
        });
        outputBudget_.releaseAndReconcile(streamId, connection_);
        auto* streamRuntime = streamRuntimes_.find(streamId);
        auto* signal = streamRuntime != nullptr ? streamRuntime->signal() : nullptr;
        if (signal != nullptr) {
            signal->wake();
        } else {
            removeStreamRuntime(streamId);
        }
    };

    for (;;) {
        auto event = connection_.nextEvent();
        if (!event.has_value()) {
            break;
        }
        if (auto* requestHead = event->requestHead()) {
            onMessageHead(requestHead);
        } else if (auto* bodyChunk = event->messageBodyChunk()) {
            onBodyChunk(bodyChunk);
        } else if (auto* tunnelData = event->tunnelData()) {
            onTunnelData(tunnelData);
        } else if (const auto* tunnelEnd = event->tunnelEnd()) {
            onTunnelEnd(tunnelEnd);
        } else if (const auto* messageEnd = event->messageEnd()) {
            onMessageEnd(messageEnd);
        } else if (const auto* streamClosed = event->streamClosed()) {
            onStreamClosed(streamClosed);
        } else if (const auto* update = event->priorityUpdate(); update != nullptr && !update->push) {
            if (auto* runtime = streamRuntimes_.find(static_cast<std::uint32_t>(update->elementId))) {
                runtime->reprioritize(update->fields.requestPriority());
            }
        }
    }
    for (const auto streamId : connection_.takeDrainedDataStreams()) {
        if (auto* signal = streamRuntimes_.signalFor(streamId)) {
            signal->wake();
        }
    }
    outputBudget_.wake();
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
    while (lifecycle_.writer_join_pending()) {
        co_await writerFinished_.wait();
    }
    lifecycle_.rethrowWriterFailure();
}

}  // namespace ruvia::detail
