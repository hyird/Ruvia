#include "ruvia/web/detail/http3/Http3BufferedRequestDispatch.h"

#include <chrono>
#include <cstddef>
#include <exception>
#include <optional>
#include <utility>

#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/response/HttpBufferedResponse.h"

namespace ruvia::detail {

Http3BufferedRequestDispatch::Http3BufferedRequestDispatch(
    Http3SansIoSessionEngine& session, const RouteTable& routes, WorkerMemory& worker,
    ContextServices services, const HttpServerOptions& options,
    Http3StreamMailbox& outbound, Http3StreamMessageId messageId)
    : session_(session),
      routes_(routes),
      worker_(worker),
      services_(std::move(services)),
      options_(options),
      outbound_(outbound),
      messageId_(messageId) {}

Http3BufferedRequestDispatch::~Http3BufferedRequestDispatch() {
    if (handlerActive_) {
        std::terminate();
    }
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

Http3BufferedRequestDispatch::PublicationDemand
Http3BufferedRequestDispatch::publicationDemand() const noexcept {
    if (!onWorker()) {
        return PublicationDemand::kWrongWorker;
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
    cancellationRequested_ = true;
    latchCancellationReason();
    state_ = State::kCancelled;
    requestStopSource_.requestStop();
    if (!handlerActive_) {
        (void)releaseDispatchStorage();
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
