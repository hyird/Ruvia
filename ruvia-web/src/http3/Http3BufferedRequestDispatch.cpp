#include "ruvia/web/detail/http3/Http3BufferedRequestDispatch.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3WebSocketHandshake.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/router/RouteEndpoint.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/response/HttpBufferedResponse.h"
#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"
#include "ruvia/web/detail/websocket/WebSocketResponseHeaders.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::int64_t steadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] bool hasUnsupportedWebSocketVersion(const HttpRequest& request) noexcept {
    std::size_t versionCount = 0;
    std::string_view version;
    for (const auto& header : request.headers()) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "sec-websocket-version")) {
            continue;
        }
        if (++versionCount != 1) {
            return false;
        }
        version = header.value();
    }
    return versionCount == 1 && version != "13";
}

class Http3WebSocketTransport final {
public:
    explicit Http3WebSocketTransport(Http3BufferedRequestDispatch& dispatch) noexcept
        : dispatch_(dispatch) {}

    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return dispatch_.executor();
    }

    [[nodiscard]] Task<WsTransportReadResult> readMore(std::pmr::string& buffer) {
        return dispatch_.readTunnel(buffer);
    }

    [[nodiscard]] Task<std::error_code> writeBytes(std::string_view bytes,
        WebSocketServerTransportDisposition disposition) {
        return dispatch_.writeTunnel(bytes, disposition);
    }

    void abort() noexcept {
        dispatch_.abortTunnel();
    }

private:
    Http3BufferedRequestDispatch& dispatch_;
};

}  // namespace

Http3BufferedRequestDispatch::Http3BufferedRequestDispatch(
    Http3SansIoSessionEngine& session, const RouteTable& routes, WorkerMemory& worker,
    ContextServices services, const HttpServerOptions& options,
    Http3StreamMailbox& outbound, Http3StreamMessageId messageId,
    ConnectionScanner::Entry& scannerEntry, asio::any_io_executor executor,
    Http3TunnelCallbacks tunnelCallbacks)
    : session_(session),
      routes_(routes),
      worker_(worker),
      services_(std::move(services)),
      options_(options),
      outbound_(outbound),
      messageId_(messageId),
      scannerEntry_(scannerEntry),
      executor_(std::move(executor)),
      tunnelCallbacks_(tunnelCallbacks),
      tunnelInputAvailable_(services_.worker()),
      tunnelOutputAvailable_(services_.worker()),
      tunnelHandshakeFrame_(worker_.resource()),
      tunnelDataFrame_(worker_.resource()) {}

Http3BufferedRequestDispatch::~Http3BufferedRequestDispatch() {
    if (handlerActive_) {
        std::terminate();
    }
    disarmPeerTransportFinTimeout();
    (void)releaseDispatchStorage();
}

Task<Http3BufferedRequestDispatch::PrepareStatus> Http3BufferedRequestDispatch::prepare() & {
    if (!onWorker()) {
        co_return PrepareStatus::kWrongWorker;
    }
    if (state_ == State::kCancelled) {
        co_return PrepareStatus::kCancelled;
    }
    if (state_ != State::kCold) {
        co_return PrepareStatus::kAlreadyPrepared;
    }
    if (cancellationRequested()) {
        cancel();
        co_return PrepareStatus::kCancelled;
    }

    state_ = State::kPreparing;
    auto acquired = session_.acquireRequest(messageId_.streamId);
    if (!acquired) {
        state_ = State::kFailed;
        co_return PrepareStatus::kRequestUnavailable;
    }
    lease_.emplace(std::move(*acquired));

    try {
        requestMemory_.emplace(worker_);
        combinedWorkerAndRequestStop_ =
            combineStopTokens(services_.stopToken(), requestStopSource_.token());
        requestDeadline_.emplace(combinedWorkerAndRequestStop_);
        // Tunnel I/O waits on worker signals, so stop must wake both directions.
        requestDeadline_->token().registerCallback(tunnelStopRegistration_, [this]() noexcept {
            tunnelInputAvailable_.notify();
            tunnelOutputAvailable_.notify();
        });
        const auto* resolved = lease_->resolution().resolved();
        const auto routeDeadline = resolved != nullptr ? resolved->route().deadlineMs() : 0;
        const auto handlerDeadline = effectiveHandlerDeadline(
            options_.deadline ? std::optional{options_.deadline->handler} : std::nullopt,
            routeDeadline);
        if (handlerDeadline > std::chrono::milliseconds::zero()) {
            requestDeadline_->arm(services_.worker(), handlerDeadline);
            deadlineArmed_ = true;
        }
        requestServices_.emplace(services_.withRequestDeadline(*requestDeadline_));
        if (cancellationRequested()) {
            cancel();
            (void)releaseDispatchStorage();
            co_return PrepareStatus::kCancelled;
        }
        state_ = State::kPrepared;
        co_return PrepareStatus::kPrepared;
    } catch (...) {
        fail(std::current_exception());
        co_return PrepareStatus::kFailed;
    }
}

