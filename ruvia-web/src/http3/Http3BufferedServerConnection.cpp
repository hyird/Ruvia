#include "ruvia/web/detail/http3/Http3BufferedServerConnection.h"

#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::uint64_t mixStreamId(std::uint64_t streamId) noexcept {
    streamId ^= streamId >> 30;
    streamId *= 0xbf58476d1ce4e5b9ULL;
    streamId ^= streamId >> 27;
    streamId *= 0x94d049bb133111ebULL;
    streamId ^= streamId >> 31;
    return streamId;
}

[[nodiscard]] HttpStatusCode rejectionStatus(
    Http3SansIoSessionEngine::Rejection rejection) {
    using Rejection = Http3SansIoSessionEngine::Rejection;
    switch (rejection) {
        case Rejection::kExpectationUnsupported:
            return http_status::kExpectationFailed;
        case Rejection::kBodyTooLarge:
            return http_status::kContentTooLarge;
        case Rejection::kConnectUnsupported:
        case Rejection::kStreamingUnsupported:
        case Rejection::kWebSocketUnsupported:
        case Rejection::kResponseStreamUnsupported:
            return http_status::kNotImplemented;
        case Rejection::kInFlightBodyCapacity:
        case Rejection::kWorkerBodyBudgetExhausted:
            return http_status::kServiceUnavailable;
        case Rejection::kNone:
            break;
    }
    throw std::invalid_argument("HTTP/3 rejection requires a status mapping");
}

}  // namespace

struct Http3BufferedServerConnection::RequestEntry final {
    RequestEntry(Http3BufferedServerConnection& ownerValue, RequestIndexSlot& slotValue,
        std::uint64_t streamIdValue)
        : slot(slotValue),
          streamId(streamIdValue),
          dispatch(ownerValue.session_, ownerValue.routes_, ownerValue.worker_,
              ownerValue.services_, ownerValue.options_, ownerValue.outbound_,
              {ownerValue.epoch_, ownerValue.connectionGeneration_, streamId}),
          publicationFinished(ownerValue.services_.worker()) {}

    RequestIndexSlot& slot;
    const std::uint64_t streamId;
    Dispatch dispatch;
    WorkerSignal publicationFinished;
    bool outputEnabled{true};
    bool outputTerminal{};
    bool retirementGranted{};
    bool cancelRequested{};
    bool deadlineActivationQueued{};
};

struct Http3BufferedServerConnection::RejectionEntry final {
    RejectionEntry(Http3BufferedServerConnection& ownerValue, RequestIndexSlot& slotValue,
        std::uint64_t streamIdValue, HttpKnownMethod method, Session::Rejection rejection)
        : slot(slotValue),
          streamId(streamIdValue),
          response(std::in_place, HttpResponse::Options{.resource = ownerValue.worker_.resource()}) {
        response->status(rejectionStatus(rejection));
        response->header("content-length", "0");
        const auto writePlan = planBufferedHttpResponseWrite(method, *response);
        auto created = Http3BufferedResponseOutput::create(*response, writePlan,
            ownerValue.worker_, ownerValue.outbound_,
            {ownerValue.epoch_, ownerValue.connectionGeneration_, streamId}, std::nullopt);
        if (!created) {
            throw std::runtime_error("HTTP/3 rejection response preparation failed");
        }
        output.emplace(std::move(*created));
    }

    RequestIndexSlot& slot;
    const std::uint64_t streamId;
    std::optional<HttpResponse> response;
    std::optional<Http3BufferedResponseOutput> output;
    bool outputEnabled{true};
    bool outputTerminal{};
    bool receiveTerminal{};
};

struct Http3BufferedServerConnection::EntryRetirement final {
    Http3BufferedServerConnection& owner;
    RequestEntry& entry;

    ~EntryRetirement() {
        if (!entry.outputTerminal) {
            owner.removeQueued(entry.slot);
            entry.outputEnabled = false;
            entry.cancelRequested = true;
            entry.retirementGranted = true;
            entry.outputTerminal = true;
            entry.slot.status = RequestStatus::kFailed;
            entry.dispatch.cancel();
            entry.publicationFinished.notify();
            owner.requireConnectionClose(TransportCloseReason::kEntryRetirementFailure,
                Http3ConnectionErrorCode::kInternalError);
        }
        owner.finishEntry(entry);
    }
};

struct Http3BufferedServerConnection::RequestRetirement final {
    Http3BufferedServerConnection& owner;
    std::uint64_t streamId;

    ~RequestRetirement() {
        if (owner.session_.request(streamId) == nullptr) {
            return;
        }
        (void)owner.session_.release(streamId);
        owner.requireConnectionClose(TransportCloseReason::kRequestRetirementFailure,
            Http3ConnectionErrorCode::kInternalError);
    }
};

Http3BufferedServerConnection::Http3BufferedServerConnection(
    const RouteTable& routes, WorkerMemory& worker, ContextServices services,
    const HttpServerOptions& options, Http3StreamMailbox& outbound,
    ActivationRef activation, Http3BufferedServerConnectionConfig config)
    : services_(services.withRoutes(routes)),
      worker_(worker),
      routes_(routes),
      options_(options),
      outbound_(outbound),
      activation_(activation),
      epoch_(config.epoch),
      connectionGeneration_(config.connectionGeneration),
      maxTrackedStreams_(config.maxTrackedStreams),
      session_(routes_, worker_, config.session),
      input_(session_, worker_, epoch_, connectionGeneration_, maxTrackedStreams_),
      tasks_(services_.worker(), {.resource = worker_.resource()}),
      requestIndex_(worker_.resource()) {
    initialize();
}

Http3BufferedServerConnection::Http3BufferedServerConnection(
    const RouteTable& routes, WorkerMemory& worker, ContextServices services,
    const HttpServerOptions& options, Http3StreamMailbox& outbound,
    ActivationRef activation, Http3ServerBodyBudget& bodyBudget,
    Http3BufferedServerConnectionConfig config)
    : services_(services.withRoutes(routes)),
      worker_(worker),
      routes_(routes),
      options_(options),
      outbound_(outbound),
      activation_(activation),
      epoch_(config.epoch),
      connectionGeneration_(config.connectionGeneration),
      maxTrackedStreams_(config.maxTrackedStreams),
      session_(routes_, worker_, bodyBudget, config.session),
      input_(session_, worker_, epoch_, connectionGeneration_, maxTrackedStreams_),
      tasks_(services_.worker(), {.resource = worker_.resource()}),
      requestIndex_(worker_.resource()) {
    initialize();
}

void Http3BufferedServerConnection::initialize() {
    if (!activation_.valid()) {
        throw std::invalid_argument("HTTP/3 connection requires a typed worker activation");
    }
    requestIndex_.resize(indexCapacity(maxTrackedStreams_));
}

Http3BufferedServerConnection::~Http3BufferedServerConnection() {
    if (dataRunnable_.head != nullptr || dataRunnable_.tail != nullptr ||
        controlRunnable_.head != nullptr || controlRunnable_.tail != nullptr ||
        localRunnable_.head != nullptr || localRunnable_.tail != nullptr ||
        dataBlocked_.head != nullptr || dataBlocked_.tail != nullptr ||
        controlBlocked_.head != nullptr || controlBlocked_.tail != nullptr ||
        readyRequestCount_ != 0 || blockedRequestCount_ != 0 || activeRequestCount_ != 0 ||
        activeRejectionCount_ != 0 || tasks_.size() != 0 || session_.activeStreamCount() != 0 ||
        pendingResetIntentCount_ != 0 || closeIntentPending_ ||
        (!transportRetired_ && !transportRetirementTakenOver_ && !closeIntentHandedOff_) ||
        (everSpawned_ && !joinCompleted_)) {
        std::terminate();
    }
}

Http3BufferedServerConnection::EventResult Http3BufferedServerConnection::acceptData(
    const Http3StreamMailbox::BorrowedBlock& block) & {
    if (!onWorker()) {
        return {.status = EventStatus::kWrongWorker};
    }
    if (admissionClosed_) {
        return {.status = EventStatus::kAdmissionClosed,
            .input = {Input::Status::kStopped},
            .connectionCloseRequired = transportCloseRequired_};
    }
    const auto streamId = block.id().streamId;
    return handleInputResult(streamId, input_.acceptData(block), false);
}

