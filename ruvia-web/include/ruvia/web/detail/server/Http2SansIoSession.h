#pragma once

// Concurrent HTTP/2 server session over the sans-I/O core (ruvia-web).
//
// Drives an Http2Connection with a reader coroutine + a single writer coroutine so that
// HTTP/2 streams are genuinely multiplexed: each admitted request is dispatched on its
// own concurrent handler task, and a slow handler never blocks reading or the other
// streams. All outbound bytes funnel through the one writer (submit* only appends to the
// connection's buffer, so ordering is automatic); the writer sleeps on a signal timer
// when idle and is woken whenever new output is produced.
//
// Framework parity with the coroutine Http2ServerSession (which this replaces): route
// policy resolved at kMessageHead (mirroring resolveStreamRoute), rate limiting,
// request-body size limits, streaming request bodies (BodyReader over the Web-owned
// per-stream body queue), WebSocket tunnels (RFC 8441), streaming + buffered + file-body
// responses with compression/CORS via prepareBufferedHttpResponse, access logging,
// client certificates, connection-scanner inactivity phases, graceful drain on server
// shutdown, TLS ALPN, and cleartext prior-knowledge startup.
//
// Lifetime safety: a request/response holds VIEWS into its stream's decoded storage, so
// before spawning a handler the stream is pinned (see Http2Connection::pinStream) -- a
// peer RST then keeps the stream alive+reset rather than freeing it, and the handler
// checks isAborted() before submitting. The handler unpins on completion, freeing it.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/ip/tcp.hpp>
#include "ruvia/core/detail/WorkerSignal.h"

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/web/detail/server/HttpResponseWriter.h"
#include "ruvia/web/detail/server/RequestMemoryArena.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/Http2RequestBuilder.h"
#include "ruvia/web/detail/http/HttpProtocolErrorInfo.h"
#include "ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/http2/Http2SansIoWsTransport.h"
#include "ruvia/http/detail/http2/Http2WebSocketHandshake.h"
#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/Http2BufferedResponseWrite.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/web/detail/server/HttpResponseStreamDispatch.h"
#include "ruvia/web/detail/server/HttpServerAccessLog.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/web/detail/server/RateLimitDecision.h"
#include "ruvia/web/detail/server/RequestBodyLimit.h"
#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/HttpServerWorkerState.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace ruvia::detail {

// Complete, non-null connection wiring captured by value in the session coroutine.
// Optional product integrations remain explicit inside ContextServices, while
// options, scanner ownership, and graceful-shutdown state are mandatory references.
class Http2SansIoSessionContext final {
public:
    Http2SansIoSessionContext(
        ContextServices services,
        const HttpServerOptions& options,
        ConnectionScanner::Entry& scannerEntry,
        const HttpServerWorkerState& workerState) noexcept
        : services_(services),
          options_(&options),
          scannerEntry_(&scannerEntry),
          workerState_(&workerState) {}

    [[nodiscard]] const HttpServerOptions& options() const noexcept {
        return *options_;
    }

    [[nodiscard]] ConnectionScanner::Entry& scannerEntry() const noexcept {
        return *scannerEntry_;
    }

    [[nodiscard]] bool workerRunning() const noexcept {
        return httpServerWorkerRunning(*workerState_);
    }

    [[nodiscard]] const ContextServices& services() const noexcept {
        return services_;
    }

private:
    ContextServices services_;
    const HttpServerOptions* options_;
    ConnectionScanner::Entry* scannerEntry_;
    const HttpServerWorkerState* workerState_;
};

[[nodiscard]] inline ConnectionScanner::Phase http2SansIoInactivityPhase(
    bool headerBlockInProgress,
    std::size_t activeRuntimeCount) noexcept {
    if (headerBlockInProgress) {
        return ConnectionScanner::Phase::kReadingInitial;
    }
    return activeRuntimeCount == 0
        ? ConnectionScanner::Phase::kIdle
        : ConnectionScanner::Phase::kReadingPayload;
}