Task<Http3BufferedRequestDispatch::RunStatus> Http3BufferedRequestDispatch::runHandler() & {
    if (!onWorker()) {
        co_return RunStatus::kWrongWorker;
    }
    if (state_ == State::kCold) {
        const auto prepared = co_await prepare();
        if (prepared == PrepareStatus::kCancelled) {
            co_return RunStatus::kCancelled;
        }
        if (prepared != PrepareStatus::kPrepared) {
            co_return RunStatus::kFailed;
        }
    } else if (state_ == State::kCancelled) {
        co_return RunStatus::kCancelled;
    } else if (state_ != State::kPrepared) {
        co_return state_ == State::kFailed ? RunStatus::kFailed : RunStatus::kAlreadyRun;
    }

    if (cancellationRequested()) {
        cancel();
        (void)releaseDispatchStorage();
        co_return RunStatus::kCancelled;
    }

    state_ = State::kRunning;
    handlerActive_ = true;
    RunStatus result = RunStatus::kFailed;
    try {
        result = co_await runHandlerInner();
    } catch (...) {
        failure_ = std::current_exception();
    }
    // The child frame (including Router responses and preparation temporaries)
    // is gone before releasing any resource those objects may reference.
    handlerActive_ = false;
    if (cancellationRequested() || result == RunStatus::kCancelled) {
        cancel();
        (void)releaseDispatchStorage();
        co_return RunStatus::kCancelled;
    }
    if (result == RunStatus::kTunnelComplete) {
        state_ = State::kComplete;
        (void)releaseDispatchStorage();
        co_return result;
    }
    if (result != RunStatus::kResponseReady) {
        fail(failure_);
        co_return result;
    }
    state_ = State::kOutputReady;
    co_return RunStatus::kResponseReady;
}

Task<Http3BufferedRequestDispatch::RunStatus> Http3BufferedRequestDispatch::runHandlerInner() {
    const auto& request = lease_->request().request();
    const auto& resolution = lease_->resolution();
    if (const auto* resolved = resolution.resolved();
        resolved != nullptr && resolved->route().endpoint().webSocket() != nullptr) {
        co_return co_await runWebSocketHandler();
    }
    const auto codingNegotiation = httpResponseCodingFor(request);
    auto codingPolicy = HttpResponseCodingPolicy::disabled();
    if (const auto* selection = codingNegotiation.selected()) {
        codingPolicy = HttpResponseCodingPolicy::selected(*selection);
    } else {
        codingPolicy = HttpResponseCodingPolicy::noAcceptableCoding();
    }

    auto response = co_await routes_.dispatchBufferedResponse(request, resolution,
        *requestMemory_, options_.documentRoot.binding(), *requestServices_,
        services_.precompressedStaticFiles() ? StaticFileSelectionMode::kPrecompressed
                                             : StaticFileSelectionMode::kIdentityOnly);
    if (cancellationRequested()) {
        co_return RunStatus::kCancelled;
    }
    response_.emplace(std::move(response));

    auto preparation = co_await prepareBufferedHttpResponseAsync(
        request, codingPolicy, *response_, options_, services_.worker());
    if (cancellationRequested()) {
        co_return RunStatus::kCancelled;
    }
    if (const auto error = httpBufferedResponsePreparationError(
            codingPolicy, request, *response_, preparation.compressionResult())) {
        response_.reset();
        auto errorResponse = co_await routes_.handleError(
            request, *requestMemory_, *error, *requestServices_);
        if (cancellationRequested()) {
            co_return RunStatus::kCancelled;
        }
        response_.emplace(std::move(errorResponse));
        preparation = co_await prepareBufferedHttpResponseAsync(
            request, codingPolicy, *response_, options_, services_.worker());
        if (cancellationRequested()) {
            co_return RunStatus::kCancelled;
        }
        if (httpBufferedResponsePreparationError(
                codingPolicy, request, *response_, preparation.compressionResult())
                .has_value()) {
            codingPolicy = HttpResponseCodingPolicy::disabled();
            preparation = co_await prepareBufferedHttpResponseAsync(
                request, codingPolicy, *response_, options_, services_.worker());
            if (cancellationRequested()) {
                co_return RunStatus::kCancelled;
            }
        }
    }

    auto output = Http3BufferedResponseOutput::create(
        *response_, preparation.writePlan(), worker_, outbound_, messageId_);
    if (!output) {
        co_return output.error() == Http3BufferedResponseOutputError::kFileBodyUnsupported
            ? RunStatus::kFilePayloadUnsupported
            : RunStatus::kFailed;
    }
    output_.emplace(std::move(*output));
    co_return RunStatus::kResponseReady;
}