Http3BufferedServerConnection::EventResult Http3BufferedServerConnection::acceptControl(
    const Http3StreamControl& control) & {
    if (!onWorker()) {
        return {.status = EventStatus::kWrongWorker};
    }
    if (control.kind == Http3StreamControl::Kind::kConnectionClosed &&
        control.id.epoch == epoch_ &&
        control.id.connectionGeneration == connectionGeneration_ && admissionClosed_) {
        (void)confirmTransportRetired(
            {.epoch = epoch_, .connectionGeneration = connectionGeneration_});
        return {.status = EventStatus::kConnectionClosed,
            .input = {Input::Status::kConnectionClosed}};
    }
    if (admissionClosed_) {
        return {.status = EventStatus::kAdmissionClosed,
            .input = {Input::Status::kStopped},
            .connectionCloseRequired = transportCloseRequired_};
    }

    // Suppress this stream's output before recording RESET in the input tombstone.
    // Identity mismatches must not affect a current request with the same stream ID.
    RequestEntry* resetEntry = nullptr;
    RejectionEntry* resetRejection = nullptr;
    if (control.kind == Http3StreamControl::Kind::kStreamReset &&
        control.id.epoch == epoch_ &&
        control.id.connectionGeneration == connectionGeneration_) {
        auto* slot = findRequestSlot(control.id.streamId);
        if (slot != nullptr && slot->entry != nullptr && !slot->entry->outputTerminal &&
            !slot->entry->cancelRequested) {
            resetEntry = slot->entry;
            resetEntry->outputEnabled = false;
            resetEntry->cancelRequested = true;
            removeQueued(resetEntry->slot);
        } else if (slot != nullptr && slot->rejectionEntry != nullptr &&
                   !slot->rejectionEntry->outputTerminal) {
            resetRejection = slot->rejectionEntry;
            resetRejection->outputEnabled = false;
            removeQueued(resetRejection->slot);
        }
    }

    auto result = input_.acceptControl(control);
    if (resetEntry != nullptr && result.status != Input::Status::kReset &&
        result.status != Input::Status::kDeferredReset &&
        result.status != Input::Status::kProtocolError && result.status != Input::Status::kStopped &&
        result.status != Input::Status::kConnectionClosed &&
        result.status != Input::Status::kCapacityExhausted &&
        result.status != Input::Status::kFinalSizeError) {
        // The input declined this control without establishing a terminal
        // tombstone. Restore a still-live response rather than silently dropping it.
        resetEntry->cancelRequested = false;
        resetEntry->outputEnabled = true;
        if (resetEntry->dispatch.responseReady()) {
            enqueueForDemand(resetEntry->slot, true);
        }
    } else if (resetRejection != nullptr && result.status != Input::Status::kReset &&
               result.status != Input::Status::kDeferredReset &&
               result.status != Input::Status::kProtocolError &&
               result.status != Input::Status::kStopped &&
               result.status != Input::Status::kConnectionClosed &&
               result.status != Input::Status::kCapacityExhausted &&
               result.status != Input::Status::kFinalSizeError) {
        resetRejection->outputEnabled = true;
        enqueueForDemand(resetRejection->slot, true);
    }
    return handleInputResult(control.id.streamId, result,
        control.kind == Http3StreamControl::Kind::kStreamReset);
}

Http3BufferedServerConnection::WorkState
Http3BufferedServerConnection::workState() const noexcept {
    if (!onWorker()) {
        return {.wrongWorker = true};
    }
    return {.runnable = {.data = dataRunnable_.head != nullptr,
                .control = controlRunnable_.head != nullptr,
                .local = localRunnable_.head != nullptr},
        .blocked = {.data = dataBlocked_.head != nullptr,
            .control = controlBlocked_.head != nullptr},
        .runnableCount = readyRequestCount_,
        .blockedCount = blockedRequestCount_};
}

std::size_t Http3BufferedServerConnection::reactivateBlocked(WorkLanes lanes) & noexcept {
    if (!onWorker()) {
        return 0;
    }
    std::size_t reactivated = 0;
    const auto reactivate = [this, &reactivated](IntrusiveQueue& blocked, QueueKind runnable) {
        while (blocked.head != nullptr) {
            auto& slot = *blocked.head;
            removeQueued(slot);
            enqueueRunnable(slot, runnable, false);
            ++reactivated;
        }
    };
    if (lanes.data) {
        reactivate(dataBlocked_, QueueKind::kDataRunnable);
    }
    if (lanes.control) {
        reactivate(controlBlocked_, QueueKind::kControlRunnable);
    }
    if (reactivated != 0) {
        notifyActivation();
    }
    return reactivated;
}

bool Http3BufferedServerConnection::reactivateBlockedOne(WorkLane lane) & noexcept {
    if (!onWorker()) {
        return false;
    }
    IntrusiveQueue* blocked = nullptr;
    QueueKind runnable = QueueKind::kNone;
    switch (lane) {
        case WorkLane::kData:
            blocked = &dataBlocked_;
            runnable = QueueKind::kDataRunnable;
            break;
        case WorkLane::kControl:
            blocked = &controlBlocked_;
            runnable = QueueKind::kControlRunnable;
            break;
        case WorkLane::kLocal:
            return false;
    }
    if (blocked->head == nullptr) {
        return false;
    }
    auto& slot = *blocked->head;
    removeQueued(slot);
    enqueueRunnable(slot, runnable, false);
    if (slot.queue != runnable) {
        return false;
    }
    return true;
}

Http3BufferedServerConnection::PublishAttempt
Http3BufferedServerConnection::publishOne(WorkLanes eligibleLanes) & noexcept {
    if (!onWorker()) {
        return {.status = PublishStatus::kWrongWorker};
    }
    auto* slot = selectRunnable(eligibleLanes);
    if (slot == nullptr) {
        return {};
    }
    auto* entry = slot->entry;
    auto* rejectionEntry = slot->rejectionEntry;
    if ((entry == nullptr) == (rejectionEntry == nullptr)) {
        std::terminate();
    }

    removeQueued(*slot);
    switch (nextPublishLane_) {
        case WorkLane::kData:
            nextPublishLane_ = WorkLane::kControl;
            break;
        case WorkLane::kControl:
            nextPublishLane_ = WorkLane::kLocal;
            break;
        case WorkLane::kLocal:
            nextPublishLane_ = WorkLane::kData;
            break;
    }
    if (rejectionEntry != nullptr) {
        return publishRejection(*slot, *rejectionEntry);
    }
    entry->slot.status = RequestStatus::kPublishing;
    const auto result = entry->dispatch.publishStep();
    switch (result.status) {
        case Dispatch::PublishStatus::kBytesPublished:
            if (entry->outputEnabled && !entry->cancelRequested && !stopRequested_) {
                entry->slot.status = RequestStatus::kPublishing;
                enqueueForDemand(entry->slot, false);
            } else {
                cancelEntry(*entry, RequestStatus::kCancelled);
            }
            break;
        case Dispatch::PublishStatus::kBackpressured:
            if (entry->outputEnabled && !entry->cancelRequested && !stopRequested_) {
                entry->slot.status = RequestStatus::kReadyToPublish;
                enqueueBlocked(entry->slot, result.blockReason);
            } else {
                cancelEntry(*entry, RequestStatus::kCancelled);
            }
            break;
        case Dispatch::PublishStatus::kNotReady:
            removeQueued(entry->slot);
            entry->outputTerminal = true;
            entry->slot.status = RequestStatus::kFailed;
            entry->publicationFinished.notify();
            requireConnectionClose(TransportCloseReason::kUnexpectedRequestState,
                Http3ConnectionErrorCode::kInternalError);
            break;
        case Dispatch::PublishStatus::kFinPublished:
        case Dispatch::PublishStatus::kComplete:
            removeQueued(entry->slot);
            entry->outputTerminal = true;
            entry->slot.status = RequestStatus::kPublished;
            entry->publicationFinished.notify();
            break;
        case Dispatch::PublishStatus::kCancelled: {
            const auto reason = entry->dispatch.cancellationReason();
            const bool alreadyCancelledByOwner = entry->cancelRequested;
            removeQueued(entry->slot);
            if (reason == Dispatch::CancellationReason::kDeadline) {
                (void)enqueueResetIntent(entry->slot);
            }
            entry->outputEnabled = false;
            entry->cancelRequested = true;
            entry->outputTerminal = true;
            entry->slot.status = RequestStatus::kCancelled;
            entry->publicationFinished.notify();
            if (reason == Dispatch::CancellationReason::kWorkerStop && !stopRequested_) {
                (void)requestStop();
            } else if (!stopRequested_ && !alreadyCancelledByOwner &&
                       reason != Dispatch::CancellationReason::kDeadline) {
                requireConnectionClose(TransportCloseReason::kPublishCancellation,
                    Http3ConnectionErrorCode::kInternalError);
            }
            break;
        }
        case Dispatch::PublishStatus::kPeerLimitRejected:
            removeQueued(entry->slot);
            (void)enqueueResetIntent(entry->slot);
            entry->outputTerminal = true;
            entry->slot.status = RequestStatus::kFailed;
            entry->publicationFinished.notify();
            break;
        case Dispatch::PublishStatus::kFailed:
        case Dispatch::PublishStatus::kWrongWorker:
            removeQueued(entry->slot);
            entry->outputTerminal = true;
            entry->slot.status = RequestStatus::kFailed;
            entry->publicationFinished.notify();
            requireConnectionClose(TransportCloseReason::kPublishFailure,
                Http3ConnectionErrorCode::kInternalError);
            break;
    }
    return {.status = PublishStatus::kAttempted,
        .streamId = entry->streamId,
        .publication = result};
}