template <typename Stream>
Task<void> runHttp2SansIoSession(
    Stream& stream,
    const RouteTable& routes,
    WorkerMemory& worker,
    Http2SansIoSessionContext session,
    std::string_view initialBytes = {}) {
    auto executor = stream.get_executor();
    const auto& options = session.options();
    auto& scannerEntry = session.scannerEntry();
    const auto& baseServices = session.services();
    const auto remoteAddress = baseServices.connInfo().remote().address();

    Http2Connection connection(worker.resource(), Http2Role::kServer);

    WorkerSignal writeSignal(baseServices.worker(), executor);
    bool stopping = false;  // reader has finished (EOF/error/connection error)
    bool writeFailed = false;
    bool writerDone = false;  // writer coroutine has fully exited (level-triggered join)
    bool initialInputRetained = false;
    // keepaliveRequests parity with h1's Http1RequestSequence: after this many
    // request heads the connection drains (GOAWAY NO_ERROR) instead of serving
    // new streams forever.
    std::size_t acceptedRequestHeads = 0;

    const auto wakeWriter = [&writeSignal]() noexcept {
        writeSignal.notify();
    };

    // One stable Web runtime owns every per-stream application concern: route
    // resolution, request-body storage, the dispatch signal, and the dispatch lease.
    // The table's dispatched count is established synchronously at admission, before
    // co_spawn can schedule the handler, and is the writer/join source of truth.
    Http2SansIoStreamRuntimeTable streamRuntimes(worker.resource());
    const auto findStreamRuntime = [&streamRuntimes](
        std::uint32_t streamId) noexcept {
        return streamRuntimes.find(streamId);
    };
    const auto eraseStreamRuntime = [&streamRuntimes](
        std::uint32_t streamId) {
        (void)streamRuntimes.remove(streamId);
    };
    const auto findSignal = [&streamRuntimes](
        std::uint32_t streamId) noexcept -> Http2SansIoStreamSignal* {
        auto* runtime = streamRuntimes.find(streamId);
        return runtime != nullptr ? runtime->signal() : nullptr;
    };
    Http2BufferedResponseWriter bufferedResponseWriter(
        connection,
        streamRuntimes,
        worker,
        writeSignal);

    // Single writer: serialize all outbound writes; sleep on writeSignal when idle.
    // The pending bytes are MOVED out before each write (takeOutput): concurrent
    // handlers keep submitting while async_write is in flight, and an append that
    // reallocated the core's buffer would dangle a pendingOutput() view mid-write.
    //
    // CRITICAL: the writer's ONLY exit is
    // `stopping && dispatchedCount == 0`. A write error
    // sets writeFailed but keeps looping (draining and discarding output) until every
    // admitted handler has finished, because the session's teardown join treats the
    // writer's exit as the handler join too -- exiting while dispatchedCount is
    // nonzero would
    // let the session destroy this frame's state under a still-suspended detached
    // handler (use-after-free). Once writeFailed, no bytes actually reach the socket.
    auto writerLoop = [&]() -> Task<void> {
        std::pmr::string writeScratch(worker.resource());
        for (;;) {
            while (connection.wantsWrite()) {
                connection.takeOutput(writeScratch);  // always drain so the buffer can't grow
                if (writeFailed) {
                    continue;  // socket is dead; discard, but keep serving handlers
                }
                // Optimistic synchronous send, same rationale as the HTTP/1
                // writer: the socket is already non-blocking (the session is
                // reached through prior async reads), so a full write_some
                // skips the reactor completion pass entirely.
                std::error_code writeEc;
                std::size_t writtenBytes = 0;
                bool writeDone = false;
                if constexpr (std::is_same_v<std::remove_cvref_t<Stream>, asio::ip::tcp::socket>) {
                    writeDone = tryPlainTcpSyncWrite(
                        stream,
                        asio::buffer(writeScratch.data(), writeScratch.size()),
                        writeScratch.size(),
                        writeEc,
                        writtenBytes);
                }
                if (!writeDone) {
                    const auto writeCompletion = co_await asyncAsio(
                        [&stream, &writeScratch, writtenBytes](auto handler) mutable {
                            asio::async_write(
                                stream,
                                asio::buffer(
                                    writeScratch.data() + writtenBytes,
                                    writeScratch.size() - writtenBytes),
                                std::move(handler));
                        });
                    writeEc = writeCompletion.errorCode();
                }
                if (writeEc) {
                    writeFailed = true;
                    continue;
                }
                scannerEntry.touch();
                // Release a peak-sized batch buffer so it (and, via takeOutput's swap,
                // the core's outBuffer_) does not pin the connection's high-water
                // capacity for its whole lifetime (the h1 work-set trims the same way).
                clearPmrStringRetainingSmall(writeScratch, 64 * 1024);
            }
            if (stopping && streamRuntimes.dispatchedCount() == 0) {
                co_return;  // nothing left to write and no more will be produced
            }
            co_await writeSignal.wait();
        }
    };

    // Handler body for one admitted stream; a 1:1 port of the coroutine
    // dispatchStream. Early co_returns are safe -- dispatchOne (below) owns cleanup.
    auto dispatchOneInner = [&](std::uint32_t streamId) -> Task<void> {
        const auto requestStart = std::chrono::steady_clock::now();
        // Seed the request arena from a block carried on THIS coroutine frame (already
        // heap-allocated by co_spawn), so the common request does zero extra heap work
        // -- restoring the coroutine session's zero-alloc invariant that the retrofit
        // lost by constructing RequestMemory(worker) (a separate >=4KB alloc/free pair).
        std::array<std::byte, kRequestArenaStackBytes> arenaBlock;
        std::optional<RequestMemory> requestMemoryStorage;
        RequestMemory& requestMemory = emplaceRequestMemory(
            requestMemoryStorage, worker,
            std::span<std::byte>(arenaBlock.data(), arenaBlock.size()));
        auto* streamState = connection.stream(streamId);
        if (streamState == nullptr) {
            co_return;
        }
        auto* streamRuntime = findStreamRuntime(streamId);
        if (streamRuntime == nullptr) {
            (void)connection.submitReset(
                streamId,
                Http2ErrorCode::kInternalError);
            wakeWriter();
            co_return;
        }
        auto* selectedRoute = streamRuntime->selectedRoute();
        if (selectedRoute == nullptr) {
            (void)connection.submitReset(
                streamId,
                Http2ErrorCode::kInternalError);
            wakeWriter();
            co_return;
        }
        auto& requestBody = selectedRoute->body();
        auto* streamingBody = requestBody.streaming();
        const auto* bufferedBody = requestBody.buffered();
        auto* streamSignal = streamRuntime->signal();
        if (streamSignal == nullptr) {
            (void)connection.submitReset(
                streamId,
                Http2ErrorCode::kInternalError);
            wakeWriter();
            co_return;
        }
        HttpRequest request = HttpRequestAccess::make();
        const auto requestBuild = Http2RequestBuilder::build(
                *streamState,
                request,
                requestMemory.resource(),
                bufferedBody == nullptr
                    ? std::string_view{}
                    : bufferedBody->bytes());
        if (const auto* failure = requestBuild.failure()) {
            auto response = co_await routes.handleError(
                request, requestMemory,
                copyHttpProtocolErrorInfo(
                    requestMemory.resource(),
                    failure->protocolError()),
                baseServices);
            (void)co_await bufferedResponseWriter.write(
                streamId,
                response,
                httpBufferedResponseWritePlan(
                    streamState->requestKnownMethod(),
                    response));
            co_return;
        }
        HttpResponse response(requestMemory.resource());
        // Select an early policy response or dispatch the route in this coroutine,
        // then converge on one buffered preparation/send tail. The single-pass block
        // avoids another request-path coroutine frame solely for common completion.
        do {
            const auto expectationPlan = streamState->expectationPlan(
                HttpUnsupportedExpectationPolicy::kReject);
            if (const auto* rejection = expectationPlan.rejection()) {
                // Expect is extensible, so the HTTP/2 decoder accepted and preserved
                // the field. This Web product supports only 100-continue and applies
                // its 417 policy through the normal application error handler.
                response = co_await routes.handleError(
                    request, requestMemory,
                    copyHttpProtocolErrorInfo(
                        requestMemory.resource(),
                        rejection->protocolError()),
                    baseServices);
                break;
            }
            const auto& resolution = selectedRoute->resolution();
            const auto* resolved = resolution.resolved();

            const auto appRateLimit = rateLimitRequestAllowed(
                baseServices.rateLimiter(), remoteAddress);
            if (!appRateLimit.allowed) {
                response = co_await routes.handleError(
                    request, requestMemory,
                    HttpErrorInfo(429, {}, "rate limit exceeded"),
                    baseServices);
                setRetryAfterSeconds(
                    response,
                    std::chrono::milliseconds(appRateLimit.resetAfterMs));
                break;
            }
            std::optional<Http2SansIoRequestBodyReader> streamReaderStorage;
            std::optional<BodyReader> bodyReaderStorage;
            if (streamingBody != nullptr &&
                streamState->tunnel().pending() == nullptr) {
                streamReaderStorage.emplace(
                    connection,
                    streamId,
                    streamingBody->queue(),
                    *streamSignal,
                    writeSignal);
                emplaceBodyReaderFacade(bodyReaderStorage, *streamReaderStorage);
            }
            auto dispatchServices = baseServices;
            if (bodyReaderStorage) {
                dispatchServices = dispatchServices.withStreamingRequestBody(
                    *bodyReaderStorage);
            }

            const auto* webSocketEndpoint = resolved == nullptr
                ? nullptr
                : resolved->route().endpoint().webSocket();
            const auto* responseStreamEndpoint = resolved == nullptr
                ? nullptr
                : resolved->route().endpoint().responseStream();
            if (webSocketEndpoint != nullptr) {
                const auto handshakeValidation =
                    validateHttp2WebSocketHandshake(*streamState, request);
                if (handshakeValidation.accepted() != nullptr) {
                    if (streamingBody == nullptr) {
                        (void)connection.submitReset(
                            streamId, Http2ErrorCode::kInternalError);
                        wakeWriter();
                        co_return;
                    }
                    using WsTransport =
                        Http2SansIoWsTransport<decltype(executor)>;
                    using WsConnection = WebSocketConnection<WsTransport>;
                    std::optional<WsConnection> webSocketConnection;
                    auto upgradeAndRun = [&](Context& context) -> Task<void> {
                        // HTTP/1 and RFC 8441 consume the same immutable
                        // negotiation only after middleware reaches the handler.
                        const auto negotiation = makeWebSocketServerNegotiation(
                            request,
                            webSocketEndpoint->subprotocols());
                        const auto handshakeResult =
                            connection.submitWebSocketHandshake(
                                streamId,
                                negotiation);
                        const auto* submittedHandshake =
                            handshakeResult.submitted();
                        if (submittedHandshake == nullptr) {
                            co_return;
                        }
                        wakeWriter();
                        // Each Extended-CONNECT tunnel registers its OWN
                        // heartbeat slot on the connection scanner entry.
                        webSocketConnection.emplace(
                            WsTransport(
                                connection,
                                streamId,
                                streamingBody->queue(),
                                *streamSignal,
                                writeSignal,
                                executor),
                            scannerEntry,
                            webSocketEndpoint->lifecycle(),
                            ProtocolByteLimit::limited(
                                options.maxWebSocketMessageBytes),
                            requestMemory.resource(),
                            /*initialBytes=*/std::string_view{},
                            submittedHandshake->deflate());
                        co_await invokeWebSocketHandler(
                            *webSocketConnection,
                            scannerEntry,
                            webSocketEndpoint->handler(),
                            context);
                    };
                    const auto terminal =
                        makeCallableRef<void, Context&>(upgradeAndRun);
                    std::optional<HttpResponse> buffered;
                    std::exception_ptr exception;
                    try {
                        buffered = co_await routes.dispatchWebSocket(
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
                            exception);
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
                response = co_await routes.handleError(
                    request,
                    requestMemory,
                    copyHttpProtocolErrorInfo(
                        requestMemory.resource(),
                        failure->protocolError()),
                    baseServices);
                failure->applyRequiredResponseHeaders(response);
            } else if (responseStreamEndpoint != nullptr) {
                // Streaming route (for example SSE): drive the shared streaming
                // dispatch through a sans-I/O sink that submits chunks via the core.
                Http2SansIoResponseStreamSink<decltype(executor)> sink(
                    connection,
                    streamId,
                    responseStreamEndpoint->kind(),
                    requestMemory.resource(),
                    executor,
                    baseServices.worker() == nullptr
                        ? WorkerHandle{}
                        : *baseServices.worker(),
                    writeSignal,
                    *streamSignal);
                auto result = co_await dispatchResponseStreamWith(
                    sink,
                    routes,
                    request,
                    *resolved,
                    requestMemory,
                    dispatchServices,
                    [&connection, streamId]() noexcept {
                        auto* s = connection.stream(streamId);
                        return s == nullptr || s->isAborted();
                    });
                if (result.peerAbortedBeforeCommit() != nullptr) {
                    // No final response head existed, so there is no HTTP status to
                    // report through the response-completion access-log callback.
                    co_return;
                }
                if (const auto committedStatus = result.committedStatus()) {
                    if (result.failedAfterCommit() != nullptr) {
                        (void)connection.submitReset(
                            streamId,
                            Http2ErrorCode::kInternalError);
                        wakeWriter();
                    }
                    recordHttpAccess(
                        options.accessLog,
                        request,
                        remoteAddress,
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
                response = co_await routes.dispatchBuffered(
                    request, resolution, requestMemory, dispatchServices);
            }
        } while (false);

        // All valid buffered branches converge here. Preparation moves any
        // compressed representation into the response itself, and the exact
        // post-transformation plan is submitted without re-planning.
        const auto writePlan = prepareBufferedHttpResponse(
            request,
            response,
            options);
        const auto result = co_await bufferedResponseWriter.write(
            streamId,
            response,
            writePlan);
        if (const auto committedStatus = result.committedStatus()) {
            recordHttpAccess(
                options.accessLog,
                request,
                remoteAddress,
                *committedStatus,
                requestStart);
        }
        co_return;
    };

    // One concurrent handler: admission already owns the table's dispatch lease.
    // Run the dispatch body, then removing the runtime releases route/body/signal and
    // the lease atomically before the protocol pin is released.
    auto dispatchOne = [&](std::uint32_t streamId) -> Task<void> {
        try {
            co_await dispatchOneInner(streamId);
        } catch (...) {
            // Last-resort: a dispatch failure must not leak the pin/dispatch lease.
            auto* live = connection.stream(streamId);
            if (live != nullptr && !live->isAborted()) {
                (void)connection.submitReset(streamId, Http2ErrorCode::kInternalError);
            }
        }
        eraseStreamRuntime(streamId);
        connection.unpinStream(streamId);
        wakeWriter();
        co_return;
    };

    // Owner-side route policy (1:1 port of the coroutine resolveStreamRoute), run at
    // kMessageHead so body-mode/tunnel decisions land BEFORE the next feed.
    const auto resolveStreamRoute = [&routes, &streamRuntimes](
        Http2StreamState& streamState) -> Http2SansIoStreamRuntime* {
        const auto method = Http2RequestBuilder::routeMethod(streamState);
        const auto path = Http2RequestBuilder::requestPath(streamState);
        auto& runtime = streamRuntimes.ensureAccepted(streamState);
        RouteResolution resolution;
        auto bodyMode = RequestBodyMode::kBuffered;
        if (method != HttpKnownMethod::kUnknown && !path.empty()) {
            resolution = routes.resolve(method, path);
        }
        const auto* resolved = resolution.resolved();
        if (resolved != nullptr) {
            bodyMode = resolved->route().endpoint().requestBodyMode();
        }
        if (http2IsPendingWebSocketConnect(streamState) &&
            resolved != nullptr &&
            resolved->route().endpoint().webSocket() != nullptr) {
            bodyMode = RequestBodyMode::kStream;
        }
        return runtime.selectRoute(std::move(resolution), bodyMode)
            ? &runtime
            : nullptr;
    };

    // Admit a stream for dispatch: EVERY dispatched stream gets a signal, so response
    // send-window pacing (awaitSendWindow / the streaming sink) can be woken by
    // takeDrainedDataStreams no matter the body kind -- a plain route returning a large
    // file or a stream body blocks on the send window exactly like a streaming route,
    // and without a signal its blocked submit could never be woken (truncation + hang).
    const auto admitStream = [&](std::uint32_t streamId) {
        auto* signal = baseServices.worker() != nullptr
            ? streamRuntimes.beginDispatch(streamId, *baseServices.worker())
            : streamRuntimes.beginDispatch(streamId, executor);
        if (signal == nullptr) {
            return false;
        }
        connection.pinStream(streamId);
        // Recycle the per-stream handler's coroutine frame (mirrors the h1 accept path);
        // under load the common request then does no extra heap work for the frame.
        asio::co_spawn(
            executor, taskAsAwaitable(dispatchOne(streamId)),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
        return true;
    };

    // Drain the core's event queue, admitting streams and routing body bytes.
    const auto drainEvents = [&]() {
        std::array<std::uint32_t,
                   Http2LocalSettings::kMaxConcurrentStreams>
            copiedBodyStreams{};
        std::size_t copiedBodyStreamCount = 0;
        const auto markBufferedBodyCopied = [&](std::uint32_t streamId) {
            const auto end = copiedBodyStreams.begin() +
                static_cast<std::ptrdiff_t>(copiedBodyStreamCount);
            if (std::find(copiedBodyStreams.begin(), end, streamId) == end) {
                if (copiedBodyStreamCount == copiedBodyStreams.size()) {
                    return false;
                }
                copiedBodyStreams[copiedBodyStreamCount++] = streamId;
            }
            return true;
        };
        const auto unmarkBufferedBodyCopied = [&](std::uint32_t streamId) {
            const auto end = copiedBodyStreams.begin() +
                static_cast<std::ptrdiff_t>(copiedBodyStreamCount);
            const auto found = std::find(
                copiedBodyStreams.begin(), end, streamId);
            if (found == end) {
                return;
            }
            --copiedBodyStreamCount;
            *found = copiedBodyStreams[copiedBodyStreamCount];
        };
        const auto resetEventStream = [&](std::uint32_t streamId,
                                          Http2ErrorCode error) {
            unmarkBufferedBodyCopied(streamId);
            auto* signal = findSignal(streamId);
            (void)connection.submitReset(streamId, error);
            if (signal != nullptr) {
                // A dispatched handler owns the runtime until dispatchOne finishes.
                signal->wake();
            } else {
                // Owner-side reset is intentionally not echoed as kStreamClosed.
                // An undispatched runtime therefore has to be reclaimed here.
                eraseStreamRuntime(streamId);
            }
            wakeWriter();
        };
        for (;;) {
            const auto event = connection.nextEvent();
            if (!event.has_value()) {
                break;
            }
            if (const auto* messageHead = event->messageHead()) {
                const auto streamId = messageHead->streamId();
                ++acceptedRequestHeads;
                if (!connection.draining() &&
                    options.keepaliveRequests.has_value() &&
                    acceptedRequestHeads >= *options.keepaliveRequests) {
                    // This request still runs; the GOAWAY covers every stream
                    // at or below it, so the peer reopens on a new connection.
                    connection.beginDrain();
                    wakeWriter();
                }
                auto* streamState = connection.stream(streamId);
                if (streamState == nullptr) {
                    continue;
                }
                auto* streamRuntime = resolveStreamRoute(*streamState);
                if (streamRuntime == nullptr) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                    continue;
                }
                const auto expectationPlan = streamState->expectationPlan(
                    HttpUnsupportedExpectationPolicy::kReject);
                if (expectationPlan.send100Continue() != nullptr) {
                    const auto status = connection.submitInterimResponseHead(
                        streamId,
                        HttpInterimResponseHead(100));
                    if (status == Http2SubmitStatus::kAccepted) {
                        wakeWriter();
                    } else {
                        if (status != Http2SubmitStatus::kClosed) {
                            resetEventStream(
                                streamId, Http2ErrorCode::kInternalError);
                        } else {
                            eraseStreamRuntime(streamId);
                        }
                        continue;
                    }
                }
                const bool connectRequest =
                    streamState->tunnel().pending() != nullptr;
                const auto* selectedRoute = streamRuntime->selectedRoute();
                const bool streamingBody = !connectRequest &&
                    selectedRoute != nullptr &&
                    selectedRoute->body().streaming() != nullptr &&
                    streamState->remoteReceive().contentOpen() != nullptr;
                if (expectationPlan.rejection() != nullptr ||
                    connectRequest || streamingBody) {
                    // Dispatch NOW; body bytes stream through the Web runtime's queue
                    // while the handler runs. Unsupported expectations also need an
                    // immediate 417: waiting for buffered content would deadlock a
                    // conforming client that is waiting for the expectation decision.
                    if (!admitStream(streamId)) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                    }
                }
            } else if (const auto* bodyChunk = event->messageBodyChunk()) {
                const auto streamId = bodyChunk->streamId();
                auto* streamState = connection.stream(streamId);
                auto* streamRuntime = findStreamRuntime(streamId);
                if (streamState == nullptr || streamRuntime == nullptr) {
                    if (streamState != nullptr) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                    }
                    continue;
                }
                auto* selectedRoute = streamRuntime->selectedRoute();
                if (selectedRoute == nullptr) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                    continue;
                }
                auto& requestBody = selectedRoute->body();
                const auto totalLimit = requestBodyByteLimit(
                    requestBody.mode(),
                    options.maxStreamBodyBytes,
                    options.maxBufferedBodyBytes);
                const auto stored = requestBody.store(
                    bodyChunk->bytes(),
                    totalLimit,
                    options.maxBufferedBodyBytes);
                if (stored.stored() == nullptr) {
                    const bool knownRejection =
                        stored.protocolFailure() != nullptr ||
                        stored.backlogOverflow() != nullptr;
                    resetEventStream(
                        streamId,
                        knownRejection
                            ? Http2ErrorCode::kCancel
                            : Http2ErrorCode::kInternalError);
                    continue;
                }
                if (requestBody.streaming() != nullptr) {
                    auto* signal = streamRuntime->signal();
                    if (signal == nullptr) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                        continue;
                    }
                    signal->wake();
                } else {
                    // Delay acknowledgement until the complete event batch has been
                    // copied. releaseReceivedData() returns all debt currently held
                    // by the stream, including later DATA frames from this feed.
                    if (!markBufferedBodyCopied(streamId)) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                    }
                }
            } else if (const auto* tunnelData = event->tunnelData()) {
                const auto streamId = tunnelData->streamId();
                auto* streamState = connection.stream(streamId);
                auto* streamRuntime = findStreamRuntime(streamId);
                auto* signal = streamRuntime != nullptr
                    ? streamRuntime->signal()
                    : nullptr;
                if (streamState == nullptr || streamRuntime == nullptr ||
                    signal == nullptr) {
                    if (streamState != nullptr) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                    }
                    continue;
                }
                auto* selectedRoute = streamRuntime->selectedRoute();
                auto* streamingBody = selectedRoute != nullptr
                    ? selectedRoute->body().streaming()
                    : nullptr;
                if (streamingBody == nullptr) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                    continue;
                }
                streamingBody->queue().enqueue(tunnelData->bytes());
                signal->wake();
            } else if (const auto* tunnelEnd = event->tunnelEnd()) {
                if (auto* signal = findSignal(tunnelEnd->streamId())) {
                    signal->wake();
                }
            } else if (const auto* messageEnd = event->messageEnd()) {
                const auto streamId = messageEnd->streamId();
                if (connection.stream(streamId) == nullptr) {
                    continue;
                }
                auto* streamRuntime = findStreamRuntime(streamId);
                if (streamRuntime == nullptr) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                    continue;
                }
                if (auto* signal = streamRuntime->signal()) {
                    signal->wake();  // remote END_STREAM was committed before this event
                } else if (!admitStream(streamId)) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                }
            } else if (const auto* streamClosed = event->streamClosed()) {
                const auto streamId = streamClosed->streamId();
                unmarkBufferedBodyCopied(streamId);
                auto* streamRuntime = findStreamRuntime(streamId);
                auto* signal = streamRuntime != nullptr
                    ? streamRuntime->signal()
                    : nullptr;
                if (signal != nullptr) {
                    signal->wake();  // stream is reset; blocked readers/writers see it
                } else {
                    // The protocol core can erase an unpinned reset stream before
                    // this event is drained. Cleanup is keyed by the typed event,
                    // not by a second lookup of already-removed core state.
                    eraseStreamRuntime(streamId);
                }
            }
        }
        for (std::size_t i = 0; i < copiedBodyStreamCount; ++i) {
            connection.releaseReceivedData(copiedBodyStreams[i]);
        }
        if (copiedBodyStreamCount != 0) {
            wakeWriter();
        }
        // Send-window reopenings drained deferred bodies inside the core; wake the
        // paced response writers so they pull their next chunk.
        for (const auto streamId : connection.takeDrainedDataStreams()) {
            if (auto* signal = findSignal(streamId)) {
                signal->wake();
            }
        }
    };

    // One ownership path for every inbound span. The core can refuse a span while an
    // earlier zero-copy event is pending; drain that event queue, then retry the exact
    // same bytes. The enum is the entire ownership result: no byte-count side channel
    // exists because the core never partially accepts a span.
    const auto feedAndDrain = [&](std::string_view bytes) {
        for (;;) {
            const auto result = connection.feed(bytes);
            drainEvents();
            if (result != Http2FeedResult::kEventsPending) {
                return result;
            }
        }
    };

    // --- startup ----------------------------------------------------------------
    connection.beginConnection();

    // Spawn the writer; it runs concurrently with the reader below. The join below is
    // LEVEL-triggered on writerDone, not on signal notification alone: notification
    // before a waiter exists must still make a late join return immediately, so if the writer finishes before the
    // reader reaches the join (common on an abrupt peer RST, where the write error
    // surfaces first), a cancel-only latch would be a no-op and the join would sleep
    // until time_point::max() forever. The bool makes a late join return immediately.
    WorkerSignal writerFinished(baseServices.worker(), executor);
    asio::co_spawn(
        executor, taskAsAwaitable(writerLoop()),
        [&writerFinished, &writerDone](std::exception_ptr) noexcept {
            writerDone = true;
            writerFinished.notify();
        });

    // Establish the feed contract: drain any startup events before initial input.
    drainEvents();
    if (!connection.connectionError().has_value() && !initialBytes.empty()) {
        const auto result = feedAndDrain(initialBytes);
        initialInputRetained =
            result == Http2FeedResult::kConnectionNotStarted;
    }
    wakeWriter();

    // Reader loop: feed inbound bytes, then act on the drained events.
    if (!connection.connectionError().has_value() && !initialInputRetained) {
        // 4 KB read scratch. Requests are the small direction of HTTP/2
        // traffic (responses never pass through here), so a max-size 16 KB
        // frame arriving in several reads is the rare case, while the buffer
        // is resident in every connection's coroutine frame for the whole
        // connection.
        std::array<char, 4096> readBuffer;
        for (;;) {
            // Pick the inactivity phase: mid-header-block -> the tight header timeout;
            // no active Web runtime (including a pre-dispatch buffered request body)
            // -> kIdle so a keep-alive connection between requests is governed by the
            // keepalive timeout, NOT client_body_timeout; otherwise a body/response is
            // in progress -> kReadingPayload. (WS tunnels carry their own heartbeat.)
            scannerEntry.setPhase(http2SansIoInactivityPhase(
                connection.headerBlockInProgress(),
                streamRuntimes.size()));
            auto readCompletion = co_await asyncAsio<std::size_t>(
                [&stream, &readBuffer](auto handler) mutable {
                    stream.async_read_some(
                        asio::buffer(readBuffer.data(), readBuffer.size()), std::move(handler));
                });
            const auto ec = readCompletion.errorCode();
            const auto bytesRead = readCompletion.result();
            if (ec || bytesRead == 0) {
                break;
            }
            scannerEntry.touch();
            // The server has begun draining: tell the peer to stop opening streams
            // (RFC 9113 §6.8); streams already started keep running.
            if (!connection.draining() &&
                !session.workerRunning()) {
                connection.beginDrain();
            }
            const auto result = feedAndDrain(
                std::string_view(readBuffer.data(), bytesRead));
            wakeWriter();  // feed may have produced ACKs / WINDOW_UPDATEs to flush
            if (result == Http2FeedResult::kConnectionNotStarted ||
                result == Http2FeedResult::kProtocolFailure || writeFailed) {
                break;  // retained input, terminal protocol failure, or dead write side
            }
        }
    }

    // Reader done: every stream still waiting on inbound bytes or window space must
    // observe EOF now, or its handler (and thus the writer join below) waits forever.
    streamRuntimes.forEach([](Http2SansIoStreamRuntime& runtime) {
        if (auto* signal = runtime.signal()) {
            signal->end();
        }
    });

    // Let the writer drain the last output once every handler has finished, then join
    // it (handlers wake the writer as they complete, so joining the writer waits for
    // them too). Level-triggered: skip the wait entirely if the writer already exited.
    stopping = true;
    wakeWriter();
    while (!writerDone) {
        co_await writerFinished.wait();
    }
    co_return;
}

}  // namespace ruvia::detail