Task<Http3BufferedRequestDispatch::RunStatus>
Http3BufferedRequestDispatch::runWebSocketHandler() {
    const auto& request = lease_->request().request();
    const auto& resolution = lease_->resolution();
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr || resolved->route().endpoint().webSocket() == nullptr) {
        co_return RunStatus::kFailed;
    }
    const auto& endpoint = *resolved->route().endpoint().webSocket();
    peerTransportFinTimeout_ = endpoint.lifecycle().peerTransportFinTimeout;
    const auto protocol = lease_->request().extendedConnectProtocol();
    const auto validation = validateHttp3WebSocketHandshake(
        request, protocol, !session_.tunnelReceiveEnded(messageId_.streamId));

    auto codingPolicy = HttpResponseCodingPolicy::disabled();
    const auto codingNegotiation = httpResponseCodingFor(request);
    if (const auto* selection = codingNegotiation.selected()) {
        codingPolicy = HttpResponseCodingPolicy::selected(*selection);
    } else {
        codingPolicy = HttpResponseCodingPolicy::noAcceptableCoding();
    }
    auto stageResponse = [this, &request, codingPolicy](HttpResponse response) mutable
        -> Task<RunStatus> {
        response_.emplace(std::move(response));
        auto preparation = co_await prepareBufferedHttpResponseAsync(
            request, codingPolicy, *response_, options_, services_.worker());
        if (cancellationRequested()) {
            co_return RunStatus::kCancelled;
        }
        if (const auto error = httpBufferedResponsePreparationError(
                codingPolicy, request, *response_, preparation.compressionResult())) {
            response_.reset();
            auto errorResponse = co_await routes_.handleError(
                request, *requestMemory_, *error, *requestServices_);
            if (cancellationRequested()) {
                co_return RunStatus::kCancelled;
            }
            response_.emplace(std::move(errorResponse));
            preparation = co_await prepareBufferedHttpResponseAsync(
                request, HttpResponseCodingPolicy::disabled(), *response_, options_,
                services_.worker());
        }
        auto output = Http3BufferedResponseOutput::create(
            *response_, preparation.writePlan(), worker_, outbound_, messageId_);
        if (!output) {
            co_return output.error() == Http3BufferedResponseOutputError::kFileBodyUnsupported
                ? RunStatus::kFilePayloadUnsupported
                : RunStatus::kFailed;
        }
        output_.emplace(std::move(*output));
        co_return RunStatus::kResponseReady;
    };

    if (!validation) {
        const auto& failure = validation.error();
        const auto unsupported = failure.kind() ==
                                     Http3WebSocketHandshakeFailure::Kind::kUnsupportedVersion ||
                                 (request.knownMethod() == HttpKnownMethod::kConnect &&
                                     httpAsciiEqualsIgnoreCase(protocol, "websocket") &&
                                     hasUnsupportedWebSocketVersion(request));
        const auto protocolError = failure.protocolError();
        auto response = co_await routes_.handleError(request, *requestMemory_,
            HttpErrorInfo({.status = protocolError.status(),
                .code = unsupported ? "websocket_version_unsupported"
                                    : "invalid_websocket_handshake",
                .message = protocolError.what()}),
            *requestServices_);
        if (unsupported) {
            response.removeHeader("Sec-WebSocket-Version");
            response.header("Sec-WebSocket-Version", "13");
        } else {
            failure.applyRequiredResponseHeaders(response);
        }
        co_return co_await stageResponse(std::move(response));
    }

    using Connection = WebSocketConnection<Http3WebSocketTransport>;
    std::optional<Connection> webSocketConnection;
    auto upgradeAndRun = [&](Context& context) -> Task<void> {
        const auto responseHeaders = webSocketResponseHeaders(context);
        auto handshake = makeHttp3WebSocketHandshake(request, protocol, true,
            {.supportedSubprotocols = endpoint.subprotocols(),
                .responseHeaders = responseHeaders,
                .resource = requestMemory_->resource(),
                .deflate = endpoint.deflate()});
        if (!handshake) {
            throw std::runtime_error("HTTP/3 WebSocket handshake construction failed");
        }
        if (tunnelCallbacks_.attachScanner == nullptr ||
            !tunnelCallbacks_.attachScanner(
                tunnelCallbacks_.context, messageId_.streamId, scannerEntry_)) {
            throw std::runtime_error("HTTP/3 WebSocket scanner attachment failed");
        }
        ContextAccess::markWebSocketHandshakeStarted(context);
        if (const auto error = co_await publishTunnelHandshake(handshake->headersFrame()); error) {
            throw std::system_error(error, "failed to publish HTTP/3 WebSocket handshake");
        }
        webSocketConnection.emplace(Http3WebSocketTransport{*this}, services_.worker(),
            scannerEntry_, endpoint.lifecycle(),
            ProtocolByteLimit::limited(options_.maxWebSocketMessageBytes),
            context.pool(), std::string_view{}, handshake->compression(),
            endpoint.deflate().compressionLevel);
        co_await invokeWebSocketHandler(*webSocketConnection, scannerEntry_,
            endpoint.handler(), context);
    };
    const auto terminal = makeCallableRef<void, Context&>(upgradeAndRun);
    std::optional<HttpResponse> buffered;
    std::exception_ptr exception;
    try {
        buffered = co_await routes_.dispatchWebSocket(
            request, *resolved, *requestMemory_, terminal, *requestServices_);
    } catch (...) {
        exception = std::current_exception();
    }

    if (webSocketConnection.has_value()) {
        co_await finishWebSocketSession(*webSocketConnection, exception,
            options_.connectionFailure, services_.connInfo().remote().address());
        if (cancellationRequested()) {
            co_return RunStatus::kCancelled;
        }
        const auto receiveEnded = co_await waitTunnelReceiveEnd();
        co_return receiveEnded ? RunStatus::kTunnelComplete : RunStatus::kCancelled;
    }
    if (cancellationRequested()) {
        co_return RunStatus::kCancelled;
    }
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (buffered.has_value()) {
        co_return co_await stageResponse(std::move(*buffered));
    }
    co_return RunStatus::kFailed;
}