bool Http3BufferedServerConnection::requestStop() & noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    if (stopRequested_) {
        return false;
    }
    requireConnectionClose(TransportCloseReason::kLocalStop,
        Http3ConnectionErrorCode::kNoError);
    return true;
}

Task<void> Http3BufferedServerConnection::join() & {
    if (!onWorker()) {
        throw std::logic_error("HTTP/3 connection join must run on its worker");
    }
    if (!admissionClosed_) {
        throw std::logic_error("HTTP/3 connection must stop before joining");
    }
    if (joinStarted_) {
        throw std::logic_error("HTTP/3 connection can only be joined once");
    }
    joinStarted_ = true;
    try {
        co_await tasks_.join();
    } catch (...) {
        joinCompleted_ = true;
        throw;
    }
    joinCompleted_ = true;
    if (activeRequestCount_ != 0 || activeRejectionCount_ != 0 ||
        readyRequestCount_ != 0 || blockedRequestCount_ != 0 ||
        session_.activeStreamCount() != 0) {
        std::terminate();
    }
}

Http3BufferedServerConnection::RequestInfo Http3BufferedServerConnection::requestInfo(
    std::uint64_t streamId) const noexcept {
    const auto* slot = findRequestSlot(streamId);
    if (slot == nullptr) {
        return {};
    }
    return {slot->status, slot->rejection, slot->runStatus};
}

std::size_t Http3BufferedServerConnection::activeRequestCount() const noexcept {
    return activeRequestCount_;
}

std::size_t Http3BufferedServerConnection::activeRejectionCount() const noexcept {
    return activeRejectionCount_;
}

std::size_t Http3BufferedServerConnection::activeTaskCount() const noexcept {
    return tasks_.size();
}

std::size_t Http3BufferedServerConnection::readyRequestCount() const noexcept {
    return readyRequestCount_;
}

std::size_t Http3BufferedServerConnection::trackedRequestCount() const noexcept {
    return trackedRequestCount_;
}

std::size_t Http3BufferedServerConnection::activeSessionStreamCount() const noexcept {
    return session_.activeStreamCount();
}

bool Http3BufferedServerConnection::drainReady(
    std::size_t expectedAdmittedRequests) const noexcept {
    if (!onWorker() || admissionClosed_ || stopRequested_ || transportCloseRequired_ ||
        input_.stopped() || input_.observedRequestStreamCount() != expectedAdmittedRequests ||
        input_.activeRequestStreamCount() != 0 || activeRejectionCount_ != 0 ||
        readyRequestCount_ != 0 ||
        blockedRequestCount_ != 0 || dataRunnable_.head != nullptr ||
        controlRunnable_.head != nullptr || localRunnable_.head != nullptr ||
        dataBlocked_.head != nullptr || controlBlocked_.head != nullptr ||
        session_.activeStreamCount() != 0 || pendingTransportIntentCount() != 0 ||
        closeIntentPending_ || closeIntentHandedOff_) {
        return false;
    }
    for (const auto& slot : requestIndex_) {
        if ((slot.entry != nullptr &&
                (!slot.entry->outputTerminal || slot.entry->dispatch.handlerActive())) ||
            slot.rejectionEntry != nullptr || slot.resetIntentPending) {
            return false;
        }
    }
    return true;
}

bool Http3BufferedServerConnection::stopped() const noexcept {
    return admissionClosed_;
}

bool Http3BufferedServerConnection::transportCloseRequired() const noexcept {
    return transportCloseRequired_;
}

std::optional<Http3BufferedServerConnection::TransportIntent>
Http3BufferedServerConnection::peekTransportIntent() const noexcept {
    if (closeIntentPending_) {
        return TransportIntent{
            .token = {.kind = TransportIntentKind::kConnectionClose,
                .id = {epoch_, connectionGeneration_, 0},
                .sequence = kReservedCloseIntentSequence},
            .closeReason = closeIntentReason_,
            .connectionErrorCode = closeIntentErrorCode_};
    }
    if (resetIntentHead_ == kNoIntentSlot) {
        return std::nullopt;
    }
    if (resetIntentHead_ >= requestIndex_.size()) {
        std::terminate();
    }
    const auto& slot = requestIndex_[resetIntentHead_];
    if (!slot.occupied || !slot.resetIntentPending) {
        std::terminate();
    }
    return TransportIntent{
        .token = {.kind = TransportIntentKind::kStreamReset,
            .id = {epoch_, connectionGeneration_, slot.streamId},
            .sequence = slot.resetIntentSequence},
        .streamResetErrorCode = slot.resetIntentErrorCode};
}

bool Http3BufferedServerConnection::ackTransportIntent(
    const TransportIntentToken& token) & noexcept {
    if (!onWorker() || token.id.epoch != epoch_ ||
        token.id.connectionGeneration != connectionGeneration_) {
        return false;
    }
    if (token.kind == TransportIntentKind::kConnectionClose) {
        if (!closeIntentPending_ || token.id.streamId != 0 ||
            token.sequence != kReservedCloseIntentSequence) {
            return false;
        }
        closeIntentPending_ = false;
        closeIntentHandedOff_ = true;
        clearResetIntents();
        notifyActivation();
        return true;
    }
    if (token.kind != TransportIntentKind::kStreamReset) {
        return false;
    }
    auto* slot = findRequestSlot(token.id.streamId);
    if (slot == nullptr || !slot->resetIntentPending ||
        token.sequence != slot->resetIntentSequence) {
        return false;
    }
    unlinkResetIntent(*slot);
    notifyActivation();
    return true;
}

bool Http3BufferedServerConnection::takeOverTransportRetirement(
    TransportRetirementTakeover takeover) & noexcept {
    if (!onWorker() || takeover.epoch != epoch_ ||
        takeover.connectionGeneration != connectionGeneration_ || !stopRequested_ ||
        !closeIntentPending_ || closeIntentHandedOff_ || transportRetired_ ||
        transportRetirementTakenOver_) {
        return false;
    }
    closeIntentPending_ = false;
    transportRetirementTakenOver_ = true;
    clearResetIntents();
    notifyActivation();
    return true;
}

bool Http3BufferedServerConnection::confirmTransportRetired(
    TransportRetirementConfirmation confirmation) & noexcept {
    if (!onWorker() || confirmation.epoch != epoch_ ||
        confirmation.connectionGeneration != connectionGeneration_ || transportRetired_) {
        return false;
    }
    transportRetired_ = true;
    closeIntentPending_ = false;
    clearResetIntents();
    if (!admissionClosed_) {
        stopRequested_ = true;
        stopEntries(false);
    }
    notifyActivation();
    return true;
}

std::size_t Http3BufferedServerConnection::pendingTransportIntentCount() const noexcept {
    return pendingResetIntentCount_ + static_cast<std::size_t>(closeIntentPending_);
}

