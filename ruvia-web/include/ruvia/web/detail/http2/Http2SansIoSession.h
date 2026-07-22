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
// client certificates, connection-scanner inactivity phases, immediate server
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
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/ip/tcp.hpp>
#include "ruvia/core/detail/worker/WorkerSignal.h"

#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/server/response/HttpResponseWriter.h"
#include "ruvia/web/detail/server/request/RequestMemoryArena.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/message/Http2RequestBuilder.h"
#include "ruvia/web/detail/http/error/HttpProtocolErrorInfo.h"
#include "ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
#include "ruvia/web/detail/http2/Http2SansIoRouteSelection.h"
#include "ruvia/web/detail/http2/Http2SansIoSessionContext.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/http2/Http2SansIoTermination.h"
#include "ruvia/web/detail/http2/Http2SansIoWsTransport.h"
#include "ruvia/http/detail/http2/message/Http2WebSocketHandshake.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/web/detail/server/response/HttpBufferedResponse.h"
#include "ruvia/web/detail/http2/Http2BufferedResponseWrite.h"
#include "ruvia/web/detail/http2/Http2SansIoSessionLifecycle.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/web/detail/server/stream/HttpResponseStreamDispatch.h"
#include "ruvia/web/detail/server/HttpServerAccessLog.h"
#include "ruvia/web/detail/ratelimit/RateLimitDecision.h"
#include "ruvia/web/detail/server/request/RequestBodyLimit.h"
#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/util/PmrString.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace ruvia::detail {