Http3BufferedRequestDispatch::PublicationDemand
Http3BufferedRequestDispatch::publicationDemand() const noexcept {
    if (!onWorker()) {
        return PublicationDemand::kWrongWorker;
    }
    if (tunnelMode_) {
        if (state_ == State::kCancelled || cancellationRequested()) {
            return PublicationDemand::kLocalCancelled;
        }
        if (tunnelOutputEnded_) {
            return PublicationDemand::kLocalComplete;
        }
        if (outbound_.stopped()) {
            return PublicationDemand::kLocalMailboxStopped;
        }
        if (tunnelHandshakeOffset_ < tunnelHandshakeFrame_.size() || tunnelDataPending_) {
            return PublicationDemand::kData;
        }
        if (tunnelEstablishedPending_ || tunnelFinPending_) {
            return PublicationDemand::kControl;
        }
        return PublicationDemand::kNotReady;
    }
    switch (state_) {
        case State::kComplete:
            return PublicationDemand::kLocalComplete;
        case State::kCancelled:
            return PublicationDemand::kLocalCancelled;
        case State::kPeerLimitRejected:
            return PublicationDemand::kLocalPeerLimitRejected;
        case State::kFailed:
            return PublicationDemand::kLocalFailed;
        case State::kCold:
        case State::kPreparing:
        case State::kPrepared:
        case State::kRunning:
            return PublicationDemand::kNotReady;
        case State::kOutputReady:
        case State::kPublishing:
            break;
    }
    if (cancellationRequested()) {
        return PublicationDemand::kLocalCancelled;
    }
    if (outbound_.stopped()) {
        return PublicationDemand::kLocalMailboxStopped;
    }
    if (!output_) {
        return PublicationDemand::kLocalFailed;
    }
    // The peer limit is advisory: gate only before the first handoff. Once a
    // HEADERS prefix is published it cannot be withdrawn or rewritten.
    if (publishedWireBytes_ == 0 && exceedsPeerFieldSectionLimit()) {
        return PublicationDemand::kLocalPeerLimitRejected;
    }
    switch (output_->nextStep()) {
        case Http3BufferedResponseOutput::NextStep::kBytes:
            return PublicationDemand::kData;
        case Http3BufferedResponseOutput::NextStep::kFin:
            return PublicationDemand::kControl;
        case Http3BufferedResponseOutput::NextStep::kComplete:
        case Http3BufferedResponseOutput::NextStep::kFailed:
            return PublicationDemand::kLocalFailed;
    }
    return PublicationDemand::kLocalFailed;
}