bool Http3BufferedServerConnection::transportRetired() const noexcept {
    return transportRetired_;
}

Task<void> Http3BufferedServerConnection::runRequest(std::uint64_t streamId) {
    auto* slot = findRequestSlot(streamId);
    if (slot == nullptr || slot->status != RequestStatus::kAdmitting || slot->entry != nullptr) {
        if (slot != nullptr) {
            slot->status = RequestStatus::kFailed;
        }
        notifyActivation();
        requireConnectionClose(TransportCloseReason::kUnexpectedRequestState,
            Http3ConnectionErrorCode::kInternalError);
        co_return;
    }

    RequestRetirement requestRetirement{*this, streamId};
    std::optional<RequestEntry> request;
    try {
        request.emplace(*this, *slot, streamId);
    } catch (...) {
        slot->status = RequestStatus::kFailed;
        notifyActivation();
        requireConnectionClose(TransportCloseReason::kRequestConstructionFailure,
            Http3ConnectionErrorCode::kInternalError);
        co_return;
    }
    slot->entry = &*request;
    slot->status = RequestStatus::kRunning;
    ++activeRequestCount_;
    EntryRetirement retirement{*this, *request};
    auto& entry = *request;

    Dispatch::RunStatus runStatus = Dispatch::RunStatus::kFailed;
    try {
        runStatus = co_await entry.dispatch.runHandler();
        entry.slot.runStatus = runStatus;
    } catch (...) {
        removeQueued(entry.slot);
        entry.dispatch.cancel();
        entry.outputTerminal = true;
        entry.slot.status = RequestStatus::kFailed;
        notifyActivation();
        requireConnectionClose(TransportCloseReason::kHandlerFailure,
            Http3ConnectionErrorCode::kInternalError);
        co_return;
    }

    if (runStatus == Dispatch::RunStatus::kCancelled && !entry.cancelRequested &&
        !admissionClosed_) {
        switch (entry.dispatch.cancellationReason()) {
            case Dispatch::CancellationReason::kWorkerStop:
                (void)requestStop();
                break;
            case Dispatch::CancellationReason::kDeadline:
                cancelDeadlineEntry(entry);
                break;
            case Dispatch::CancellationReason::kExplicit:
                cancelEntry(entry, RequestStatus::kCancelled);
                break;
            case Dispatch::CancellationReason::kNone:
                entry.outputTerminal = true;
                entry.slot.status = RequestStatus::kFailed;
                notifyActivation();
                requireConnectionClose(TransportCloseReason::kHandlerFailure,
                    Http3ConnectionErrorCode::kInternalError);
                break;
        }
    }

    if (runStatus == Dispatch::RunStatus::kResponseReady && entry.outputEnabled &&
        !entry.cancelRequested && !admissionClosed_) {
        entry.slot.status = RequestStatus::kReadyToPublish;
        try {
            (void)entry.dispatch.registerPublicationDeadlineCallback(
                [this, entryPointer = &entry]() noexcept {
                    publicationStopped(*entryPointer);
                });
            enqueueForDemand(entry.slot, true);
        } catch (...) {
            removeQueued(entry.slot);
            entry.dispatch.cancel();
            entry.outputEnabled = false;
            entry.outputTerminal = true;
            entry.slot.status = RequestStatus::kFailed;
            entry.publicationFinished.notify();
            notifyActivation();
            requireConnectionClose(TransportCloseReason::kHandlerFailure,
                Http3ConnectionErrorCode::kInternalError);
            co_return;
        }
        while (!entry.outputTerminal && !entry.retirementGranted) {
            co_await entry.publicationFinished.wait();
        }
        co_return;
    }

    if (runStatus == Dispatch::RunStatus::kCancelled || entry.cancelRequested ||
        admissionClosed_ || !entry.outputEnabled) {
        while (!entry.retirementGranted) {
            co_await entry.publicationFinished.wait();
        }
        co_return;
    }

    entry.outputTerminal = true;
    entry.slot.status = RequestStatus::kFailed;
    notifyActivation();
    requireConnectionClose(TransportCloseReason::kHandlerFailure,
        Http3ConnectionErrorCode::kInternalError);
}

Http3BufferedServerConnection::EventResult
Http3BufferedServerConnection::handleInputResult(std::uint64_t streamId,
    Input::Result result, bool fromControl) {
    if (result.status == Input::Status::kConnectionClosed) {
        (void)confirmTransportRetired(
            {.epoch = epoch_, .connectionGeneration = connectionGeneration_});
        return {.status = EventStatus::kConnectionClosed,
            .input = result,
            .connectionCloseRequired = false};
    }

    if (result.status == Input::Status::kProtocolError) {
        const bool connectionFailure =
            result.protocol.scope == Http3ConnectionErrorScope::kConnection ||
            result.protocol.status == Http3ConnectionStatus::kConnectionError || input_.stopped();
        if (connectionFailure) {
            requireConnectionClose(TransportCloseReason::kConnectionProtocolError,
                result.protocol.code);
            return {.status = EventStatus::kProtocolError,
                .input = result,
                .connectionCloseRequired = true};
        }

        bool created = false;
        auto* slot = findOrCreateRequestSlot(streamId, created);
        if (slot == nullptr) {
            requireConnectionClose(TransportCloseReason::kRequestIndexCapacityExhausted,
                Http3ConnectionErrorCode::kExcessiveLoad);
            return {.status = EventStatus::kRequestIndexFull,
                .input = result,
                .connectionCloseRequired = true};
        }
        if (slot->entry != nullptr) {
            cancelEntry(*slot->entry, RequestStatus::kProtocolError);
        } else if (slot->rejectionEntry != nullptr) {
            (void)retireRejectedReceive(streamId, result, false);
            slot->status = RequestStatus::kProtocolError;
        } else {
            slot->status = RequestStatus::kProtocolError;
        }
        (void)enqueueResetIntent(*slot, result.protocol.code,
            ResetIntentOrigin::kStreamProtocolError);
        notifyActivation();
        return {.status = EventStatus::kProtocolError,
            .input = result,
            .connectionCloseRequired = false};
    }

    if (result.status == Input::Status::kCapacityExhausted ||
        result.status == Input::Status::kFinalSizeError) {
        const bool hasProtocolError =
            result.protocol.code != Http3ConnectionErrorCode::kNoError ||
            result.protocol.scope != Http3ConnectionErrorScope::kNone ||
            result.protocol.status == Http3ConnectionStatus::kStreamError ||
            result.protocol.status == Http3ConnectionStatus::kConnectionError;
        const auto errorCode = hasProtocolError
                                   ? result.protocol.code
                               : result.status == Input::Status::kCapacityExhausted
                                   ? Http3ConnectionErrorCode::kExcessiveLoad
                                   : Http3ConnectionErrorCode::kInternalError;
        requireConnectionClose(result.status == Input::Status::kCapacityExhausted
                                   ? TransportCloseReason::kInputCapacityExhausted
                                   : TransportCloseReason::kFinalSizeError,
            errorCode);
        return {.status = EventStatus::kInputRejected,
            .input = result,
            .connectionCloseRequired = true};
    }

    if (result.status == Input::Status::kStopped) {
        requireConnectionClose(TransportCloseReason::kInputStopped,
            Http3ConnectionErrorCode::kInternalError);
        return {.status = EventStatus::kInputRejected,
            .input = result,
            .connectionCloseRequired = true};
    }

    if (result.status == Input::Status::kDeferredReset) {
        if (fromControl) {
            auto* slot = findRequestSlot(streamId);
            if (slot != nullptr && slot->resetIntentPending) {
                unlinkResetIntent(*slot);
                notifyActivation();
            }
            if (slot != nullptr && slot->entry != nullptr) {
                cancelEntry(*slot->entry, RequestStatus::kCancelled);
            } else if (slot != nullptr && slot->rejectionEntry != nullptr) {
                cancelRejectionOutput(*slot->rejectionEntry, RequestStatus::kCancelled);
            }
        }
        return {.status = EventStatus::kAccepted,
            .input = result};
    }

    if (result.status == Input::Status::kReset) {
        auto* slot = findRequestSlot(streamId);
        if (slot != nullptr && slot->resetIntentPending) {
            unlinkResetIntent(*slot);
            notifyActivation();
        }
        if (slot != nullptr && slot->entry != nullptr) {
            cancelEntry(*slot->entry, RequestStatus::kCancelled);
            return {.status = EventStatus::kStreamCancelled,
                .input = result};
        }
        if (slot != nullptr && slot->rejectionEntry != nullptr) {
            return retireRejectedReceive(streamId, result, true);
        }
        return {.status = EventStatus::kAccepted,
            .input = result};
    }

    if (result.status == Input::Status::kFed || result.status == Input::Status::kFinished) {
        const auto rejection = session_.rejection(streamId);
        if (session_.request(streamId) != nullptr && rejection != Session::Rejection::kNone) {
            return startRejection(streamId, rejection, result,
                result.status == Input::Status::kFinished);
        }
    }

    if (result.status == Input::Status::kFinished) {
        // The protocol core must consume peer unidirectional streams first so
        // critical-stream termination and stream-type errors remain visible.
        // A successfully ignored unknown peer uni stream is not a Web request.
        if (isHttp3ClientUnidirectionalStreamId(streamId)) {
            return {.status = EventStatus::kAccepted,
                .input = result};
        }
        return admitFinishedRequest(streamId, result);
    }

    if (result.status == Input::Status::kFed || result.status == Input::Status::kDeferredFin ||
        result.status == Input::Status::kIgnoredControl) {
        return {.status = EventStatus::kAccepted,
            .input = result};
    }
    return {.status = EventStatus::kInputRejected,
        .input = result};
}

