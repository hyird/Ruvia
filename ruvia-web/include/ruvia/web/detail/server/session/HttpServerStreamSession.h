#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/ratelimit/RateLimitDecision.h"
#include "ruvia/web/detail/server/RequestDeadline.h"
#include "ruvia/web/detail/server/HttpServerAccessLog.h"
#include "ruvia/web/detail/server/session/HttpServerConnectionGuards.h"
#include "ruvia/web/detail/server/session/HttpServerIdleWorkSet.h"
#include "ruvia/web/detail/server/http1/Http1RequestSequence.h"
#include "ruvia/web/detail/server/route/HttpServerBodyRouteCompletion.h"
#include "ruvia/web/detail/server/route/HttpServerStreamBodyRoute.h"
#include "ruvia/web/detail/server/route/HttpServerWebSocketRoute.h"
#include "ruvia/web/detail/server/stream/HttpServerResponseStreamRoute.h"
#include "ruvia/web/detail/server/response/HttpServerResponseState.h"
#include "ruvia/web/detail/server/response/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/response/HttpResponseWriter.h"
#include "ruvia/web/detail/server/request/RequestMemoryArena.h"
#include "ruvia/web/detail/server/http1/Http1ClosingRejection.h"
#include "ruvia/web/detail/http2/CleartextUpgrade.h"
#include "ruvia/web/detail/server/tls/HttpServerAutoHttps.h"
#include "ruvia/web/detail/server/request/HttpServerRequestState.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

// Member-template definitions for WebWorkerRuntime, kept out of its header so the
// class stays readable. Included as an ordinary header: everything used here is
// included here.