Http3BufferedRequestDispatch::PublishResult Http3BufferedRequestDispatch::publishStep() & noexcept {
    const auto demand = publicationDemand();
    if (tunnelMode_ && (demand == PublicationDemand::kData ||
                           demand == PublicationDemand::kControl)) {
        return publishTunnelStep(demand);
    }
    switch (demand) {
        case PublicationDemand::kWrongWorker:
            return {PublishStatus::kWrongWorker};
        case PublicationDemand::kNotReady:
            return {PublishStatus::kNotReady};
        case PublicationDemand::kLocalComplete:
            return {PublishStatus::kComplete};
        case PublicationDemand::kLocalCancelled:
            cancel();
            return {PublishStatus::kCancelled};
        case PublicationDemand::kLocalMailboxStopped:
            fail();
            return {PublishStatus::kFailed};
        case PublicationDemand::kLocalPeerLimitRejected:
            if (state_ == State::kPeerLimitRejected) {
                return {PublishStatus::kPeerLimitRejected};
            }
            state_ = State::kPublishing;
            (void)outbound_.drainReturns();
            state_ = State::kPeerLimitRejected;
            if (!releaseDispatchStorage()) {
                state_ = State::kFailed;
                return {PublishStatus::kFailed};
            }
            return {PublishStatus::kPeerLimitRejected};
        case PublicationDemand::kLocalFailed:
            if (state_ != State::kFailed) {
                fail();
            }
            return {PublishStatus::kFailed};
        case PublicationDemand::kData:
        case PublicationDemand::kControl:
            break;
    }

    state_ = State::kPublishing;
    const auto publication = output_->publishStep();
    publishedWireBytes_ += publication.bytesAccepted;
    switch (publication.status) {
        case Http3BufferedResponseOutput::Status::kBytes:
            return {PublishStatus::kBytesPublished, publication.bytesAccepted,
                publication.notifyPeer};
        case Http3BufferedResponseOutput::Status::kFin:
            state_ = State::kComplete;
            if (!releaseDispatchStorage()) {
                state_ = State::kFailed;
                return {PublishStatus::kFailed, 0, publication.notifyPeer};
            }
            return {PublishStatus::kFinPublished, 0, publication.notifyPeer};
        case Http3BufferedResponseOutput::Status::kBackpressured:
            return {PublishStatus::kBackpressured, 0, publication.notifyPeer,
                publication.blockReason == Http3BufferedResponseOutput::BlockReason::kData
                    ? PublishBlockReason::kData
                : publication.blockReason == Http3BufferedResponseOutput::BlockReason::kControl
                    ? PublishBlockReason::kControl
                    : PublishBlockReason::kNone};
        case Http3BufferedResponseOutput::Status::kComplete:
            state_ = State::kComplete;
            if (!releaseDispatchStorage()) {
                state_ = State::kFailed;
                return {PublishStatus::kFailed, 0, publication.notifyPeer};
            }
            return {PublishStatus::kComplete, 0, publication.notifyPeer};
        case Http3BufferedResponseOutput::Status::kFailed:
            fail();
            return {PublishStatus::kFailed, publication.bytesAccepted,
                publication.notifyPeer};
    }
    fail();
    return {PublishStatus::kFailed, publication.bytesAccepted, publication.notifyPeer};
}

Http3BufferedRequestDispatch::PublishResult
Http3BufferedRequestDispatch::publishTunnelStep(PublicationDemand demand) noexcept {
    if (!onWorker()) {
        return {PublishStatus::kWrongWorker};
    }
    if (demand == PublicationDemand::kLocalCancelled) {
        cancel();
        return {PublishStatus::kCancelled};
    }
    if (demand == PublicationDemand::kLocalMailboxStopped) {
        cancel();
        return {PublishStatus::kCancelled};
    }
    if (demand == PublicationDemand::kNotReady) {
        return {PublishStatus::kNotReady};
    }

    bool notifyPeer = false;
    if (demand == PublicationDemand::kData) {
        std::span<const std::byte> bytes;
        if (tunnelHandshakeOffset_ < tunnelHandshakeFrame_.size()) {
            const auto count = (std::min)(Http3StreamMailbox::kMaxBlockBytes,
                tunnelHandshakeFrame_.size() - tunnelHandshakeOffset_);
            bytes = std::as_bytes(std::span<const char>(
                tunnelHandshakeFrame_.data() + tunnelHandshakeOffset_, count));
        } else if (tunnelDataPending_) {
            bytes = std::as_bytes(std::span<const char>(
                tunnelDataFrame_.data(), tunnelDataFrame_.size()));
        } else {
            return {PublishStatus::kNotReady};
        }
        const auto result = outbound_.trySend(messageId_, bytes);
        if (result == Http3StreamMailbox::SendResult::kFull) {
            return {PublishStatus::kBackpressured, 0, false, PublishBlockReason::kData};
        }
        if (result != Http3StreamMailbox::SendResult::kSent &&
            result != Http3StreamMailbox::SendResult::kSentNotifyPeer) {
            return {PublishStatus::kFailed};
        }
        notifyPeer = result == Http3StreamMailbox::SendResult::kSentNotifyPeer;
        tunnelPublishedWireBytes_ += bytes.size();
        publishedWireBytes_ += bytes.size();
        if (tunnelHandshakeOffset_ < tunnelHandshakeFrame_.size()) {
            tunnelHandshakeOffset_ += bytes.size();
            if (tunnelHandshakeOffset_ == tunnelHandshakeFrame_.size()) {
                tunnelOutputAvailable_.notify();
            }
        } else {
            tunnelDataPending_ = false;
            tunnelDataFrame_.clear();
            tunnelOutputAvailable_.notify();
        }
        return {PublishStatus::kBytesPublished, bytes.size(), notifyPeer};
    }

    const bool establishingTunnel = tunnelEstablishedPending_;
    Http3StreamControl event{
        .kind = establishingTunnel ? Http3StreamControl::Kind::kTunnelEstablished
                                   : Http3StreamControl::Kind::kStreamFin,
        .id = messageId_,
        .value = tunnelPublishedWireBytes_};
    const auto result = outbound_.trySendControl(event);
    if (result == Http3StreamMailbox::ControlResult::kFull) {
        return {PublishStatus::kBackpressured, 0, false, PublishBlockReason::kControl};
    }
    if (result != Http3StreamMailbox::ControlResult::kSent &&
        result != Http3StreamMailbox::ControlResult::kSentNotifyPeer) {
        return {PublishStatus::kFailed};
    }
    notifyPeer = result == Http3StreamMailbox::ControlResult::kSentNotifyPeer;
    if (establishingTunnel) {
        tunnelEstablishedPending_ = false;
        tunnelEstablishedPublished_ = true;
        tunnelOutputAvailable_.notify();
        return {PublishStatus::kControlPublished, 0, notifyPeer};
    }
    tunnelFinPending_ = false;
    tunnelOutputEnded_ = true;
    armPeerTransportFinTimeout();
    tunnelOutputAvailable_.notify();
    return {PublishStatus::kFinPublished, 0, notifyPeer};
}