Http3BufferedServerConnection::EventResult
Http3BufferedServerConnection::startRejection(std::uint64_t streamId,
    Session::Rejection rejection, Input::Result result, bool receiveFinished) {
    const auto* request = session_.request(streamId);
    if (request == nullptr || rejection == Session::Rejection::kNone) {
        return {.status = EventStatus::kSessionNotReady,
            .input = result,
            .rejection = rejection,
            .connectionCloseRequired = true};
    }

    bool created = false;
    auto* slot = findOrCreateRequestSlot(streamId, created);
    if (slot == nullptr) {
        requireConnectionClose(TransportCloseReason::kRequestIndexCapacityExhausted,
            Http3ConnectionErrorCode::kExcessiveLoad);
        return {.status = EventStatus::kRequestIndexFull,
            .input = result,
            .rejection = rejection,
            .connectionCloseRequired = true};
    }

    auto* entry = slot->rejectionEntry;
    bool rejectionCreated = false;
    if (entry == nullptr) {
        if (slot->entry != nullptr || (!created && slot->status != RequestStatus::kUnknown)) {
            slot->status = RequestStatus::kFailed;
            requireConnectionClose(TransportCloseReason::kUnexpectedRequestState,
                Http3ConnectionErrorCode::kInternalError);
            return {.status = EventStatus::kSessionNotReady,
                .input = result,
                .rejection = rejection,
                .connectionCloseRequired = true};
        }
        std::pmr::polymorphic_allocator<RejectionEntry> allocator(worker_.resource());
        try {
            entry = allocator.allocate(1);
            try {
                std::construct_at(entry, *this, *slot, streamId,
                    request->request().knownMethod(), rejection);
            } catch (...) {
                allocator.deallocate(entry, 1);
                entry = nullptr;
                throw;
            }
        } catch (...) {
            slot->status = RequestStatus::kFailed;
            requireConnectionClose(TransportCloseReason::kRequestConstructionFailure,
                Http3ConnectionErrorCode::kInternalError);
            return {.status = EventStatus::kSessionNotReady,
                .input = result,
                .rejection = rejection,
                .connectionCloseRequired = true};
        }
        slot->rejectionEntry = entry;
        slot->status = RequestStatus::kRejected;
        slot->rejection = rejection;
        ++activeRejectionCount_;
        rejectionCreated = true;
    } else if (slot->entry != nullptr || slot->rejection != rejection) {
        slot->status = RequestStatus::kFailed;
        requireConnectionClose(TransportCloseReason::kUnexpectedRequestState,
            Http3ConnectionErrorCode::kInternalError);
        return {.status = EventStatus::kSessionNotReady,
            .input = result,
            .rejection = rejection,
            .connectionCloseRequired = true};
    }

    if (receiveFinished && !entry->receiveTerminal) {
        if (!session_.release(streamId)) {
            requireConnectionClose(TransportCloseReason::kSessionNotReady,
                Http3ConnectionErrorCode::kInternalError);
            return {.status = EventStatus::kSessionNotReady,
                .input = result,
                .rejection = rejection,
                .connectionCloseRequired = true};
        }
        entry->receiveTerminal = true;
        if (entry->outputTerminal) {
            finishRejection(*entry);
            return {.status = EventStatus::kRejected,
                .input = result,
                .rejection = rejection};
        }
    }
    if (rejectionCreated) {
        enqueueForDemand(*slot, true);
    }
    return {.status = EventStatus::kRejected,
        .input = result,
        .rejection = rejection};
}

Http3BufferedServerConnection::EventResult
Http3BufferedServerConnection::retireRejectedReceive(std::uint64_t streamId,
    Input::Result result, bool reset) {
    auto* slot = findRequestSlot(streamId);
    if (slot == nullptr || slot->rejectionEntry == nullptr) {
        return {.status = reset ? EventStatus::kAccepted : EventStatus::kProtocolError,
            .input = result};
    }
    auto* entry = slot->rejectionEntry;
    entry->receiveTerminal = true;
    if (!entry->outputTerminal) {
        cancelRejectionOutput(*entry,
            reset ? RequestStatus::kCancelled : RequestStatus::kProtocolError);
    } else {
        finishRejection(*entry);
    }
    return {.status = reset ? EventStatus::kStreamCancelled : EventStatus::kProtocolError,
        .input = result};
}

Http3BufferedServerConnection::PublishAttempt
Http3BufferedServerConnection::publishRejection(RequestIndexSlot& slot,
    RejectionEntry& entry) noexcept {
    const auto streamId = entry.streamId;
    if (!entry.outputEnabled || entry.outputTerminal || !entry.output) {
        slot.status = RequestStatus::kFailed;
        requireConnectionClose(TransportCloseReason::kUnexpectedRequestState,
            Http3ConnectionErrorCode::kInternalError);
        return {.status = PublishStatus::kAttempted,
            .streamId = streamId,
            .publication = {Dispatch::PublishStatus::kFailed}};
    }

    const auto result = entry.output->publishStep();
    Dispatch::PublishResult publication{};
    switch (result.status) {
        case Http3BufferedResponseOutput::Status::kBytes:
            publication = {Dispatch::PublishStatus::kBytesPublished,
                result.bytesAccepted, result.notifyPeer};
            enqueueForDemand(slot, false);
            break;
        case Http3BufferedResponseOutput::Status::kFin:
            publication = {Dispatch::PublishStatus::kFinPublished, 0, result.notifyPeer};
            entry.outputTerminal = true;
            slot.status = RequestStatus::kPublished;
            entry.output.reset();
            entry.response.reset();
            finishRejection(entry);
            break;
        case Http3BufferedResponseOutput::Status::kBackpressured: {
            publication.status = Dispatch::PublishStatus::kBackpressured;
            publication.notifyPeer = result.notifyPeer;
            switch (result.blockReason) {
                case Http3BufferedResponseOutput::BlockReason::kData:
                    publication.blockReason = Dispatch::PublishBlockReason::kData;
                    break;
                case Http3BufferedResponseOutput::BlockReason::kControl:
                    publication.blockReason = Dispatch::PublishBlockReason::kControl;
                    break;
                case Http3BufferedResponseOutput::BlockReason::kNone:
                    publication.status = Dispatch::PublishStatus::kFailed;
                    slot.status = RequestStatus::kFailed;
                    entry.outputTerminal = true;
                    requireConnectionClose(TransportCloseReason::kPublishFailure,
                        Http3ConnectionErrorCode::kInternalError);
                    break;
            }
            if (publication.status == Dispatch::PublishStatus::kBackpressured) {
                enqueueBlocked(slot, publication.blockReason);
            }
            break;
        }
        case Http3BufferedResponseOutput::Status::kComplete:
            publication = {Dispatch::PublishStatus::kComplete, 0, result.notifyPeer};
            entry.outputTerminal = true;
            slot.status = RequestStatus::kPublished;
            entry.output.reset();
            entry.response.reset();
            finishRejection(entry);
            break;
        case Http3BufferedResponseOutput::Status::kFailed:
            publication = {Dispatch::PublishStatus::kFailed,
                result.bytesAccepted, result.notifyPeer};
            slot.status = RequestStatus::kFailed;
            entry.outputTerminal = true;
            requireConnectionClose(TransportCloseReason::kPublishFailure,
                Http3ConnectionErrorCode::kInternalError);
            break;
    }
    return {.status = PublishStatus::kAttempted,
        .streamId = streamId,
        .publication = publication};
}