namespace ruvia::detail {

template <typename Stream>
Task<void> WebWorkerRuntime::handleStreamSession(HttpServerListener& listener, Stream& stream, TcpSocket& socket, ContextServices baseRouteServices) {
    // Resident connection identity (held for the whole connection): the scanner
    // entry, the keep-alive request sequence, the remote address, and the count
    // of buffered bytes. The heavy per-request working set (read buffer, request arena,
    // parse result, response head, file chunk) is borrowed from a per-worker
    // pool only while the connection is actively serving and returned the moment
    // it goes idle, so an idle keep-alive connection holds none of it.
    ConnectionScanner::Entry scannerEntry;
    ConnectionScanner::Guard scannerGuard(&connectionScanner_, scannerEntry, socket);
    const auto& routes = routes_;
    const auto remoteAddress = baseRouteServices.connInfo().remote().address();
    // Re-resolved per request: one keep-alive connection carries many requests,
    // each with its own forwarding headers. Falls back to the peer until the
    // first request line is parsed.
    auto clientAddress = remoteAddress;
    Http1RequestSequence requestSequence(options_.maxRequestsPerConnection);
    std::size_t usedBytes = 0;
    ConnectionWorkSet* workSet = nullptr;
    WorkSetReturn workSetReturn(workSetPool_, workSet);
    // Connection-resident landing pad for the first bytes of a request that
    // arrives while the connection holds no work set (see the idle wait below).
    std::array<char, kIdleResidentReadBytes> idleReadBuffer;
    std::size_t idleReadBytes = 0;
    // nginx-aligned wait semantics. The first request on a connection, and any
    // partially-received request header, are bounded by requestHeaderTimeout
    // (kReadingInitial). The idle wait for the *next* request on a reused
    // keep-alive connection -- no bytes of it received yet -- is bounded by
    // idleTimeout (kIdle), matching nginx keepalive_timeout. Flips true
    // once a request has been served so later idle waits use the keepalive
    // deadline instead of the header deadline.
    bool servedKeepaliveRequest = false;

    constexpr bool kPlainTcp = std::is_same_v<std::remove_cvref_t<Stream>, TcpSocket>;
    for (;;) {
        scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);

        // Borrow-on-use / return-on-idle for the whole work set: when the
        // connection has no buffered bytes, return the work set to the
        // per-worker pool and read the next request's first bytes into the
        // small connection-resident buffer instead, so an idle keep-alive
        // connection occupies no work set (memory scales with in-flight
        // requests, not total connections). Reading directly -- rather than a
        // bufferless readiness wait -- costs no extra reactor pass when data
        // is already queued. A pipelined burst (usedBytes > 0) keeps its work
        // set and skips this. Plain TCP only: a TLS engine may buffer a
        // decrypted record that raw socket readiness cannot see, so an idle
        // wait there could stall; TLS holds across the connection.
        if constexpr (kPlainTcp) {
            if (plainTcpShouldWaitForNextRequest(usedBytes)) {
                releaseIdleWorkSet(workSetPool_, workSet);
                // Idle wait for the next keep-alive request uses idleTimeout;
                // the connection's first request uses requestHeaderTimeout.
                scannerEntry.setPhase(servedKeepaliveRequest ? ConnectionScanner::Phase::kIdle : ConnectionScanner::Phase::kReadingInitial);
                auto idleCompletion = co_await asyncAsio<std::size_t>([&socket, &idleReadBuffer](auto handler) mutable { socket.async_read_some(asio::buffer(idleReadBuffer.data(), idleReadBuffer.size()), std::move(handler)); });
                const auto idleEc = idleCompletion.errorCode();
                const auto idleBytes = idleCompletion.result();
                if (idleEc || !httpServerWorkerRunning(workerState_)) {
                    co_return;
                }
                idleReadBytes = idleBytes;
            }
        }
        if (workSet == nullptr) {
            workSet = workSetPool_.acquire();
        }
        auto& readBuffer = workSet->readBuffer;
        auto& parser = workSet->parser;
        auto& parsed = workSet->parsed;
        auto& responseHead = workSet->responseHead;
        auto& fileChunk = workSet->fileChunk;
        auto& routeResolution = workSet->routeResolution;

        if constexpr (kPlainTcp) {
            if (idleReadBytes > 0) {
                static_assert(kIdleResidentReadBytes <= kInitialReadBufferBytes);
                std::memcpy(readBuffer.data(), idleReadBuffer.data(), idleReadBytes);
                usedBytes = idleReadBytes;
                idleReadBytes = 0;
                scannerEntry.touch();
            }
        }

        std::optional<RequestMemory> requestMemoryStorage;
        auto& requestMemory = emplaceRequestMemory(requestMemoryStorage, memory_, std::span<std::byte>(workSet->arenaBlock, sizeof(workSet->arenaBlock)));
        HttpResponse response({.resource = requestMemory.resource()});
        HttpResponseCodingPolicy responseCodingPolicy = HttpResponseCodingPolicy::disabled();
        // Holds the next pipelined request from the moment a body route hands it
        // over until the read buffer is cleaned up below. Declared before
        // requestCompletion, which borrows it, and empty for the common case of a
        // client that does not pipeline.
        std::pmr::string pipelineStash(requestMemory.resource());
        // Declared here, before requestCompletion and everything that borrows
        // the services below, so the deadline's stop source outlives every
        // dispatch that observes its token.
        std::optional<RequestDeadline> requestDeadline;
        ContextServices requestServices = baseRouteServices;
        std::optional<Http1SessionRequestCompletion> requestCompletion;
        // Rejections that close the connection funnel through one co_await
        // site after the read loop: every co_await expression in a coroutine
        // reserves its own frame slots for the call's temporaries (GCC does
        // not overlap them), so inlining handleError at each rejection site
        // costs ~660 resident bytes per site in every connection's frame.
        Http1ClosingRejection closingRejection;
        std::size_t headerSearchOffset = 0;
        const auto requestStart = std::chrono::steady_clock::now();
        for (;;) {
            if constexpr (kPlainTcp) {
                if (usedBytes > 0) {
                    const auto h2Result = co_await dispatchCleartextHttp2Preface(
                        Http2ServerSessionSetup<Stream>{
                            .stream = stream,
                            .socket = socket,
                            .memory = memory_,
                            .routes = routes_,
                            .options = options_,
                            .scannerEntry = scannerEntry,
                            .services = baseRouteServices,
                            .workerState = workerState_,
                        },
                        readBuffer, usedBytes, listener.redirect() != nullptr);
                    if (h2Result == CleartextHttp2DispatchResult::kSessionFinished) {
                        co_return;
                    }
                    if (h2Result == CleartextHttp2DispatchResult::kContinueReadLoop) {
                        continue;
                    }
                }
            }
            const auto bufferView = std::string_view(readBuffer.data(), usedBytes);
            responseCodingPolicy = HttpResponseCodingPolicy::disabled();
            parser.parseHead(bufferView, parsed, headerSearchOffset);
            HttpRequestAccess::setResource(parsed.request, requestMemory.resource());
            if (const auto* requestHead = parsed.headReady()) {
                // Reset phase so requestHeaderTimeout stops counting against dispatch
                // time. Body readers will set kReadingPayload on their own; the
                // streaming/websocket paths set their own phases below; the
                // buffered write path sets kWriting before responding. Until
                // one of those transitions, idleTimeout governs as the
                // deadman switch for hung handlers.
                scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
                // Negotiate against every coding first. A document root or a
                // Context::staticFile route may serve an indexed precompressed
                // sidecar even when this worker has no runtime encoder. The
                // actual encoder capability is carried to representation
                // preparation instead of pruning the request's choices here.
                const auto responseCodingNegotiation = parsed.responseCodingSelection();
                if (const auto* selection = responseCodingNegotiation.selected()) {
                    responseCodingPolicy = HttpResponseCodingPolicy::selected(*selection);
                } else {
                    // Buffered routes may still produce a representation-free
                    // 204/205/304. Keep the negotiation failure typed until
                    // the response status is known instead of rejecting every
                    // request before its handler runs.
                    responseCodingPolicy = HttpResponseCodingPolicy::noAcceptableCoding();
                }
                // Reset per request: a keep-alive connection serves many, and
                // each gets its own deadline or none. Resolve before any
                // server-layer rejection below: a custom onError/onNotFound
                // handler is a handler too and must see the request stop token.
                requestDeadline.reset();
                requestServices = baseRouteServices;
                routeResolution = routes.resolve(parsed.request);
                // Keyed on the client, not the hop: behind a trusted proxy every
                // request would otherwise share the proxy's single key.
                clientAddress = baseRouteServices.resolveConnInfo(parsed.request).client().address();
                const auto* resolved = routeResolution.resolved();
                const auto handlerDeadline = effectiveHandlerDeadline(options_.deadline ? options_.deadline->handler : std::nullopt, resolved != nullptr ? resolved->route().deadlineMs() : 0);
                if (handlerDeadline > std::chrono::milliseconds::zero()) {
                    requestDeadline.emplace(stopToken_);
                    requestDeadline->arm(workerHandle_, handlerDeadline);
                    requestServices = baseRouteServices.withStopToken(requestDeadline->token()).withRequestDeadline(&*requestDeadline);
                }

                const auto expectationPlan = parsed.bodyPlan.expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
                if (const auto* rejection = expectationPlan.rejection()) {
                    // Expect extensions are valid HTTP syntax. The protocol parser
                    // reports the semantic fact; this Web product deliberately does
                    // not implement extensions beyond 100-continue and chooses the
                    // RFC 9110-permitted 417 response before reading request content.
                    closingRejection = Http1ClosingRejection::error(copyHttpProtocolErrorInfo(requestMemory.resource(), rejection->protocolError()));
                    break;
                }
                if (const auto* redirect = listener.redirect()) {
                    if (requestKnownHeader(parsed.request, RequestKnownHeader::kHost).empty()) {
                        closingRejection = Http1ClosingRejection::error(HttpErrorInfo({.status = ruvia::http_status::kBadRequest, .message = "missing Host header"}));
                        break;
                    }
                    response = makeAutoHttpsRedirectResponse(parsed.request, requestMemory, redirect->httpsPort);
                    if (httpResponseNeedsNotAcceptable(responseCodingPolicy, parsed.request, response)) {
                        response = co_await routes.handleError(
                            parsed.request,
                            requestMemory,
                            HttpErrorInfo({
                                .status = ruvia::http_status::kNotAcceptable,
                                .code = "not_acceptable",
                                .message = "no acceptable response content coding",
                            }),
                            requestServices);
                        // The redirect branch commits directly because its
                        // connection is intentionally closing; make the
                        // generated policy error terminal rather than letting
                        // it inherit the original negotiated coding promise.
                        responseCodingPolicy = HttpResponseCodingPolicy::disabled();
                    }
                    const auto connectionPlan = requireHttp1FinalResponseCommit(response, parsed.connectionPlan.requireClose());
                    requestCompletion.emplace(Http1SessionRequestCompletion::makeBufferedClosing(connectionPlan));
                    scannerEntry.touch();
                    break;
                }

                const auto appRateLimit = decideRequestRateLimit(&rateLimiter_, clientAddress);
                if (const auto* rejection = appRateLimit.rejection()) {
                    closingRejection = Http1ClosingRejection::rateLimit(rateLimitRejectionError(), *rejection);
                    break;
                }

                if (resolved == nullptr) {
                    if (const auto bodyFailure = contentLengthLimitFailure(parsed.bodyPlan, ProtocolByteLimit::limited(options_.maxBufferedBodyBytes))) {
                        closingRejection = Http1ClosingRejection::error(copyHttpProtocolErrorInfo(requestMemory.resource(), bodyFailure->protocolError()));
                        break;
                    }
                    response = co_await routes.dispatchBufferedResponse(
                        parsed.request,
                        routeResolution,
                        requestMemory,
                        options_.documentRoot.binding(),
                        requestServices,
                        requestServices.precompressedStaticFiles() ? StaticFileSelectionMode::kPrecompressed : StaticFileSelectionMode::kIdentityOnly);
                    // An unresolved request never consumes its body, regardless
                    // of whether the shared Web dispatch selected a document-root
                    // file, 404, 405, or OPTIONS response.
                    auto connectionPlan = http1ApplyRequestBodyConsumption(parsed.connectionPlan, parsed.bodyPlan.requiresConsumption() ? Http1RequestBodyConsumption::kIncomplete : Http1RequestBodyConsumption::kComplete);
                    connectionPlan = finalizeBufferedRouteResponse(response, connectionPlan, requestSequence);
                    requestCompletion.emplace(Http1SessionRequestCompletion::makeBufferedUnrestored(connectionPlan, requestHead->headerBytes()));
                    scannerEntry.touch();
                    break;
                }

                // One bundle of what every route dispatch below needs from
                // this session; each dispatcher adds only its own arguments.
                const auto routeDispatch = [&] {
                    return Http1RouteDispatch<Stream>{
                        .stream = stream,
                        .memory = memory_,
                        .scannerEntry = scannerEntry,
                        .parsed = parsed,
                        .responseCoding = *responseCodingPolicy.selection(),
                        .responseCodingAvailability = options_.compression.has_value() ? HttpResponseCodingAvailability::kIdentityAndCompression : HttpResponseCodingAvailability::kIdentityOnly,
                        .routes = routes,
                        .requestMemory = requestMemory,
                        .baseRouteServices = requestServices,
                        .options = options_,
                        .response = response,
                        .requestSequence = requestSequence,
                    };
                };

                const auto& route = resolved->route();
                const auto& endpoint = route.endpoint();
                if (responseCodingPolicy.negotiationFailed() && endpoint.responseStream() != nullptr) {
                    // A response stream commits its representation before a
                    // buffered status can be inspected. WebSocket upgrades do
                    // not select an HTTP response representation.
                    closingRejection = Http1ClosingRejection::error(HttpErrorInfo({.status = ruvia::http_status::kNotAcceptable, .code = "not_acceptable", .message = "no acceptable response content coding"}));
                    break;
                }
                const auto maxRequestBodyBytes = requestBodyByteLimit(endpoint.requestBodyMode(), options_.maxStreamBodyBytes, options_.maxBufferedBodyBytes, resolved->route().maxRequestBodyBytes());
                if (const auto bodyFailure = contentLengthLimitFailure(parsed.bodyPlan, maxRequestBodyBytes)) {
                    closingRejection = Http1ClosingRejection::error(copyHttpProtocolErrorInfo(requestMemory.resource(), bodyFailure->protocolError()));
                    break;
                }

                if (endpoint.webSocket() != nullptr) {
                    const auto pendingFrames = std::string_view(readBuffer.data() + requestHead->headerBytes(), usedBytes - requestHead->headerBytes());
                    auto webSocketCompletion = co_await dispatchHttpWebSocketRoute(routeDispatch(), *resolved, pendingFrames);
                    if (!webSocketCompletion.has_value()) {
                        co_return;
                    }
                    requestCompletion.emplace(std::move(*webSocketCompletion));
                    break;
                }

                if (endpoint.responseStream() != nullptr) {
                    requestCompletion.emplace(co_await dispatchHttpResponseStreamRoute(routeDispatch(), responseHead, *requestHead, *resolved));
                    break;
                }
                const auto* bufferedEndpoint = endpoint.buffered();
                if (bufferedEndpoint != nullptr && bufferedEndpoint->requestBodyMode() == RequestBodyMode::kStream) {
                    requestCompletion.emplace(co_await dispatchHttpStreamBodyRoute(routeDispatch(), *requestHead, routeResolution, readBuffer, usedBytes, pipelineStash));
                    break;
                }

                // Buffered-body dispatch, inlined into the session loop: this
                // is the hot path for every plain buffered route, and a
                // dedicated coroutine here would cost one frame allocation
                // per request.
                {
                    const auto bodyAndPipeline = httpBodyAndPipeline(*requestHead, readBuffer, usedBytes);

                    // The body reader/loader setup can throw (e.g. constructing a
                    // transfer-coding decoder for a bad Transfer-Encoding), so it
                    // stays guarded. The dispatch itself never throws:
                    // dispatchBufferedResponse turns any handler or routing
                    // failure into a response, so it sits outside the guard.
                    std::exception_ptr bodySetupException;
                    HttpLazyBufferedBodyRouteState<Stream> bodyState;
                    try {
                        prepareHttpLazyBufferedBodyRoute(bodyState, routeDispatch(), maxRequestBodyBytes, bodyAndPipeline);
                    } catch (...) {
                        bodySetupException = std::current_exception();
                    }

                    if (bodySetupException != nullptr) {
                        requestCompletion.emplace(co_await completeFailedHttpBodyRoute(scannerEntry, bodySetupException, parsed, routes, requestMemory, requestServices, response));
                        break;
                    }

                    response = co_await routes.dispatchBufferedResponse(parsed.request, routeResolution, requestMemory, options_.documentRoot.binding(), bodyState.withLoader(requestServices));

                    requestCompletion.emplace(completeSuccessfulHttpBodyRoute(scannerEntry, response, parsed.connectionPlan, requestSequence, bodyState.consumption(), pipelineStash, [&bodyState](std::pmr::string& stash) { bodyState.takePipeline(stash); }));
                    break;
                }
            }

            if (const auto* failure = parsed.failure()) {
                if constexpr (kPlainTcp) {
                    if (listener.redirect() == nullptr && shouldDropInvalidCleartextHttp1Input(bufferView, failure->source())) {
                        co_return;
                    }
                }
                const auto error = failure->protocolError();
                closingRejection = Http1ClosingRejection::error(copyHttpProtocolErrorInfo(requestMemory.resource(), error));
                break;
            }

            headerSearchOffset = usedBytes > 3 ? usedBytes - 3 : 0;

            // With no request bytes yet on a reused connection this read is the
            // keepalive idle wait (idleTimeout); once any header bytes are
            // buffered, or on the first request, requestHeaderTimeout governs.
            scannerEntry.setPhase((usedBytes == 0 && servedKeepaliveRequest) ? ConnectionScanner::Phase::kIdle : ConnectionScanner::Phase::kReadingInitial);
            growReadBuffer(readBuffer, usedBytes);
            if (usedBytes == readBuffer.size()) {
                const auto error = httpParseProtocolError(HttpParseError::kHeaderTooLarge);
                closingRejection = Http1ClosingRejection::error(copyHttpProtocolErrorInfo(requestMemory.resource(), error));
                break;
            }

            auto readCompletion = co_await asyncAsio<std::size_t>([&stream, &readBuffer, usedBytes](auto handler) mutable { stream.async_read_some(asio::buffer(readBuffer.data() + usedBytes, readBuffer.size() - usedBytes), std::move(handler)); });
            const auto ec = readCompletion.errorCode();
            const auto bytesRead = readCompletion.result();
            if (ec) {
                co_return;
            }

            usedBytes += bytesRead;
            scannerEntry.touch();
        }

        // Shared exit for every rejection recorded above: one co_await site
        // keeps one set of call temporaries in the frame instead of one per
        // rejection branch.
        if (const auto* closingError = closingRejection.error()) {
            response = co_await routes.handleError(parsed.request, requestMemory, *closingError, requestServices);
            if (const auto* rateLimit = closingRejection.rateLimit()) {
                applyRateLimitRejectionHeaders(response, *rateLimit);
            }
            requestCompletion.emplace(Http1SessionRequestCompletion::makeBufferedClosing(requireHttp1FinalResponseCommit(response, parsed.connectionPlan.requireClose())));
        }

        if (!requestCompletion) {
            throw std::logic_error("HTTP/1 request dispatch returned no terminal completion");
        }
        auto connectionPlan = requestCompletion->connectionPlan();
        if (requestCompletion->bufferedResponse() != nullptr) {
            scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);
            auto preparation = co_await prepareBufferedHttpResponseAsync(parsed.request, responseCodingPolicy, response, options_, workerHandle_);
            if (const auto error = httpBufferedResponsePreparationError(responseCodingPolicy, parsed.request, response, preparation.compressionResult())) {
                response = co_await routes.handleError(
                    parsed.request,
                    requestMemory,
                    *error,
                    requestServices);
                preparation = co_await prepareBufferedHttpResponseAsync(parsed.request, responseCodingPolicy, response, options_, workerHandle_);
                if (httpBufferedResponsePreparationError(responseCodingPolicy, parsed.request, response, preparation.compressionResult()).has_value()) {
                    // The negotiated coding could not be installed even on
                    // the generated terminal error. Make the terminal error
                    // state explicit before allowing identity bytes.
                    responseCodingPolicy = HttpResponseCodingPolicy::disabled();
                    preparation = co_await prepareBufferedHttpResponseAsync(parsed.request, responseCodingPolicy, response, options_, workerHandle_);
                }
                connectionPlan = requireHttp1FinalResponseCommit(response, connectionPlan);
                requestCompletion = requestCompletion->withBufferedConnectionPlan(connectionPlan);
                connectionPlan = requestCompletion->connectionPlan();
            }
            const auto writePlan = preparation.writePlan();
            const auto responsePlan = http1BufferedResponsePlan(writePlan, connectionPlan);
            const auto writeResult = co_await writeResponse(stream, memory_, &responseHead, &fileChunk, response, responsePlan);
            scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
            if (const auto committedStatus = writeResult.committedStatus()) {
                recordHttpAccess(options_.accessLog, parsed.request, clientAddress, *committedStatus, requestStart);
            }
            if (writeResult.completed() == nullptr) {
                co_return;
            }
        } else if (const auto* committed = requestCompletion->committedStream()) {
            scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
            recordHttpAccess(options_.accessLog, parsed.request, clientAddress, committed->status(), requestStart);
        } else {
            throw std::logic_error("HTTP/1 request completion has no wire alternative");
        }

        if (connectionPlan.disposition() == Http1ClosePolicy::kCloseAfterResponse || !httpServerWorkerRunning(workerState_)) {
            co_return;
        }
        applyReusableHttp1RequestBufferCompletion(requestCompletion->bufferCompletion(), readBuffer, usedBytes);
        trimReadBufferStorage(readBuffer, usedBytes);
        // A request completed and the connection is being reused: the next
        // wait with no buffered bytes is a keepalive idle wait.
        servedKeepaliveRequest = true;
    }
}

}  // namespace ruvia::detail