bool Http3BufferedRequestDispatch::registerPublicationDeadlineCallback(
    MoveOnlyFunction<void()> callback) & {
    if (!onWorker()) {
        throw std::logic_error("HTTP/3 publication deadline registration must run on its worker");
    }
    if (state_ != State::kOutputReady || !callback) {
        throw std::logic_error("HTTP/3 publication deadline registration requires ready output");
    }
    if (!deadlineArmed_) {
        return false;
    }
    if (publicationDeadlineCallbackRegistered_ || !requestDeadline_) {
        throw std::logic_error("HTTP/3 publication deadline callback is already registered");
    }
    publicationDeadlineCallbackRegistered_ = true;
    requestDeadline_->token().registerCallback(
        publicationDeadlineRegistration_, std::move(callback));
    return true;
}

Http3BufferedRequestDispatch::CancellationReason
Http3BufferedRequestDispatch::cancellationReason() const noexcept {
    latchCancellationReason();
    return cancellationReason_;
}

void Http3BufferedRequestDispatch::cancel() & noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    if (state_ == State::kComplete || state_ == State::kFailed ||
        state_ == State::kCancelled || state_ == State::kPeerLimitRejected) {
        return;
    }
    // Set the terminal state first: stop callbacks may resume a suspended handler.
    disarmPeerTransportFinTimeout();
    cancellationRequested_ = true;
    latchCancellationReason();
    state_ = State::kCancelled;
    tunnelAborted_ = true;
    requestStopSource_.requestStop();
    tunnelInputAvailable_.notify();
    tunnelOutputAvailable_.notify();
    if (!handlerActive_) {
        (void)releaseDispatchStorage();
    }
}