void Http3BufferedServerConnection::cancelRejectionOutput(
    RejectionEntry& entry, RequestStatus status) noexcept {
    if (entry.outputTerminal) {
        return;
    }
    removeQueued(entry.slot);
    entry.outputEnabled = false;
    if (entry.output) {
        entry.output->stop();
        entry.output.reset();
    }
    entry.response.reset();
    entry.outputTerminal = true;
    entry.slot.status = status;
    notifyActivation();
    finishRejection(entry);
}

void Http3BufferedServerConnection::finishRejection(RejectionEntry& entry) noexcept {
    if (!entry.outputTerminal || !entry.receiveTerminal) {
        return;
    }
    auto& slot = entry.slot;
    if (slot.queue != QueueKind::kNone || slot.rejectionEntry != &entry ||
        activeRejectionCount_ == 0) {
        std::terminate();
    }
    slot.rejectionEntry = nullptr;
    --activeRejectionCount_;
    std::pmr::polymorphic_allocator<RejectionEntry> allocator(worker_.resource());
    std::destroy_at(&entry);
    allocator.deallocate(&entry, 1);
}

Http3BufferedServerConnection::EventResult
Http3BufferedServerConnection::admitFinishedRequest(
    std::uint64_t streamId, Input::Result result) {
    const auto streamState = session_.streamState(streamId);
    if (streamState != Session::StreamState::kReady) {
        bool created = false;
        auto* slot = findOrCreateRequestSlot(streamId, created);
        if (slot != nullptr) {
            slot->status = RequestStatus::kFailed;
        }
        notifyActivation();
        requireConnectionClose(TransportCloseReason::kSessionNotReady,
            Http3ConnectionErrorCode::kInternalError);
        return {.status = EventStatus::kSessionNotReady,
            .input = result,
            .connectionCloseRequired = true};
    }

    bool created = false;
    auto* slot = findOrCreateRequestSlot(streamId, created);
    if (slot == nullptr) {
        requireConnectionClose(TransportCloseReason::kRequestIndexCapacityExhausted,
            Http3ConnectionErrorCode::kExcessiveLoad);
        return {.status = EventStatus::kRequestIndexFull,
            .input = result,
            .connectionCloseRequired = true};
    }
    if (!created) {
        return {.status = EventStatus::kInputRejected,
            .input = result};
    }

    slot->status = RequestStatus::kAdmitting;
    try {
        auto task = runRequest(streamId);
        tasks_.spawn(std::move(task));
        everSpawned_ = true;
    } catch (...) {
        slot->status = RequestStatus::kFailed;
        requireConnectionClose(TransportCloseReason::kDispatchStartFailure,
            Http3ConnectionErrorCode::kInternalError);
        return {.status = EventStatus::kDispatchStartFailed,
            .input = result,
            .connectionCloseRequired = true};
    }
    return {.status = EventStatus::kDispatched,
        .input = result};
}