// Complete, non-null connection wiring captured by value in the session coroutine.
// Optional product integrations remain explicit inside ContextServices, while
// options, scanner ownership, and shutdown state are mandatory references.

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

    WorkerSignal writeSignal(baseServices.worker());
    WorkerSignal handlerFinished(baseServices.worker());
    Http2SansIoSessionLifecycle lifecycle;
    Http2SansIoTermination termination;
    std::size_t activeHandlerTasks = 0;
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
    // The table's dispatch lease is established synchronously at admission. The
    // separate task count below is released only by co_spawn completion, after the
    // handler awaitable and its coroutine frame are actually finished.
    Http2SansIoStreamRuntimeTable streamRuntimes(
        worker.resource(), termination);
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
    const auto terminateSession = [&](std::error_code error) noexcept {
        if (!termination.terminate(error)) {
            return;
        }
        std::error_code ignored;
        stream.lowest_layer().cancel(ignored);
        streamRuntimes.forEach([](Http2SansIoStreamRuntime& runtime) {
            if (auto* signal = runtime.signal()) {
                signal->wake();
            }
        });
        writeSignal.notify();
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
    // The writer exits only after stopping begins and every admitted handler's
    // co_spawn completion has run. A write error terminates the shared transport,
    // then the loop keeps draining/discarding output while handlers unwind.
    auto writerLoop = [&]() -> Task<void> {
        std::pmr::string writeScratch(worker.resource());
        for (;;) {
            while (connection.wantsWrite()) {
                connection.takeOutput(writeScratch);  // always drain so the buffer can't grow
                if (lifecycle.writeFailed()) {
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
                    lifecycle.markWriteFailed();
                    terminateSession(writeEc);
                    continue;
                }
                scannerEntry.touch();
                // Release a peak-sized batch buffer so it (and, via takeOutput's swap,
                // the core's outBuffer_) does not pin the connection's high-water
                // capacity for its whole lifetime (the h1 work-set trims the same way).
                clearPmrStringRetainingSmall(writeScratch, 64 * 1024);
            }
            if (lifecycle.stopping() && activeHandlerTasks == 0) {
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
            std::span<std::byte>(arenaBlock));
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

            const auto appRateLimit = decideRequestRateLimit(
                baseServices.rateLimiter(), remoteAddress);
            if (const auto* rejection = appRateLimit.rejection()) {
                response = co_await routes.handleError(
                    request, requestMemory,
                    rateLimitRejectionError(),
                    baseServices);
                applyRateLimitRejectionHeaders(response, *rejection);
                break;
            }
            std::optional<BodyReaderBinding<Http2SansIoRequestBodyReader>>
                bodyReaderStorage;
            if (streamingBody != nullptr &&
                streamState->tunnel().pending() == nullptr) {
                bodyReaderStorage.emplace(
                    connection,
                    streamId,
                    streamingBody->queue(),
                    *streamSignal,
                    writeSignal);
            }
            auto dispatchServices = baseServices;
            if (bodyReaderStorage) {
                dispatchServices = dispatchServices.withStreamingRequestBody(
                    bodyReaderStorage->facade());
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
                        auto negotiation = makeWebSocketServerNegotiation(
                            request,
                            webSocketEndpoint->subprotocols(),
                            requestMemory.resource());
                        const auto handshakeResult =
                            connection.submitWebSocketHandshake(
                                streamId,
                                std::move(negotiation));
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
                            baseServices.worker(),
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
                Http2SansIoResponseStreamSink sink(
                    connection,
                    streamId,
                    responseStreamEndpoint->kind(),
                    baseServices.worker(),
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
                response = co_await routes.dispatchBufferedResponse(
                    request,
                    resolution,
                    requestMemory,
                    options.documentRoot.root,
                    dispatchServices);
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

    const auto resolveStreamRoute = [&routes, &streamRuntimes](
        Http2StreamState& streamState) -> Http2SansIoStreamRuntime* {
        return http2SelectStreamRoute(routes, streamRuntimes, streamState);
    };

    // Admit a stream for dispatch: EVERY dispatched stream gets a signal, so response
    // shared send-window pacing can be woken by
    // takeDrainedDataStreams no matter the body kind -- a plain route returning a large
    // file or a stream body blocks on the send window exactly like a streaming route,
    // and without a signal its blocked submit could never be woken (truncation + hang).
    const auto admitStream = [&](std::uint32_t streamId) {
        auto* signal =
            streamRuntimes.beginDispatch(streamId, baseServices.worker());
        if (signal == nullptr) {
            return false;
        }
        connection.pinStream(streamId);
        ++activeHandlerTasks;
        // Recycle the per-stream handler's coroutine frame (mirrors the h1 accept path);
        // under load the common request then does no extra heap work for the frame.
        try {
            asio::co_spawn(
                executor,
                taskAsAwaitable(dispatchOne(streamId)),
                asio::bind_allocator(
                    asio::recycling_allocator<void>(),
                    [&activeHandlerTasks,
                     &handlerFinished,
                     &writeSignal,
                     &terminateSession](std::exception_ptr exception) noexcept {
                        if (exception != nullptr) {
                            terminateSession(std::make_error_code(
                                std::errc::operation_canceled));
                        }
                        --activeHandlerTasks;
                        if (activeHandlerTasks == 0) {
                            handlerFinished.notify();
                        }
                        writeSignal.notify();
                    }));
        } catch (...) {
            --activeHandlerTasks;
            connection.unpinStream(streamId);
            eraseStreamRuntime(streamId);
            return false;
        }
        return true;
    };

    // Drain the core's event queue, admitting streams and routing body bytes.
    const auto drainEvents = [&]() {
        std::array<std::uint32_t,
                   Http2LocalSettings::kMaxConcurrentStreams>
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
        // One handler per event kind. The loop below is the dispatch; each
        // handler returns when it is done with its event, exactly as the
        // `continue` it replaced did.
        const auto onMessageHead = [&](const auto* messageHead) {
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
                return;
            }
            auto* streamRuntime = resolveStreamRoute(*streamState);
            if (streamRuntime == nullptr) {
                resetEventStream(
                    streamId, Http2ErrorCode::kInternalError);
                return;
            }
            const auto expectationPlan = streamState->expectationPlan(
                HttpUnsupportedExpectationPolicy::kReject);
            if (expectationPlan.sendContinue() != nullptr) {
                const auto status = connection.submitInterimResponseHead(
                    streamId,
                    HttpInterimResponseHead(ruvia::http_status::kContinue));
                if (status == Http2SubmitStatus::kAccepted) {
                    wakeWriter();
                } else {
                    if (status != Http2SubmitStatus::kClosed) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                    } else {
                        eraseStreamRuntime(streamId);
                    }
                    return;
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
        };
        const auto onBodyChunk = [&](const auto* bodyChunk) {
            const auto streamId = bodyChunk->streamId();
            auto* streamState = connection.stream(streamId);
            auto* streamRuntime = findStreamRuntime(streamId);
            if (streamState == nullptr || streamRuntime == nullptr) {
                if (streamState != nullptr) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                }
                return;
            }
            auto* selectedRoute = streamRuntime->selectedRoute();
            if (selectedRoute == nullptr) {
                resetEventStream(
                    streamId, Http2ErrorCode::kInternalError);
                return;
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
                return;
            }
            if (requestBody.streaming() != nullptr) {
                auto* signal = streamRuntime->signal();
                if (signal == nullptr) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                    return;
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
        };
        const auto onTunnelData = [&](const auto* tunnelData) {
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
                return;
            }
            auto* selectedRoute = streamRuntime->selectedRoute();
            auto* streamingBody = selectedRoute != nullptr
                ? selectedRoute->body().streaming()
                : nullptr;
            if (streamingBody == nullptr) {
                resetEventStream(
                    streamId, Http2ErrorCode::kInternalError);
                return;
            }
            streamingBody->queue().enqueue(tunnelData->bytes());
            signal->wake();
        };
        const auto onTunnelEnd = [&](const auto* tunnelEnd) {
            if (auto* signal = findSignal(tunnelEnd->streamId())) {
                signal->wake();
            }
        };
        const auto onMessageEnd = [&](const auto* messageEnd) {
            const auto streamId = messageEnd->streamId();
            if (connection.stream(streamId) == nullptr) {
                return;
            }
            auto* streamRuntime = findStreamRuntime(streamId);
            if (streamRuntime == nullptr) {
                resetEventStream(
                    streamId, Http2ErrorCode::kInternalError);
                return;
            }
            if (auto* signal = streamRuntime->signal()) {
                signal->wake();  // remote END_STREAM was committed before this event
            } else if (!admitStream(streamId)) {
                resetEventStream(
                    streamId, Http2ErrorCode::kInternalError);
            }
        };
        const auto onStreamClosed = [&](const auto* streamClosed) {
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
        };

        for (;;) {
            const auto event = connection.nextEvent();
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
    // LEVEL-triggered on the writer-done phase, not on signal notification alone:
    // notification
    // before a waiter exists must still make a late join return immediately, so if the writer finishes before the
    // reader reaches the join (common on an abrupt peer RST, where the write error
    // surfaces first), a cancel-only latch would be a no-op and the join would sleep
    // until time_point::max() forever. The bool makes a late join return immediately.
    WorkerSignal writerFinished(baseServices.worker());
    bool writerTaskDone = false;
    try {
        asio::co_spawn(
            executor,
            taskAsAwaitable(writerLoop()),
            [&writerFinished,
             &writerTaskDone,
             &lifecycle,
             &terminateSession](std::exception_ptr exception) noexcept {
                if (exception != nullptr) {
                    terminateSession(std::make_error_code(
                        std::errc::operation_canceled));
                }
                writerTaskDone = true;
                lifecycle.markWriterDone();
                writerFinished.notify();
            });
    } catch (...) {
        terminateSession(std::make_error_code(
            std::errc::operation_canceled));
        throw;
    }

    std::error_code readerTerminalError;
    std::exception_ptr readerFailure;
    try {
        // Establish the feed contract: drain any startup events before initial input.
        drainEvents();
        if (!connection.connectionError().has_value() && !initialBytes.empty()) {
            const auto result = feedAndDrain(initialBytes);
            initialInputRetained =
                result == Http2FeedResult::kConnectionNotStarted;
        }
        wakeWriter();

        // Reader loop: feed inbound bytes, then act on the drained events.
        if (!connection.connectionError().has_value() && !initialInputRetained &&
            !termination.terminated()) {
            // 4 KB read scratch. Requests are the small direction of HTTP/2
            // traffic (responses never pass through here), so a max-size 16 KB
            // frame arriving in several reads is the rare case, while the buffer
            // is resident in every connection's coroutine frame for the whole
            // connection.
            std::array<char, 4096> readBuffer;
            for (;;) {
                // Pick the inactivity phase: mid-header-block -> the tight header
                // timeout; no active Web runtime (including a pre-dispatch buffered
                // request body) -> kIdle so a keep-alive connection between requests
                // is governed by the keepalive timeout, not client_body_timeout;
                // otherwise a body/response is in progress -> kReadingPayload.
                // WebSocket tunnels carry their own heartbeat.
                scannerEntry.setPhase(http2SansIoInactivityPhase(
                    connection.headerBlockInProgress(),
                    streamRuntimes.size()));
                auto readCompletion = co_await asyncAsio<std::size_t>(
                    [&stream, &readBuffer](auto handler) mutable {
                        stream.async_read_some(
                            asio::buffer(readBuffer.data(), readBuffer.size()),
                            std::move(handler));
                });
                const auto ec = readCompletion.errorCode();
                const auto bytesRead = readCompletion.result();
                const bool workerStopped = !session.workerRunning();
                if (ec || bytesRead == 0 || workerStopped) {
                    readerTerminalError = ec ? ec : std::make_error_code(
                        workerStopped
                            ? std::errc::operation_canceled
                            : std::errc::connection_reset);
                    break;
                }
                scannerEntry.touch();
                const auto result = feedAndDrain(
                    std::string_view(readBuffer.data(), bytesRead));
                wakeWriter();  // feed may have produced ACKs / WINDOW_UPDATEs to flush
                if (result == Http2FeedResult::kConnectionNotStarted ||
                    result == Http2FeedResult::kProtocolFailure ||
                    lifecycle.writeFailed()) {
                    if (result == Http2FeedResult::kConnectionNotStarted ||
                        result == Http2FeedResult::kProtocolFailure) {
                        readerTerminalError = std::make_error_code(
                            std::errc::protocol_error);
                    }
                    break;
                }
            }
        }
    } catch (...) {
        readerFailure = std::current_exception();
        readerTerminalError = std::make_error_code(
            std::errc::operation_canceled);
    }

    // Reader, writer, and protocol failure converge on one terminal error. Active
    // bodies only report EOF after a protocol END_STREAM; transport termination is
    // delivered as an error and wakes every stream capability and cancellable sleep.
    if (!termination.terminated()) {
        if (!readerTerminalError) {
            readerTerminalError = connection.connectionError().has_value() ||
                    initialInputRetained
                ? std::make_error_code(std::errc::protocol_error)
                : std::make_error_code(std::errc::connection_aborted);
        }
        terminateSession(readerTerminalError);
    }

    // Explicitly join both operation sources. Runtime removal is not completion:
    // only the co_spawn callback proves the handler awaitable and frame are gone.
    // The level checks also cover completion that happened before either wait began.
    lifecycle.beginStopping();
    wakeWriter();
    while (activeHandlerTasks != 0) {
        co_await handlerFinished.wait();
    }
    wakeWriter();
    while (!writerTaskDone) {
        co_await writerFinished.wait();
    }
    if (readerFailure != nullptr) {
        std::rethrow_exception(readerFailure);
    }
    co_return;
}

}  // namespace ruvia::detail