Task<std::error_code> Http3BufferedRequestDispatch::publishTunnelHandshake(
    std::span<const char> headersFrame) {
    if (!onWorker() || headersFrame.empty() || tunnelMode_ ||
        tunnelCallbacks_.outputReady == nullptr) {
        co_return std::make_error_code(std::errc::invalid_argument);
    }
    try {
        tunnelHandshakeFrame_.assign(headersFrame.data(), headersFrame.size());
    } catch (...) {
        co_return std::make_error_code(std::errc::not_enough_memory);
    }
    tunnelMode_ = true;
    tunnelHandshakeOffset_ = 0;
    notifyTunnelOutput();
    while (tunnelHandshakeOffset_ < tunnelHandshakeFrame_.size()) {
        if (cancellationRequested() || tunnelAborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        co_await tunnelOutputAvailable_.wait();
    }
    if (cancellationRequested() || tunnelAborted_) {
        co_return std::make_error_code(std::errc::operation_canceled);
    }
    tunnelEstablishedPending_ = true;
    notifyTunnelOutput();
    while (!tunnelEstablishedPublished_) {
        if (cancellationRequested() || tunnelAborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        co_await tunnelOutputAvailable_.wait();
    }
    co_return std::error_code{};
}

void Http3BufferedRequestDispatch::notifyTunnelInput() noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    tunnelInputAvailable_.notify();
}

Task<WsTransportReadResult> Http3BufferedRequestDispatch::readTunnel(
    std::pmr::string& buffer) {
    if (!onWorker()) {
        co_return WsTransportReadResult::makeFailure(
            std::make_error_code(std::errc::operation_not_permitted));
    }
    for (;;) {
        if (cancellationRequested() || tunnelAborted_) {
            co_return WsTransportReadResult::makeFailure(
                std::make_error_code(std::errc::operation_canceled));
        }
        std::array<char, 4096> bytes{};
        const auto result = session_.readTunnelData(messageId_.streamId, bytes);
        if (result.reset || result.ended) {
            disarmPeerTransportFinTimeout();
        }
        if (result.overflow) {
            abortTunnel();
            co_return WsTransportReadResult::makeFailure(
                std::make_error_code(std::errc::no_buffer_space));
        }
        if (result.reset) {
            co_return WsTransportReadResult::makeFailure(
                std::make_error_code(std::errc::connection_reset));
        }
        if (result.bytes != 0) {
            buffer.append(bytes.data(), result.bytes);
            co_return WsTransportReadResult::makeData();
        }
        if (result.ended) {
            co_return WsTransportReadResult::makeEnd();
        }
        co_await tunnelInputAvailable_.wait();
    }
}

Task<std::error_code> Http3BufferedRequestDispatch::writeTunnel(
    std::string_view bytes, WebSocketServerTransportDisposition disposition) {
    if (!onWorker() || !tunnelMode_ || tunnelOutputEnded_) {
        co_return std::make_error_code(std::errc::operation_not_permitted);
    }
    constexpr auto maxFrameHeader = kHttp3FrameHeaderMaxBytes;
    constexpr auto maxPayload = Http3StreamMailbox::kMaxBlockBytes - maxFrameHeader;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (cancellationRequested() || tunnelAborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        if (tunnelDataPending_ || tunnelFinPending_) {
            co_return std::make_error_code(std::errc::operation_not_permitted);
        }
        const auto count = (std::min)(maxPayload, bytes.size() - offset);
        std::array<char, maxFrameHeader> header{};
        const auto encoded = encodeHttp3FrameHeader(header,
            static_cast<std::uint64_t>(Http3FrameType::kData), count);
        if (!encoded) {
            abortTunnel();
            co_return std::make_error_code(std::errc::message_size);
        }
        try {
            tunnelDataFrame_.assign(header.data(), *encoded);
            tunnelDataFrame_.append(bytes.data() + offset, count);
        } catch (...) {
            abortTunnel();
            co_return std::make_error_code(std::errc::not_enough_memory);
        }
        tunnelDataPending_ = true;
        notifyTunnelOutput();
        while (tunnelDataPending_) {
            if (cancellationRequested() || tunnelAborted_) {
                co_return std::make_error_code(std::errc::operation_canceled);
            }
            co_await tunnelOutputAvailable_.wait();
        }
        offset += count;
    }
    if (disposition == WebSocketServerTransportDisposition::kEndTransport) {
        tunnelFinPending_ = true;
        notifyTunnelOutput();
        while (!tunnelOutputEnded_) {
            if (cancellationRequested() || tunnelAborted_) {
                co_return std::make_error_code(std::errc::operation_canceled);
            }
            co_await tunnelOutputAvailable_.wait();
        }
    }
    co_return std::error_code{};
}

Task<bool> Http3BufferedRequestDispatch::waitTunnelReceiveEnd() {
    if (!onWorker()) {
        co_return false;
    }
    for (;;) {
        if (cancellationRequested() || tunnelAborted_) {
            co_return false;
        }
        const auto result = session_.readTunnelData(messageId_.streamId, {});
        if (result.reset || result.overflow) {
            disarmPeerTransportFinTimeout();
            co_return false;
        }
        if (result.ended) {
            disarmPeerTransportFinTimeout();
            co_return true;
        }
        co_await tunnelInputAvailable_.wait();
    }
}

void Http3BufferedRequestDispatch::peerTransportFinTimeoutTick(
    void* target, std::int64_t nowMs) noexcept {
    auto& dispatch = *static_cast<Http3BufferedRequestDispatch*>(target);
    if (!dispatch.onWorker()) {
        std::terminate();
    }
    const auto peerState = dispatch.session_.readTunnelData(dispatch.messageId_.streamId, {});
    if (peerState.ended || peerState.reset || peerState.overflow) {
        dispatch.disarmPeerTransportFinTimeout();
        return;
    }
    if (nowMs >= dispatch.peerTransportFinDeadlineMs_) {
        dispatch.abortTunnel();
    }
}

void Http3BufferedRequestDispatch::armPeerTransportFinTimeout() noexcept {
    if (!onWorker() || peerFinTimeoutArmed_ ||
        peerTransportFinTimeout_ <= std::chrono::milliseconds::zero()) {
        return;
    }
    const auto peerState = session_.readTunnelData(messageId_.streamId, {});
    if (peerState.ended || peerState.reset || peerState.overflow) {
        return;
    }
    const auto now = steadyNowMs();
    const auto timeout = peerTransportFinTimeout_.count();
    peerTransportFinDeadlineMs_ = timeout > (std::numeric_limits<std::int64_t>::max)() - now
                                      ? (std::numeric_limits<std::int64_t>::max)()
                                      : now + timeout;
    scannerEntry_.registerPeriodicCheck(
        peerTransportFinCheck_, this, &Http3BufferedRequestDispatch::peerTransportFinTimeoutTick);
    peerFinTimeoutArmed_ = true;
}

void Http3BufferedRequestDispatch::disarmPeerTransportFinTimeout() noexcept {
    peerTransportFinCheck_.reset();
    peerFinTimeoutArmed_ = false;
    peerTransportFinDeadlineMs_ = 0;
}

void Http3BufferedRequestDispatch::abortTunnel() noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    if (tunnelAborted_) {
        return;
    }
    disarmPeerTransportFinTimeout();
    tunnelAborted_ = true;
    tunnelInputAvailable_.notify();
    tunnelOutputAvailable_.notify();
    if (tunnelCallbacks_.abort != nullptr) {
        tunnelCallbacks_.abort(tunnelCallbacks_.context, messageId_.streamId);
    }
}