Http3BufferedServerConnection::RequestIndexSlot*
Http3BufferedServerConnection::findOrCreateRequestSlot(
    std::uint64_t streamId, bool& created) noexcept {
    created = false;
    const auto mask = requestIndex_.size() - 1;
    auto index = static_cast<std::size_t>(mixStreamId(streamId)) & mask;
    for (std::size_t probes = 0; probes < requestIndex_.size(); ++probes) {
        auto& slot = requestIndex_[index];
        if (!slot.occupied) {
            if (trackedRequestCount_ >= maxTrackedStreams_) {
                return nullptr;
            }
            slot.streamId = streamId;
            slot.entry = nullptr;
            slot.queuePrevious = nullptr;
            slot.queueNext = nullptr;
            slot.queue = QueueKind::kNone;
            slot.status = RequestStatus::kUnknown;
            slot.rejection = Session::Rejection::kNone;
            slot.runStatus.reset();
            slot.resetIntentPrevious = kNoIntentSlot;
            slot.resetIntentNext = kNoIntentSlot;
            slot.resetIntentSequence = 0;
            slot.resetIntentErrorCode = Http3ConnectionErrorCode::kRequestCancelled;
            slot.resetIntentOrigin = ResetIntentOrigin::kLocalCancellation;
            slot.resetIntentPending = false;
            slot.occupied = true;
            ++trackedRequestCount_;
            created = true;
            return &slot;
        }
        if (slot.streamId == streamId) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

Http3BufferedServerConnection::RequestIndexSlot*
Http3BufferedServerConnection::findRequestSlot(std::uint64_t streamId) noexcept {
    return const_cast<RequestIndexSlot*>(
        std::as_const(*this).findRequestSlot(streamId));
}

const Http3BufferedServerConnection::RequestIndexSlot*
Http3BufferedServerConnection::findRequestSlot(std::uint64_t streamId) const noexcept {
    if (requestIndex_.empty()) {
        return nullptr;
    }
    const auto mask = requestIndex_.size() - 1;
    auto index = static_cast<std::size_t>(mixStreamId(streamId)) & mask;
    for (std::size_t probes = 0; probes < requestIndex_.size(); ++probes) {
        const auto& slot = requestIndex_[index];
        if (!slot.occupied) {
            return nullptr;
        }
        if (slot.streamId == streamId) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

Http3BufferedServerConnection::IntrusiveQueue&
Http3BufferedServerConnection::queueFor(QueueKind kind) noexcept {
    switch (kind) {
        case QueueKind::kDataRunnable:
            return dataRunnable_;
        case QueueKind::kControlRunnable:
            return controlRunnable_;
        case QueueKind::kLocalRunnable:
            return localRunnable_;
        case QueueKind::kDataBlocked:
            return dataBlocked_;
        case QueueKind::kControlBlocked:
            return controlBlocked_;
        case QueueKind::kNone:
            break;
    }
    std::terminate();
}

const Http3BufferedServerConnection::IntrusiveQueue&
Http3BufferedServerConnection::queueFor(QueueKind kind) const noexcept {
    return const_cast<Http3BufferedServerConnection*>(this)->queueFor(kind);
}

void Http3BufferedServerConnection::enqueueRunnable(
    RequestIndexSlot& slot, QueueKind kind, bool notify) noexcept {
    if (kind != QueueKind::kDataRunnable && kind != QueueKind::kControlRunnable &&
        kind != QueueKind::kLocalRunnable) {
        std::terminate();
    }
    const auto* entry = slot.entry;
    const auto* rejection = slot.rejectionEntry;
    if (slot.queue != QueueKind::kNone || stopRequested_ ||
        ((entry == nullptr) == (rejection == nullptr)) ||
        (entry != nullptr && (!entry->outputEnabled || entry->outputTerminal)) ||
        (rejection != nullptr && (!rejection->outputEnabled || rejection->outputTerminal))) {
        return;
    }
    auto& queue = queueFor(kind);
    const bool wasLaneEmpty = queue.head == nullptr;
    slot.queuePrevious = queue.tail;
    slot.queueNext = nullptr;
    if (queue.tail != nullptr) {
        queue.tail->queueNext = &slot;
    } else {
        queue.head = &slot;
    }
    queue.tail = &slot;
    slot.queue = kind;
    ++readyRequestCount_;
    if (notify && wasLaneEmpty) {
        notifyActivation();
    }
}

void Http3BufferedServerConnection::enqueueForDemand(
    RequestIndexSlot& slot, bool notify) noexcept {
    auto* entry = slot.entry;
    auto* rejection = slot.rejectionEntry;
    if ((entry == nullptr) == (rejection == nullptr)) {
        std::terminate();
    }
    QueueKind desired = QueueKind::kLocalRunnable;
    if (rejection != nullptr) {
        switch (rejection->output->nextStep()) {
            case Http3BufferedResponseWrite::NextStep::kBytes:
                desired = QueueKind::kDataRunnable;
                break;
            case Http3BufferedResponseWrite::NextStep::kFin:
                desired = QueueKind::kControlRunnable;
                break;
            case Http3BufferedResponseWrite::NextStep::kComplete:
            case Http3BufferedResponseWrite::NextStep::kFailed:
                break;
        }
    } else {
        switch (entry->dispatch.publicationDemand()) {
            case Dispatch::PublicationDemand::kData:
                desired = QueueKind::kDataRunnable;
                break;
            case Dispatch::PublicationDemand::kControl:
                desired = QueueKind::kControlRunnable;
                break;
            case Dispatch::PublicationDemand::kLocalComplete:
            case Dispatch::PublicationDemand::kLocalCancelled:
            case Dispatch::PublicationDemand::kLocalMailboxStopped:
            case Dispatch::PublicationDemand::kLocalPeerLimitRejected:
            case Dispatch::PublicationDemand::kLocalFailed:
            case Dispatch::PublicationDemand::kNotReady:
            case Dispatch::PublicationDemand::kWrongWorker:
                break;
        }
    }
    if (slot.queue == desired) {
        return;
    }
    removeQueued(slot);
    enqueueRunnable(slot, desired, notify);
}

void Http3BufferedServerConnection::enqueueBlocked(
    RequestIndexSlot& slot, Dispatch::PublishBlockReason reason) noexcept {
    QueueKind kind = QueueKind::kNone;
    switch (reason) {
        case Dispatch::PublishBlockReason::kData:
            kind = QueueKind::kDataBlocked;
            break;
        case Dispatch::PublishBlockReason::kControl:
            kind = QueueKind::kControlBlocked;
            break;
        case Dispatch::PublishBlockReason::kNone:
            std::terminate();
    }
    const auto* entry = slot.entry;
    const auto* rejection = slot.rejectionEntry;
    if (slot.queue != QueueKind::kNone || stopRequested_ ||
        ((entry == nullptr) == (rejection == nullptr)) ||
        (entry != nullptr && (!entry->outputEnabled || entry->outputTerminal)) ||
        (rejection != nullptr && (!rejection->outputEnabled || rejection->outputTerminal))) {
        return;
    }
    auto& queue = queueFor(kind);
    slot.queuePrevious = queue.tail;
    slot.queueNext = nullptr;
    if (queue.tail != nullptr) {
        queue.tail->queueNext = &slot;
    } else {
        queue.head = &slot;
    }
    queue.tail = &slot;
    slot.queue = kind;
    ++blockedRequestCount_;
}

void Http3BufferedServerConnection::removeQueued(RequestIndexSlot& slot) noexcept {
    if (slot.queue == QueueKind::kNone) {
        return;
    }
    auto& queue = queueFor(slot.queue);
    if (slot.queuePrevious != nullptr) {
        slot.queuePrevious->queueNext = slot.queueNext;
    } else {
        queue.head = slot.queueNext;
    }
    if (slot.queueNext != nullptr) {
        slot.queueNext->queuePrevious = slot.queuePrevious;
    } else {
        queue.tail = slot.queuePrevious;
    }
    const bool wasBlocked = slot.queue == QueueKind::kDataBlocked ||
                            slot.queue == QueueKind::kControlBlocked;
    slot.queuePrevious = nullptr;
    slot.queueNext = nullptr;
    slot.queue = QueueKind::kNone;
    auto& count = wasBlocked ? blockedRequestCount_ : readyRequestCount_;
    if (count == 0) {
        std::terminate();
    }
    --count;
}

Http3BufferedServerConnection::RequestIndexSlot*
Http3BufferedServerConnection::selectRunnable(WorkLanes eligibleLanes) const noexcept {
    std::array<WorkLane, 3> lanes{};
    switch (nextPublishLane_) {
        case WorkLane::kData:
            lanes = {WorkLane::kData, WorkLane::kControl, WorkLane::kLocal};
            break;
        case WorkLane::kControl:
            lanes = {WorkLane::kControl, WorkLane::kLocal, WorkLane::kData};
            break;
        case WorkLane::kLocal:
            lanes = {WorkLane::kLocal, WorkLane::kData, WorkLane::kControl};
            break;
    }
    for (const auto lane : lanes) {
        if (!eligibleLanes.contains(lane)) {
            continue;
        }
        const auto kind = lane == WorkLane::kData      ? QueueKind::kDataRunnable
                          : lane == WorkLane::kControl ? QueueKind::kControlRunnable
                                                       : QueueKind::kLocalRunnable;
        if (const auto* queue = &queueFor(kind); queue->head != nullptr) {
            return queue->head;
        }
    }
    return nullptr;
}

void Http3BufferedServerConnection::publicationStopped(RequestEntry& entry) noexcept {
    // StopToken callbacks run synchronously on their source thread. The owner
    // contract requires every stop source observed here to fire on this worker;
    // fail before inspecting or touching any intrusive queue otherwise.
    if (!onWorker()) {
        std::terminate();
    }
    if (entry.outputTerminal || entry.retirementGranted || stopRequested_) {
        return;
    }
    if (entry.slot.queue == QueueKind::kLocalRunnable) {
        entry.deadlineActivationQueued = true;
        return;
    }
    removeQueued(entry.slot);
    enqueueRunnable(entry.slot, QueueKind::kLocalRunnable, false);
    if (!entry.deadlineActivationQueued && entry.slot.queue == QueueKind::kLocalRunnable) {
        entry.deadlineActivationQueued = true;
        notifyActivation();
    }
}

void Http3BufferedServerConnection::cancelDeadlineEntry(RequestEntry& entry) noexcept {
    if (entry.outputTerminal) {
        return;
    }
    // Its complete request FIN is already recorded. Do not call Input.cancelRequest:
    // that API is reserved for transport-retired input fencing and would replace
    // the valid finished-input record with a local-cancel tombstone.
    (void)enqueueResetIntent(entry.slot);
    cancelEntry(entry, RequestStatus::kCancelled);
}

void Http3BufferedServerConnection::cancelEntry(
    RequestEntry& entry, RequestStatus status) noexcept {
    if (entry.outputTerminal) {
        return;
    }
    entry.outputEnabled = false;
    entry.cancelRequested = true;
    removeQueued(entry.slot);
    // The input RESET/tombstone or connection stop is established by the caller
    // before this function. A suspended handler retains its dispatch lease until
    // runHandler() returns; only then may the wrapper consume this permit.
    entry.dispatch.cancel();
    entry.outputTerminal = true;
    entry.retirementGranted = true;
    entry.slot.status = status;
    entry.publicationFinished.notify();
    notifyActivation();
}

void Http3BufferedServerConnection::stopEntries(bool inputAlreadyStopped) noexcept {
    admissionClosed_ = true;

    // First quiesce every output so cancellation of one handler cannot let a
    // sibling publish while the connection is being retired.
    for (auto& slot : requestIndex_) {
        if (auto* rejection = slot.rejectionEntry;
            rejection != nullptr && !rejection->outputTerminal) {
            rejection->outputEnabled = false;
            removeQueued(slot);
        }
        auto* entry = slot.entry;
        if (entry == nullptr || entry->outputTerminal) {
            continue;
        }
        entry->outputEnabled = false;
        entry->cancelRequested = true;
        removeQueued(slot);
    }
    if (!inputAlreadyStopped) {
        input_.stop();
    }

    for (auto& slot : requestIndex_) {
        if (auto* rejection = slot.rejectionEntry; rejection != nullptr) {
            if (!rejection->outputTerminal) {
                rejection->output->stop();
                rejection->output.reset();
                rejection->response.reset();
                rejection->outputTerminal = true;
                slot.status = RequestStatus::kCancelled;
            }
            rejection->receiveTerminal = true;
            finishRejection(*rejection);
        }
        auto* entry = slot.entry;
        if (entry == nullptr || entry->outputTerminal) {
            continue;
        }
        entry->dispatch.cancel();
        entry->outputTerminal = true;
        entry->retirementGranted = true;
        entry->slot.status = RequestStatus::kCancelled;
        entry->publicationFinished.notify();
    }
    notifyActivation();
}

void Http3BufferedServerConnection::notifyActivation() noexcept {
    if (!onWorker() || !activation_.valid()) {
        std::terminate();
    }
    const auto activation = activationSnapshot();
    activation_.activate(activation_.context, epoch_, connectionGeneration_,
        activation_.slotGeneration, activation);
}

Http3BufferedServerConnection::WorkerActivation
Http3BufferedServerConnection::activationSnapshot() const noexcept {
    const auto intent = peekTransportIntent();
    return {.work = workState(),
        .transportIntent = intent ? std::optional<TransportIntentToken>(intent->token) : std::nullopt};
}

bool Http3BufferedServerConnection::detachActivationAfterJoin() noexcept {
    if (!onWorker() || !joinCompleted_ || !admissionClosed_ || activeRequestCount_ != 0 ||
        activeRejectionCount_ != 0 || tasks_.size() != 0 || pendingTransportIntentCount() != 0 ||
        (!transportRetired_ && !transportRetirementTakenOver_ && !closeIntentHandedOff_)) {
        return false;
    }
    activation_ = {};
    return true;
}

bool Http3BufferedServerConnection::enqueueResetIntent(
    RequestIndexSlot& slot, Http3ConnectionErrorCode errorCode,
    ResetIntentOrigin origin) noexcept {
    if (!slot.occupied) {
        requireConnectionClose(TransportCloseReason::kTransportIntentCapacityExhausted,
            Http3ConnectionErrorCode::kInternalError);
        return false;
    }
    if (transportRetired_ || transportRetirementTakenOver_ || closeIntentHandedOff_) {
        return false;
    }
    if (slot.resetIntentPending) {
        // Identical codes are idempotent. The first protocol error wins, and a
        // local H3_REQUEST_CANCELLED never downgrades an existing protocol code;
        // only a protocol error may supersede a pending local cancellation.
        if (slot.resetIntentErrorCode == errorCode) {
            if (origin == ResetIntentOrigin::kStreamProtocolError) {
                slot.resetIntentOrigin = origin;
            }
            return true;
        }
        if (slot.resetIntentOrigin == ResetIntentOrigin::kStreamProtocolError ||
            origin != ResetIntentOrigin::kStreamProtocolError) {
            return true;
        }
        if (nextTransportIntentSequence_ == 0 ||
            nextTransportIntentSequence_ == kReservedCloseIntentSequence) {
            requireConnectionClose(TransportCloseReason::kTransportIntentSequenceExhausted,
                Http3ConnectionErrorCode::kInternalError);
            return false;
        }
        // A protocol error supersedes only a local cancellation. Give the new
        // payload a new token so an ack for the old code cannot clear it.
        slot.resetIntentErrorCode = errorCode;
        slot.resetIntentOrigin = origin;
        slot.resetIntentSequence = nextTransportIntentSequence_++;
        return true;
    }
    if (pendingResetIntentCount_ >= maxTrackedStreams_) {
        requireConnectionClose(TransportCloseReason::kTransportIntentCapacityExhausted,
            Http3ConnectionErrorCode::kInternalError);
        return false;
    }
    if (nextTransportIntentSequence_ == 0 ||
        nextTransportIntentSequence_ == kReservedCloseIntentSequence) {
        requireConnectionClose(TransportCloseReason::kTransportIntentSequenceExhausted,
            Http3ConnectionErrorCode::kInternalError);
        return false;
    }

    const auto slotIndex = static_cast<std::size_t>(&slot - requestIndex_.data());
    if (slotIndex >= requestIndex_.size()) {
        requireConnectionClose(TransportCloseReason::kTransportIntentCapacityExhausted,
            Http3ConnectionErrorCode::kInternalError);
        return false;
    }
    slot.resetIntentSequence = nextTransportIntentSequence_++;
    slot.resetIntentErrorCode = errorCode;
    slot.resetIntentOrigin = origin;
    slot.resetIntentPending = true;
    slot.resetIntentPrevious = resetIntentTail_;
    slot.resetIntentNext = kNoIntentSlot;
    if (resetIntentTail_ != kNoIntentSlot) {
        if (resetIntentTail_ >= requestIndex_.size()) {
            std::terminate();
        }
        requestIndex_[resetIntentTail_].resetIntentNext = slotIndex;
    } else {
        resetIntentHead_ = slotIndex;
    }
    resetIntentTail_ = slotIndex;
    ++pendingResetIntentCount_;
    return true;
}

void Http3BufferedServerConnection::unlinkResetIntent(RequestIndexSlot& slot) noexcept {
    if (!slot.resetIntentPending) {
        std::terminate();
    }
    if (slot.resetIntentPrevious != kNoIntentSlot) {
        if (slot.resetIntentPrevious >= requestIndex_.size()) {
            std::terminate();
        }
        requestIndex_[slot.resetIntentPrevious].resetIntentNext = slot.resetIntentNext;
    } else {
        resetIntentHead_ = slot.resetIntentNext;
    }
    if (slot.resetIntentNext != kNoIntentSlot) {
        if (slot.resetIntentNext >= requestIndex_.size()) {
            std::terminate();
        }
        requestIndex_[slot.resetIntentNext].resetIntentPrevious = slot.resetIntentPrevious;
    } else {
        resetIntentTail_ = slot.resetIntentPrevious;
    }
    slot.resetIntentPrevious = kNoIntentSlot;
    slot.resetIntentNext = kNoIntentSlot;
    slot.resetIntentSequence = 0;
    slot.resetIntentErrorCode = Http3ConnectionErrorCode::kRequestCancelled;
    slot.resetIntentOrigin = ResetIntentOrigin::kLocalCancellation;
    slot.resetIntentPending = false;
    if (pendingResetIntentCount_ == 0) {
        std::terminate();
    }
    --pendingResetIntentCount_;
}

void Http3BufferedServerConnection::clearResetIntents() noexcept {
    auto index = resetIntentHead_;
    while (index != kNoIntentSlot) {
        if (index >= requestIndex_.size()) {
            std::terminate();
        }
        auto& slot = requestIndex_[index];
        if (!slot.occupied || !slot.resetIntentPending) {
            std::terminate();
        }
        const auto next = slot.resetIntentNext;
        slot.resetIntentPrevious = kNoIntentSlot;
        slot.resetIntentNext = kNoIntentSlot;
        slot.resetIntentSequence = 0;
        slot.resetIntentErrorCode = Http3ConnectionErrorCode::kRequestCancelled;
        slot.resetIntentOrigin = ResetIntentOrigin::kLocalCancellation;
        slot.resetIntentPending = false;
        index = next;
    }
    resetIntentHead_ = kNoIntentSlot;
    resetIntentTail_ = kNoIntentSlot;
    pendingResetIntentCount_ = 0;
}

void Http3BufferedServerConnection::requireConnectionClose(
    TransportCloseReason reason, std::optional<Http3ConnectionErrorCode> errorCode) noexcept {
    if (reason == TransportCloseReason::kNone) {
        std::terminate();
    }
    transportCloseRequired_ = true;
    stopRequested_ = true;
    if (!closeIntentPending_ && !closeIntentHandedOff_ && !transportRetired_ &&
        !transportRetirementTakenOver_) {
        closeIntentReason_ = reason;
        closeIntentErrorCode_ = errorCode;
        closeIntentPending_ = true;
    }
    if (!admissionClosed_) {
        stopEntries(false);
    }
}

void Http3BufferedServerConnection::finishEntry(RequestEntry& entry) noexcept {
    if (entry.slot.queue != QueueKind::kNone || entry.dispatch.handlerActive() ||
        !entry.outputTerminal || entry.slot.entry != &entry || activeRequestCount_ == 0) {
        std::terminate();
    }
    entry.slot.entry = nullptr;
    --activeRequestCount_;
}

bool Http3BufferedServerConnection::onWorker() const noexcept {
    return services_.worker().isCurrent();
}

std::size_t Http3BufferedServerConnection::indexCapacity(std::size_t maxTrackedStreams) {
    if (maxTrackedStreams == 0 ||
        maxTrackedStreams > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::invalid_argument("HTTP/3 request index capacity must be positive and bounded");
    }
    const auto needed = maxTrackedStreams * 2;
    const auto maxPowerOfTwo = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (needed > maxPowerOfTwo) {
        throw std::length_error("HTTP/3 request index capacity is too large");
    }
    return std::bit_ceil(needed);
}

}  // namespace ruvia::detail