void Http3BufferedRequestDispatch::notifyTunnelOutput() noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    if (tunnelCallbacks_.outputReady != nullptr) {
        tunnelCallbacks_.outputReady(tunnelCallbacks_.context, messageId_.streamId);
    }
}

bool Http3BufferedRequestDispatch::handlerActive() const noexcept {
    return handlerActive_;
}

bool Http3BufferedRequestDispatch::responseReady() const noexcept {
    return state_ == State::kOutputReady || state_ == State::kPublishing;
}

bool Http3BufferedRequestDispatch::complete() const noexcept {
    return state_ == State::kComplete;
}

std::uint64_t Http3BufferedRequestDispatch::publishedWireBytes() const noexcept {
    return publishedWireBytes_;
}

std::exception_ptr Http3BufferedRequestDispatch::failure() const noexcept {
    return failure_;
}

bool Http3BufferedRequestDispatch::onWorker() const noexcept {
    return services_.worker().isCurrent();
}

bool Http3BufferedRequestDispatch::cancellationRequested() const noexcept {
    latchCancellationReason();
    return cancellationReason_ != CancellationReason::kNone ||
           (requestDeadline_ && requestDeadline_->token().stopRequested());
}

void Http3BufferedRequestDispatch::latchCancellationReason() const noexcept {
    if (cancellationReason_ != CancellationReason::kNone) {
        return;
    }
    if (services_.stopToken().stopRequested()) {
        cancellationReason_ = CancellationReason::kWorkerStop;
    } else if (cancellationRequested_ || requestStopSource_.stopRequested()) {
        cancellationReason_ = CancellationReason::kExplicit;
    } else if (requestDeadline_ && requestDeadline_->token().stopRequested()) {
        cancellationReason_ = requestDeadline_->exceeded()
                                  ? CancellationReason::kDeadline
                                  : CancellationReason::kWorkerStop;
    }
}

bool Http3BufferedRequestDispatch::exceedsPeerFieldSectionLimit() const noexcept {
    const auto limit = session_.peerMaxFieldSectionSize();
    return limit.has_value() && output_ &&
           std::cmp_greater(output_->decodedFieldSectionSize(), *limit);
}

void Http3BufferedRequestDispatch::fail(std::exception_ptr failure) noexcept {
    disarmPeerTransportFinTimeout();
    if (handlerActive_) {
        std::terminate();
    }
    if (failure != nullptr) {
        failure_ = std::move(failure);
    }
    state_ = State::kFailed;
    (void)releaseDispatchStorage();
}

bool Http3BufferedRequestDispatch::releaseDispatchStorage() noexcept {
    if (handlerActive_) {
        std::terminate();
    }
    publicationDeadlineRegistration_.reset();
    tunnelStopRegistration_.reset();
    latchCancellationReason();
    requestServices_.reset();
    requestDeadline_.reset();
    combinedWorkerAndRequestStop_ = StopToken{};
    output_.reset();
    response_.reset();
    requestMemory_.reset();

    const bool hadLease = lease_.has_value();
    lease_.reset();
    if (!hadLease) {
        return true;
    }
    if (session_.release(messageId_.streamId)) {
        return true;
    }
    // A reset/stop retired the session entry while the lease pinned it; its
    // destructor performs that cleanup instead of the normal release() path.
    return session_.request(messageId_.streamId) == nullptr;
}

}  // namespace ruvia::detail
