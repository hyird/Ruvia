#include "http3/Http3ServerConnection.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/HttpRequestTarget.h"

#include "router/RouteTable.h"
#include "server/HttpServerOptions.h"

namespace ruvia::detail {
namespace {

[[nodiscard]]

[[nodiscard]] HttpStatusCode
rejectionStatus(
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

struct Http3ServerConnection::PendingPush final {
    PendingPush(Http3ServerConnection& ownerValue, TransportIntentToken tokenValue)
        : owner(ownerValue),
          token(tokenValue),
          ready(owner.services_.worker()),
          previous(owner.pendingPushTail_),
          intentPrevious(owner.pushIntentTail_) {
        if (previous) {
            previous->next = this;
        } else {
            owner.pendingPushHead_ = this;
        }
        owner.pendingPushTail_ = this;
        if (intentPrevious) {
            intentPrevious->intentNext = this;
        } else {
            owner.pushIntentHead_ = this;
        }
        owner.pushIntentTail_ = this;
        ++owner.pendingPushCount_;
        ++owner.pendingPushIntentCount_;
    }
    ~PendingPush() {
        if (owed || owner.pendingPushCount_ == 0) {
            std::terminate();
        }
        if (previous) {
            previous->next = next;
        } else {
            owner.pendingPushHead_ = next;
        }
        if (next) {
            next->previous = previous;
        } else {
            owner.pendingPushTail_ = previous;
        }
        --owner.pendingPushCount_;
    }
    void settle(PushStreamOpenResult result) noexcept {
        if (!owed || owner.pendingPushIntentCount_ == 0) {
            std::terminate();
        }
        opened = result;
        owed = false;
        if (intentPrevious) {
            intentPrevious->intentNext = intentNext;
        } else {
            owner.pushIntentHead_ = intentNext;
        }
        if (intentNext) {
            intentNext->intentPrevious = intentPrevious;
        } else {
            owner.pushIntentTail_ = intentPrevious;
        }
        intentPrevious = nullptr;
        intentNext = nullptr;
        --owner.pendingPushIntentCount_;
        ready.notify();
    }
    Http3ServerConnection& owner;
    const TransportIntentToken token;
    WorkerSignal ready;
    PendingPush* previous{};
    PendingPush* next{};
    PendingPush* intentPrevious{};
    PendingPush* intentNext{};
    std::optional<PushStreamOpenResult> opened{};
    bool owed{true};
    bool cancelled{};
};

struct Http3ServerConnection::RequestEntry final {
    RequestEntry(Http3ServerConnection& ownerValue, RequestIndexSlot& slotValue,
        std::uint64_t streamIdValue)
        : ownerScanner(ownerValue.connectionScanner_),
          slot(slotValue),
          streamId(streamIdValue),
          dispatch(ownerValue.session_, ownerValue.routes_, ownerValue.worker_,
              ownerValue.services_, ownerValue.options_, ownerValue.outbound_,
              {ownerValue.epoch_, ownerValue.connectionGeneration_, streamId, slotValue.pushId,
                  ownerValue.input_.receivedEarlyData(streamId)},

              scannerEntry, ownerValue.executor_,
              {.context = &ownerValue,
                  .attachScanner = &Http3ServerConnection::attachTunnelScannerThunk,
                  .outputReady = &Http3ServerConnection::tunnelOutputReadyThunk,
                  .abort = &Http3ServerConnection::abortTunnelThunk,
                  .inputConsumed = &Http3ServerConnection::requestInputConsumedThunk,
                  .push = [](void* raw, std::uint64_t parent, HttpPushRequestView request) -> Task<bool> {
                      co_return co_await static_cast<Http3ServerConnection*>(raw)->pushRequest(parent, request);
                  },
                  .sendDatagram = [](void* raw, std::uint64_t streamId, std::span<const std::byte> bytes) {
                      auto& owner=*static_cast<Http3ServerConnection*>(raw);
                      if(!owner.datagramOutput_.send || owner.stopRequested_){ throw std::runtime_error("HTTP Datagram output is unavailable");
}
                      owner.datagramOutput_.send(owner.datagramOutput_.context,streamId,bytes); }},
              slotValue.responsePreludeBytes),
          publicationFinished(ownerValue.services_.worker()) {}

    ~RequestEntry() {
        if (scannerRegistered) {
            ownerScanner->unregisterEntry(scannerEntry);
        }
    }

    ConnectionScanner::Entry scannerEntry;
    ConnectionScanner* ownerScanner{};

    RequestIndexSlot& slot;
    const std::uint64_t streamId;
    Dispatch dispatch;
    WorkerSignal publicationFinished;
    bool scannerRegistered{};
    bool outputEnabled{true};
    // Publication can terminate while the request frame still awaits peer FIN.
    bool outputTerminal{};
    // Independent permission for runRequest to retire after cancellation.
    bool retirementGranted{};
    bool cancelRequested{};
    bool deadlineActivationQueued{};
};

struct Http3ServerConnection::RejectionEntry final {
    RejectionEntry(Http3ServerConnection& ownerValue, RequestIndexSlot& slotValue,
        std::uint64_t streamIdValue, HttpKnownMethod method, Session::Rejection rejection)
        : slot(slotValue),
          streamId(streamIdValue),
          response(std::in_place, HttpResponse::Options{.resource = ownerValue.worker_.resource()}) {
        response->status(rejectionStatus(rejection));
        response->header("content-length", "0");
        const auto writePlan = planBufferedHttpResponseWrite(method, *response);
        auto created = Http3BufferedResponseOutput::create(*response, writePlan,
            ownerValue.worker_, ownerValue.outbound_,
            {ownerValue.epoch_, ownerValue.connectionGeneration_, streamId}, std::nullopt, slotValue.responsePreludeBytes);
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

struct Http3ServerConnection::EntryRetirement final {
    Http3ServerConnection& owner;
    RequestEntry& entry;

    ~EntryRetirement() {
        if (!entry.outputTerminal) {
            owner.output_.remove(entry.slot);
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

struct Http3ServerConnection::RequestRetirement final {
    Http3ServerConnection& owner;
    std::uint64_t streamId;

    ~RequestRetirement() {
        if (owner.session_.request(streamId) == nullptr) {
            return;
        }
        const auto* slot = owner.requests_.find(streamId);
        if (slot != nullptr && slot->status == RequestStatus::kPublished) {
            // The response is terminal, not the peer's receive direction. Its
            // remaining storage belongs to the input owner until FIN or reset.
            return;
        }
        (void)owner.session_.release(streamId);
        owner.requireConnectionClose(TransportCloseReason::kRequestRetirementFailure,
            Http3ConnectionErrorCode::kInternalError);
    }
};

Http3ServerConnection::Http3ServerConnection(
    const RouteTable& routes, WorkerMemory& worker, ContextServices services,
    const HttpServerOptions& options, http3_stream_buffer& outbound,
    ActivationRef activation, Http3ServerConnectionConfig config)
    : services_(services.withRoutes(routes)),
      worker_(worker),
      routes_(routes),
      options_(options),
      outbound_(outbound),
      activation_(activation),
      epoch_(config.epoch),
      connectionGeneration_(config.connectionGeneration),
      maxTrackedStreams_(config.maxTrackedStreams),
      connectionScanner_(config.connectionScanner),
      executor_(config.executor),
      session_(routes_, worker_, config.session),
      input_(session_, worker_, epoch_, connectionGeneration_, maxTrackedStreams_),
      tasks_(services_.worker(), {.resource = worker_.resource()}),
      requests_(worker_.resource(), maxTrackedStreams_),
      retirement_(requests_, maxTrackedStreams_, epoch_, connectionGeneration_) {
    datagramOutput_ = config.datagramOutput;
    initialize();
}

Http3ServerConnection::Http3ServerConnection(
    const RouteTable& routes, WorkerMemory& worker, ContextServices services,
    const HttpServerOptions& options, http3_stream_buffer& outbound,
    ActivationRef activation, Http3ServerBodyBudget& bodyBudget,
    Http3ServerConnectionConfig config)
    : services_(services.withRoutes(routes)),
      worker_(worker),
      routes_(routes),
      options_(options),
      outbound_(outbound),
      activation_(activation),
      epoch_(config.epoch),
      connectionGeneration_(config.connectionGeneration),
      maxTrackedStreams_(config.maxTrackedStreams),
      connectionScanner_(config.connectionScanner),
      executor_(config.executor),
      session_(routes_, worker_, bodyBudget, config.session),
      input_(session_, worker_, epoch_, connectionGeneration_, maxTrackedStreams_),
      tasks_(services_.worker(), {.resource = worker_.resource()}),
      requests_(worker_.resource(), maxTrackedStreams_),
      retirement_(requests_, maxTrackedStreams_, epoch_, connectionGeneration_) {
    datagramOutput_ = config.datagramOutput;
    initialize();
}

void Http3ServerConnection::receiveDatagram(std::span<const std::byte> bytes) noexcept {
    if (!onWorker() || stopRequested_) {
        return;
    }
    const auto code = static_cast<Http3ConnectionErrorCode>(kHttp3DatagramErrorCode);
    auto decoded = decodeHttp3Datagram(std::span(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!decoded) {
        requireConnectionClose(TransportCloseReason::kConnectionProtocolError, code);
        return;
    }
    try {
        const auto status = session_.receiveDatagram(*decoded);
        auto* slot = requests_.find(decoded->streamId);
        if (status == Http3DatagramReceiveStatus::kConnectionError) {
            requireConnectionClose(TransportCloseReason::kConnectionProtocolError, code);
        } else if (status == Http3DatagramReceiveStatus::kStreamError && slot) {
            (void)enqueueResetIntent(*slot, code);
            (void)retireRequestInput(slot->streamId);
            if (slot->entry) {
                cancelEntry(*slot->entry, RequestStatus::kCancelled);
            }
        } else if (status == Http3DatagramReceiveStatus::kDeliver && slot && slot->entry) {
            slot->entry->dispatch.notifyTunnelInput();
        }
    } catch (...) {
        requireConnectionClose(TransportCloseReason::kInputCapacityExhausted);
    }
}
void Http3ServerConnection::initialize() {
    session_.bindControlOutputWake(this, [](void* raw) noexcept {
        static_cast<Http3ServerConnection*>(raw)->notifyActivation();
    });
    if (!activation_.valid()) {
        throw std::invalid_argument("HTTP/3 connection requires a typed worker activation");
    }
    session_.observePushCancellation(this, [](void* raw, std::uint64_t pushId) noexcept {
        static_cast<Http3ServerConnection*>(raw)->observePushCancellation(pushId);
    });
}

Http3ServerConnection::~Http3ServerConnection() {
    if (std::as_const(output_).queue(QueueKind::kDataRunnable).head != nullptr || std::as_const(output_).queue(QueueKind::kDataRunnable).tail != nullptr ||
        std::as_const(output_).queue(QueueKind::kControlRunnable).head != nullptr || std::as_const(output_).queue(QueueKind::kControlRunnable).tail != nullptr ||
        std::as_const(output_).queue(QueueKind::kLocalRunnable).head != nullptr || std::as_const(output_).queue(QueueKind::kLocalRunnable).tail != nullptr ||
        std::as_const(output_).queue(QueueKind::kDataBlocked).head != nullptr || std::as_const(output_).queue(QueueKind::kDataBlocked).tail != nullptr ||
        std::as_const(output_).queue(QueueKind::kControlBlocked).head != nullptr || std::as_const(output_).queue(QueueKind::kControlBlocked).tail != nullptr ||
        output_.ready() != 0 || output_.blocked() != 0 || requests_.active() != 0 ||
        requests_.rejections() != 0 || tasks_.size() != 0 || session_.activeStreamCount() != 0 ||
        retirement_.pending() != 0 || pendingPushCount_ != 0 || pendingPushIntentCount_ != 0 ||
        pendingPushHead_ != nullptr || pendingPushTail_ != nullptr || pushIntentHead_ != nullptr || pushIntentTail_ != nullptr ||
        (!retirement_.retired() && !retirement_.taken_over() && !retirement_.handed_off()) ||
        (everSpawned_ && !joinCompleted_)) {
        std::terminate();
    }
}

bool Http3ServerConnection::resumeQpackInput() noexcept {
    if (!onWorker() || admissionClosed_) {
        return false;
    }
    bool progress = false;
    try {
        for (unsigned i = 0; i < 32; ++i) {
            const auto resumed = input_.resumeQpack();
            if (!resumed) {
                break;
            }
            progress = true;
            const auto result = handleInputResult(resumed->streamId, resumed->result, false);
            if (result.connectionCloseRequired) {
                break;
            }
        }
    } catch (...) {
        requireConnectionClose(TransportCloseReason::kConnectionProtocolError, Http3ConnectionErrorCode::kInternalError);
    }
    if (progress) {
        notifyActivation();
    }
    return progress;
}

bool Http3ServerConnection::canAcceptInput(std::uint64_t streamId, std::size_t wireBytes) const noexcept {
    return admissionClosed_ || (input_.canAcceptInput(streamId) && session_.canAcceptInput(streamId, wireBytes));
}

Http3ServerConnection::EventResult Http3ServerConnection::acceptData(
    const http3_stream_buffer::borrowed_block& block) & {
    if (!onWorker()) {
        return {.status = EventStatus::kWrongWorker};
    }
    if (admissionClosed_) {
        return {.status = EventStatus::kAdmissionClosed,
            .input = {Input::Status::kStopped},
            .connectionCloseRequired = retirement_.close_required()};
    }
    const auto streamId = block.id().stream_id;
    auto result = handleInputResult(streamId, input_.acceptData(block), false);
    notifyActivation();
    return result;
}

Http3ServerConnection::EventResult Http3ServerConnection::accept_peer_stream_data(
    http3_stream_id id, std::span<const std::byte> bytes) & {
    if (!onWorker()) {
        return {.status = EventStatus::kWrongWorker};
    }
    if (admissionClosed_) {
        return {.status = EventStatus::kAdmissionClosed,
            .input = {Input::Status::kStopped},
            .connectionCloseRequired = retirement_.close_required()};
    }
    auto result = handleInputResult(id.stream_id, input_.accept_peer_stream_data(id, bytes), false);
    notifyActivation();
    return result;
}

Http3ServerConnection::EventResult Http3ServerConnection::acceptControl(
    const http3_stream_control& control) & {
    if (!onWorker()) {
        return {.status = EventStatus::kWrongWorker};
    }
    if (control.id.push_id) {
        if (control.id.epoch != epoch_ || control.id.connection_generation != connectionGeneration_ ||
            control.kind != http3_stream_control::kind::stream_reset) {
            return {.status = EventStatus::kInputRejected};
        }
        auto* slot = requests_.find(control.id.stream_id);
        if (slot == nullptr || slot->pushId != control.id.push_id) {
            return {.status = EventStatus::kInputRejected};
        }
        slot->pushCancelled = true;
        slot->pushPrefix.reset();
        (void)session_.cancelRequest(slot->streamId);
        if (slot->entry != nullptr) {
            cancelEntry(*slot->entry, RequestStatus::kCancelled);
        }
        return {.status = EventStatus::kStreamCancelled};
    }
    if (control.kind == http3_stream_control::kind::connection_closed &&
        control.id.epoch == epoch_ &&
        control.id.connection_generation == connectionGeneration_ && admissionClosed_) {
        (void)confirmTransportRetired(
            {.epoch = epoch_, .connectionGeneration = connectionGeneration_});
        return {.status = EventStatus::kConnectionClosed,
            .input = {Input::Status::kConnectionClosed}};
    }
    if (admissionClosed_) {
        return {.status = EventStatus::kAdmissionClosed,
            .input = {Input::Status::kStopped},
            .connectionCloseRequired = retirement_.close_required()};
    }

    // Suppress this stream's output before recording RESET in the input tombstone.
    // Identity mismatches must not affect a current request with the same stream ID.
    RequestEntry* resetEntry = nullptr;
    RejectionEntry* resetRejection = nullptr;
    if (control.kind == http3_stream_control::kind::stream_reset &&
        control.id.epoch == epoch_ &&
        control.id.connection_generation == connectionGeneration_) {
        auto* slot = requests_.find(control.id.stream_id);
        if (slot != nullptr && slot->entry != nullptr && !slot->entry->cancelRequested) {
            resetEntry = slot->entry;
            resetEntry->outputEnabled = false;
            resetEntry->cancelRequested = true;
            output_.remove(resetEntry->slot);
        } else if (slot != nullptr && slot->rejectionEntry != nullptr &&
                   !slot->rejectionEntry->outputTerminal) {
            resetRejection = slot->rejectionEntry;
            resetRejection->outputEnabled = false;
            output_.remove(resetRejection->slot);
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
    return handleInputResult(control.id.stream_id, result,
        control.kind == http3_stream_control::kind::stream_reset);
}

Http3ServerConnection::WorkState
Http3ServerConnection::workState() const noexcept {
    if (!onWorker()) {
        return {.wrongWorker = true};
    }
    const bool criticalPending = !admissionClosed_ && (!session_.pendingQpackEncoderOutput().empty() || !session_.pendingQpackDecoderOutput().empty() || !session_.pendingControlOutput().empty());
    return {.runnable = {.data = std::as_const(output_).queue(QueueKind::kDataRunnable).head != nullptr || (criticalPending && !criticalOutputBlocked_),
                .control = std::as_const(output_).queue(QueueKind::kControlRunnable).head != nullptr,
                .local = std::as_const(output_).queue(QueueKind::kLocalRunnable).head != nullptr},
        .blocked = {.data = std::as_const(output_).queue(QueueKind::kDataBlocked).head != nullptr || (criticalPending && criticalOutputBlocked_),
            .control = std::as_const(output_).queue(QueueKind::kControlBlocked).head != nullptr},
        .runnableCount = output_.ready() + (criticalPending && !criticalOutputBlocked_ ? 1 : 0),
        .blockedCount = output_.blocked() + (criticalPending && criticalOutputBlocked_ ? 1 : 0)};
}

std::size_t Http3ServerConnection::reactivateBlocked(WorkLanes lanes) & noexcept {
    if (!onWorker()) {
        return 0;
    }
    std::size_t reactivated = 0;
    const auto reactivate = [this, &reactivated](const IntrusiveQueue& blocked, QueueKind runnable) {
        while (blocked.head != nullptr) {
            auto& slot = *blocked.head;
            output_.remove(slot);
            enqueueRunnable(slot, runnable, false);
            ++reactivated;
        }
    };
    if (lanes.data) {
        if (criticalOutputBlocked_) {
            criticalOutputBlocked_ = false;
            ++reactivated;
        }
        reactivate(std::as_const(output_).queue(QueueKind::kDataBlocked), QueueKind::kDataRunnable);
    }
    if (lanes.control) {
        reactivate(std::as_const(output_).queue(QueueKind::kControlBlocked), QueueKind::kControlRunnable);
    }
    if (reactivated != 0) {
        notifyActivation();
    }
    return reactivated;
}

bool Http3ServerConnection::reactivateBlockedOne(WorkLane lane) & noexcept {
    if (!onWorker()) {
        return false;
    }
    const IntrusiveQueue* blocked = nullptr;
    QueueKind runnable = QueueKind::kNone;
    switch (lane) {
        case WorkLane::kData:
            if (criticalOutputBlocked_) {
                criticalOutputBlocked_ = false;
                notifyActivation();
                return true;
            }
            blocked = &std::as_const(output_).queue(QueueKind::kDataBlocked);
            runnable = QueueKind::kDataRunnable;
            break;
        case WorkLane::kControl:
            blocked = &std::as_const(output_).queue(QueueKind::kControlBlocked);
            runnable = QueueKind::kControlRunnable;
            break;
        case WorkLane::kLocal:
            return false;
    }
    if (blocked->head == nullptr) {
        return false;
    }
    auto& slot = *blocked->head;
    output_.remove(slot);
    enqueueRunnable(slot, runnable, false);
    if (slot.queue != runnable) {
        return false;
    }
    return true;
}

Http3ServerConnection::PublishAttempt
Http3ServerConnection::publishOne(WorkLanes eligibleLanes) & noexcept {
    if (!onWorker()) {
        return {.status = PublishStatus::kWrongWorker};
    }
    const auto encoder = session_.pendingQpackEncoderOutput();
    const auto decoder = session_.pendingQpackDecoderOutput();
    const auto control = session_.pendingControlOutput();
    if (eligibleLanes.data && !admissionClosed_ && !criticalOutputBlocked_ &&
        (!encoder.empty() || !decoder.empty() || !control.empty()) && (preferCriticalOutput_ || std::as_const(output_).queue(QueueKind::kDataRunnable).head == nullptr)) {
        preferCriticalOutput_ = false;
        const std::array pending{encoder, decoder, control};
        auto index = nextCriticalOutput_;
        for (std::size_t attempt = 0; pending[index].empty() && attempt < pending.size(); ++attempt) {
            index = (index + 1) % pending.size();
        }
        nextCriticalOutput_ = (index + 1) % pending.size();
        const auto kind = index == 0 ? ruvia::http3_critical_stream_output::stream_kind::qpack_encoder : index == 1 ? ruvia::http3_critical_stream_output::stream_kind::qpack_decoder
                                                                                                                    : ruvia::http3_critical_stream_output::stream_kind::control;
        const auto bytes = pending[index].first(std::min(http3_stream_buffer::max_block_bytes, pending[index].size()));
        const auto sent = outbound_.try_send_critical({epoch_, connectionGeneration_, kind}, std::as_bytes(bytes));
        Dispatch::PublishResult result;
        switch (sent) {
            case http3_stream_buffer::send_result::sent:
                // The reliable buffer now owns a copy through QUIC acceptance.
                if (!(index == 0 ? session_.consumeQpackEncoderOutput(bytes.size()) : index == 1 ? session_.consumeQpackDecoderOutput(bytes.size())
                                                                                                 : session_.consumeControlOutput(bytes.size()))) {
                    std::terminate();
                }
                result = {.status = Dispatch::PublishStatus::kBytesPublished, .bytesPublished = bytes.size()};
                break;
            case http3_stream_buffer::send_result::full:
            case http3_stream_buffer::send_result::no_block:
                criticalOutputBlocked_ = true;
                result = {.status = Dispatch::PublishStatus::kBackpressured, .blockReason = Dispatch::PublishBlockReason::kData};
                break;
            default:
                requireConnectionClose(TransportCloseReason::kPublishFailure, Http3ConnectionErrorCode::kInternalError);
                result = {.status = Dispatch::PublishStatus::kFailed};
                break;
        }
        notifyActivation();
        return {.status = PublishStatus::kAttempted, .criticalKind = kind, .publication = result};
    }
    auto* slot = output_.select(eligibleLanes);
    if (slot == nullptr) {
        return {};
    }
    preferCriticalOutput_ = true;
    if (slot->interimResponse) {
        return publishInterimResponse(*slot);
    }
    auto* entry = slot->entry;
    auto* rejectionEntry = slot->rejectionEntry;
    if ((entry == nullptr) == (rejectionEntry == nullptr)) {
        std::terminate();
    }

    output_.remove(*slot);
    output_.advance();
    if (rejectionEntry != nullptr) {
        return publishRejection(*slot, *rejectionEntry);
    }
    entry->slot.status = RequestStatus::kPublishing;
    const auto result = entry->dispatch.publishStep();
    switch (result.status) {
        case Dispatch::PublishStatus::kBytesPublished:
        case Dispatch::PublishStatus::kControlPublished:
            if (entry->outputEnabled && !entry->cancelRequested && !stopRequested_) {
                entry->slot.status = RequestStatus::kPublishing;
                if (entry->dispatch.publicationDemand() !=
                    Dispatch::PublicationDemand::kNotReady) {
                    enqueueForDemand(entry->slot, false);
                }
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
            output_.remove(entry->slot);
            entry->outputTerminal = true;
            entry->slot.status = RequestStatus::kFailed;
            entry->publicationFinished.notify();
            requireConnectionClose(TransportCloseReason::kUnexpectedRequestState,
                Http3ConnectionErrorCode::kInternalError);
            break;
        case Dispatch::PublishStatus::kFinPublished:
        case Dispatch::PublishStatus::kComplete:
            output_.remove(entry->slot);
            entry->outputTerminal = true;
            entry->slot.status = RequestStatus::kPublished;
            entry->publicationFinished.notify();
            break;
        case Dispatch::PublishStatus::kCancelled: {
            const auto reason = entry->dispatch.cancellationReason();
            const bool alreadyCancelledByOwner = entry->cancelRequested;
            if (reason == Dispatch::CancellationReason::kDeadline) {
                cancelDeadlineEntry(*entry);
            } else {
                output_.remove(entry->slot);
                entry->outputEnabled = false;
                entry->cancelRequested = true;
                entry->outputTerminal = true;
                entry->slot.status = RequestStatus::kCancelled;
                entry->publicationFinished.notify();
            }
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
            output_.remove(entry->slot);
            (void)enqueueResetIntent(entry->slot);
            entry->outputTerminal = true;
            entry->slot.status = RequestStatus::kFailed;
            entry->publicationFinished.notify();
            break;
        case Dispatch::PublishStatus::kFailed:
        case Dispatch::PublishStatus::kWrongWorker:
            output_.remove(entry->slot);
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

bool Http3ServerConnection::requestStop() & noexcept {
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

Task<void> Http3ServerConnection::join() & {
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
    if (requests_.active() != 0 || requests_.rejections() != 0 ||
        output_.ready() != 0 || output_.blocked() != 0 ||
        session_.activeStreamCount() != 0) {
        std::terminate();
    }
}

Http3ServerConnection::RequestInfo Http3ServerConnection::requestInfo(
    std::uint64_t streamId) const noexcept {
    const auto* slot = requests_.find(streamId);
    if (slot == nullptr) {
        return {};
    }
    return {slot->status, slot->rejection, slot->runStatus};
}

std::size_t Http3ServerConnection::activeRequestCount() const noexcept {
    return requests_.active();
}

std::size_t Http3ServerConnection::activeRejectionCount() const noexcept {
    return requests_.rejections();
}

std::size_t Http3ServerConnection::activeTaskCount() const noexcept {
    return tasks_.size();
}

std::size_t Http3ServerConnection::readyRequestCount() const noexcept {
    return output_.ready();
}

std::size_t Http3ServerConnection::trackedRequestCount() const noexcept {
    return requests_.tracked();
}

std::size_t Http3ServerConnection::activeSessionStreamCount() const noexcept {
    return session_.activeStreamCount();
}

bool Http3ServerConnection::drainReady(
    std::size_t expectedAdmittedRequests) const noexcept {
    if (!onWorker() || admissionClosed_ || stopRequested_ || retirement_.close_required() ||
        input_.stopped() || input_.observedRequestStreamCount() != expectedAdmittedRequests ||
        input_.activeRequestStreamCount() != 0 || requests_.rejections() != 0 ||
        output_.ready() != 0 ||
        output_.blocked() != 0 || std::as_const(output_).queue(QueueKind::kDataRunnable).head != nullptr ||
        std::as_const(output_).queue(QueueKind::kControlRunnable).head != nullptr || std::as_const(output_).queue(QueueKind::kLocalRunnable).head != nullptr ||
        std::as_const(output_).queue(QueueKind::kDataBlocked).head != nullptr || std::as_const(output_).queue(QueueKind::kControlBlocked).head != nullptr ||
        session_.activeStreamCount() != 0 || pendingTransportIntentCount() != 0 ||
        retirement_.close_pending() || retirement_.handed_off()) {
        return false;
    }
    for (const auto& slot : requests_.slots()) {
        if ((slot.entry != nullptr &&
                (!slot.entry->outputTerminal || slot.entry->dispatch.handlerActive())) ||
            slot.rejectionEntry != nullptr || slot.resetIntentPending) {
            return false;
        }
    }
    return true;
}

bool Http3ServerConnection::stopped() const noexcept {
    return admissionClosed_;
}

bool Http3ServerConnection::transportCloseRequired() const noexcept {
    return retirement_.close_required();
}

std::optional<Http3ServerConnection::TransportIntent>
Http3ServerConnection::peekTransportIntent() const noexcept {
    auto selected = retirement_.next_intent();
    if (selected && selected->token.kind == TransportIntentKind::kConnectionClose) {
        return selected;
    }
    if (pushIntentHead_ != nullptr && (!selected || pushIntentHead_->token.sequence < selected->token.sequence)) {
        selected = TransportIntent{.token = pushIntentHead_->token};
    }
    return selected;
}

bool Http3ServerConnection::ackTransportIntent(
    const TransportIntentToken& token, std::optional<PushStreamOpenResult> opened) & noexcept {
    if (!onWorker() || token.id.epoch != epoch_ ||
        token.id.connection_generation != connectionGeneration_) {
        return false;
    }
    if (token.kind == TransportIntentKind::kOpenPushStream) {
        if (!opened || !token.id.push_id ||
            (opened->status == PushStreamOpenResult::Status::kOpened &&
                http3StreamIdType(opened->streamId) != Http3StreamIdType::kServerUnidirectional)) {
            return false;
        }
        for (auto* pending = pendingPushHead_; pending != nullptr; pending = pending->next) {
            if (pending != nullptr && pending->owed && pending->token == token) {
                pending->settle(*opened);
                notifyActivation();
                return true;
            }
        }
        return false;
    }
    if (opened || !retirement_.acknowledge(token)) {
        return false;
    }
    notifyActivation();
    return true;
}

bool Http3ServerConnection::takeOverTransportRetirement(
    TransportRetirementTakeover takeover) & noexcept {
    if (!onWorker() || takeover.epoch != epoch_ ||
        takeover.connectionGeneration != connectionGeneration_ || !stopRequested_ ||
        !retirement_.take_over()) {
        return false;
    }
    // The close intent and queued reset intents remain debts until each exact
    // token is acknowledged, even though the transport owner has retired it.
    notifyActivation();
    return true;
}

bool Http3ServerConnection::confirmTransportRetired(
    TransportRetirementConfirmation confirmation) & noexcept {
    if (!onWorker() || confirmation.epoch != epoch_ ||
        confirmation.connectionGeneration != connectionGeneration_ || !retirement_.confirm()) {
        return false;
    }
    // Preserve the complete intent ledger until network publishes an exact ACK
    // for each token. Retirement changes settlement disposition, not ownership.
    if (!admissionClosed_) {
        stopRequested_ = true;
        stopEntries(false);
    }
    notifyActivation();
    return true;
}

std::size_t Http3ServerConnection::pendingTransportIntentCount() const noexcept {
    return retirement_.pending() + pendingPushIntentCount_;
}

bool Http3ServerConnection::transportRetired() const noexcept {
    return retirement_.retired();
}

Task<void> Http3ServerConnection::runRequest(std::uint64_t streamId) {
    auto* slot = requests_.find(streamId);
    if (slot != nullptr && slot->pushId && (slot->pushCancelled || admissionClosed_)) {
        slot->pushPrefix.reset();
        (void)session_.cancelRequest(streamId);
        slot->status = RequestStatus::kCancelled;
        (void)enqueueResetIntent(*slot);
        co_return;
    }
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
    requests_.attach(*slot, *request);
    slot->status = RequestStatus::kRunning;
    EntryRetirement retirement{*this, *request};
    auto& entry = *request;

    Dispatch::RunStatus runStatus = Dispatch::RunStatus::kFailed;
    try {
        if (slot->pushPrefix) {
            auto prefix = std::move(*slot->pushPrefix);
            slot->pushPrefix.reset();
            co_await entry.dispatch.publishResponseBytes(prefix);
        }
        runStatus = co_await entry.dispatch.runHandler();
        entry.slot.runStatus = runStatus;
    } catch (...) {
        if (entry.cancelRequested || admissionClosed_) {
            runStatus = Dispatch::RunStatus::kCancelled;
            entry.slot.runStatus = runStatus;
        } else {
            output_.remove(entry.slot);
            entry.dispatch.cancel();
            entry.outputTerminal = true;
            entry.slot.status = RequestStatus::kFailed;
            notifyActivation();
            requireConnectionClose(TransportCloseReason::kHandlerFailure,
                Http3ConnectionErrorCode::kInternalError);
            co_return;
        }
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

    if (runStatus == Dispatch::RunStatus::peer_field_section_limit) {
        output_.remove(entry.slot);
        (void)enqueueResetIntent(entry.slot);
        entry.outputTerminal = true;
        entry.slot.status = RequestStatus::kFailed;
        notifyActivation();
        co_return;
    }

    if (runStatus == Dispatch::RunStatus::kTunnelComplete || runStatus == Dispatch::RunStatus::kOutputComplete) {
        if (!entry.outputTerminal || entry.slot.status != RequestStatus::kPublished) {
            entry.outputTerminal = true;
            entry.slot.status = RequestStatus::kFailed;
            requireConnectionClose(TransportCloseReason::kUnexpectedRequestState,
                Http3ConnectionErrorCode::kInternalError);
        }
        co_return;
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
            output_.remove(entry.slot);
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

Task<bool> Http3ServerConnection::pushRequest(std::uint64_t parentStreamId, HttpPushRequestView request) {
    auto* parent = requests_.find(parentStreamId);
    const auto* original = session_.request(parentStreamId);
    const auto maximum = session_.peerMaxPushId();
    if (!onWorker() || admissionClosed_ || parent == nullptr || parent->pushId || parent->entry == nullptr ||
        parent->entry->dispatch.responseAborted() || original == nullptr || !maximum || nextPushId_ > *maximum || nextPushId_ >= session_.maxRememberedPushes()) {
        co_return false;
    }
    if (!httpAsciiEqualsIgnoreCase(original->request().scheme(), request.scheme) ||
        !httpAuthoritiesEqual(BorrowedText(original->request().authority()), BorrowedText(request.authority),
            original->request().scheme() == "https" ? 443 : 80)) {
        throw std::invalid_argument("push request must use its associated request origin");
    }
    if (pendingPushCount_ >= std::min(maxTrackedStreams_, std::size_t{32}) || requests_.tracked() + pendingPushCount_ >= maxTrackedStreams_ ||
        !retirement_.sequence_available()) {
        co_return false;
    }
    const auto pushId = nextPushId_;
    auto promise = session_.preparePushPromise(parentStreamId, pushId, request);
    if (!promise) {
        if (promise.error() == Http3ConnectionErrorCode::kMessageError) {
            throw std::invalid_argument("invalid HTTP/3 push request");
        }
        co_return false;
    }
    ++nextPushId_;
    PendingPush pending(*this,
        {.kind = TransportIntentKind::kOpenPushStream,
            .id = {epoch_, connectionGeneration_, parentStreamId, pushId},
            .sequence = retirement_.allocate_sequence()});
    notifyActivation();
    while (!pending.opened) {
        co_await pending.ready.wait();
    }
    if (pending.opened->status != PushStreamOpenResult::Status::kOpened) {
        co_return false;
    }
    const auto streamId = pending.opened->streamId;
    bool created = false;
    auto* pushed = requests_.find_or_create(streamId, created);
    if (pushed == nullptr || !created) {
        requireConnectionClose(TransportCloseReason::kRequestIndexCapacityExhausted, Http3ConnectionErrorCode::kExcessiveLoad);
        co_return false;
    }
    pushed->pushId = pushId;
    pushed->pushCancelled = pending.cancelled;
    if (admissionClosed_ || pending.cancelled || parent->entry == nullptr || parent->entry->dispatch.responseAborted()) {
        pushed->status = RequestStatus::kCancelled;
        (void)enqueueResetIntent(*pushed);
        co_return false;
    }
    try {
        auto prefix = session_.admitPushStream(streamId, pushId);
        if (!prefix) {
            pushed->status = RequestStatus::kCancelled;
            (void)enqueueResetIntent(*pushed);
            co_return false;
        }
        pushed->pushPrefix.emplace(std::move(*prefix));
        co_await parent->entry->dispatch.publishResponseBytes(*promise);
        if (admissionClosed_ || pushed->pushCancelled) {
            pushed->pushPrefix.reset();
            (void)session_.cancelRequest(streamId);
            pushed->status = RequestStatus::kCancelled;
            (void)enqueueResetIntent(*pushed);
            co_return false;
        }
        const auto admitted = admitFinishedRequest(streamId, {});
        co_return admitted.status == EventStatus::kDispatched;
    } catch (...) {
        pushed->pushPrefix.reset();
        (void)session_.cancelRequest(streamId);
        pushed->status = RequestStatus::kCancelled;
        (void)enqueueResetIntent(*pushed);
        throw;
    }
}

void Http3ServerConnection::observePushCancellation(std::uint64_t pushId) noexcept {
    for (auto* pending = pendingPushHead_; pending != nullptr; pending = pending->next) {
        if (pending != nullptr && pending->token.id.push_id == pushId) {
            pending->cancelled = true;
        }
    }
    for (auto& slot : requests_.slots()) {
        if (slot.occupied && slot.pushId == pushId) {
            slot.pushCancelled = true;
        }
    }
}

void Http3ServerConnection::processPushCancellations() noexcept {
    for (auto& slot : requests_.slots()) {
        if (slot.occupied && slot.pushId && slot.pushCancelled && slot.entry != nullptr && !slot.entry->retirementGranted) {
            (void)session_.cancelRequest(slot.streamId);
            (void)enqueueResetIntent(slot);
            cancelEntry(*slot.entry, RequestStatus::kCancelled);
        }
    }
}

void Http3ServerConnection::retireRequestInput(std::uint64_t streamId) noexcept {
    const auto* slot = requests_.find(streamId);
    if (slot != nullptr && slot->pushId) {
        (void)session_.cancelRequest(streamId);
    } else {
        (void)input_.cancelRequest(streamId);
    }
}

Http3ServerConnection::EventResult
Http3ServerConnection::handleInputResult(std::uint64_t streamId,
    Input::Result result, bool fromControl) {
    processPushCancellations();
    if (result.status == Input::Status::kConnectionClosed) {
        (void)confirmTransportRetired(
            {.epoch = epoch_, .connectionGeneration = connectionGeneration_});
        return {.status = EventStatus::kConnectionClosed,
            .input = result,
            .connectionCloseRequired = false};
    }

    if (auto* slot = requests_.find(streamId); slot != nullptr && slot->entry != nullptr) {
        slot->entry->dispatch.notifyTunnelInput();
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
        auto* slot = requests_.find_or_create(streamId, created);
        if (slot == nullptr) {
            requireConnectionClose(TransportCloseReason::kRequestIndexCapacityExhausted,
                Http3ConnectionErrorCode::kExcessiveLoad);
            return {.status = EventStatus::kRequestIndexFull,
                .input = result,
                .connectionCloseRequired = true};
        }
        if (slot->interimResponse) {
            output_.remove(*slot);
            slot->interimResponse.reset();
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

    if (session_.tunnelInputOverflowed(streamId)) {
        bool created = false;
        auto* slot = requests_.find_or_create(streamId, created);
        if (slot == nullptr) {
            requireConnectionClose(TransportCloseReason::kRequestIndexCapacityExhausted,
                Http3ConnectionErrorCode::kExcessiveLoad);
            return {.status = EventStatus::kRequestIndexFull,
                .input = result,
                .connectionCloseRequired = true};
        }
        if (slot->entry != nullptr) {
            cancelEntry(*slot->entry, RequestStatus::kCancelled);
        } else {
            slot->status = RequestStatus::kCancelled;
        }
        (void)enqueueResetIntent(*slot);
        (void)retireRequestInput(streamId);
        notifyActivation();
        return {.status = EventStatus::kStreamCancelled,
            .input = result};
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
            auto* slot = requests_.find(streamId);
            if (slot != nullptr && slot->interimResponse) {
                output_.remove(*slot);
                slot->interimResponse.reset();
                slot->status = RequestStatus::kCancelled;
            }
            if (slot != nullptr && slot->resetIntentPending) {
                retirement_.observe_peer_reset(*slot);
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
        auto* slot = requests_.find(streamId);
        if (slot != nullptr && slot->interimResponse) {
            output_.remove(*slot);
            slot->interimResponse.reset();
            slot->status = RequestStatus::kCancelled;
        }
        if (slot != nullptr && slot->resetIntentPending) {
            retirement_.observe_peer_reset(*slot);
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

    if (result.status == Input::Status::kFed && session_.request(streamId) != nullptr && session_.rejection(streamId) == Session::Rejection::kNone) {
        if (!queueContinueResponse(streamId)) {
            return {.status = EventStatus::kStreamCancelled, .input = result};
        }
    }

    if (result.status == Input::Status::kFed || result.status == Input::Status::kFinished) {
        const auto* slot = requests_.find(streamId);
        if (slot != nullptr && slot->requestStarted &&
            (slot->entry == nullptr || slot->entry->dispatch.complete())) {
            // A response can finish before peer FIN. Its dispatch has returned
            // the lease; only now may the input owner release receive storage.
            if (result.status == Input::Status::kFinished) {
                (void)session_.release(streamId);
            }
            return {.status = EventStatus::kAccepted, .input = result};
        }
    }

    if ((result.status == Input::Status::kFed ||
            result.status == Input::Status::kFinished) &&
        session_.streamState(streamId) == Session::StreamState::kReady) {
        const auto* request = session_.request(streamId);
        if (request != nullptr && (!request->extendedConnectProtocol().empty() || session_.streamingRequest(streamId))) {
            return admitFinishedRequest(streamId, result);
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

    if (result.status == Input::Status::kFed || result.status == Input::Status::kDeferredQpack || result.status == Input::Status::kDeferredFin ||
        result.status == Input::Status::kIgnoredControl) {
        return {.status = EventStatus::kAccepted,
            .input = result};
    }
    return {.status = EventStatus::kInputRejected,
        .input = result};
}

bool Http3ServerConnection::queueContinueResponse(std::uint64_t streamId) {
    const auto* request = session_.request(streamId);
    if (request == nullptr) {
        return true;
    }
    const auto expectation = request->expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
    if (expectation.sendContinue() == nullptr) {
        return true;
    }
    bool created = false;
    auto* slot = requests_.find_or_create(streamId, created);
    if (slot == nullptr) {
        requireConnectionClose(TransportCloseReason::kRequestIndexCapacityExhausted, Http3ConnectionErrorCode::kExcessiveLoad);
        return false;
    }
    if (slot->responsePreludeBytes != 0) {
        return true;
    }
    auto head = session_.encodeInterimResponseHead(streamId, HttpInterimResponseHead(http_status::kContinue));
    const auto peerLimit = session_.peerMaxFieldSectionSize();
    if (!head || (peerLimit && head->field_section.decodedFieldSectionSize() > *peerLimit)) {
        slot->status = RequestStatus::kCancelled;
        (void)enqueueResetIntent(*slot);
        (void)retireRequestInput(streamId);
        notifyActivation();
        return false;
    }
    std::pmr::vector<char> frame(worker_.resource());
    frame.resize(kHttp3FrameHeaderMaxBytes + head->field_section.fieldSection.size());
    const auto prefix = encodeHttp3FrameHeader(frame, static_cast<std::uint64_t>(Http3FrameType::kHeaders), head->field_section.fieldSection.size());
    if (!prefix) {
        throw std::runtime_error("HTTP/3 continue response framing failed");
    }
    frame.resize(*prefix + head->field_section.fieldSection.size());
    std::copy(head->field_section.fieldSection.begin(), head->field_section.fieldSection.end(), frame.begin() + static_cast<std::ptrdiff_t>(*prefix));
    slot->responsePreludeBytes = frame.size();
    slot->interimResponse.emplace(PendingInterimResponse{std::move(frame)});
    enqueueForDemand(*slot, true);
    return true;
}

Http3ServerConnection::PublishAttempt Http3ServerConnection::publishInterimResponse(RequestIndexSlot& slot) noexcept {
    output_.remove(slot);
    auto& pending = *slot.interimResponse;
    const auto bytes = std::span<const char>(pending.frame).subspan(pending.offset);
    const auto sent = outbound_.try_send({epoch_, connectionGeneration_, slot.streamId}, std::as_bytes(bytes.first(std::min(bytes.size(), http3_stream_buffer::max_block_bytes))));
    Dispatch::PublishResult result;
    switch (sent) {
        case http3_stream_buffer::send_result::sent: {
            const auto count = std::min(bytes.size(), http3_stream_buffer::max_block_bytes);
            pending.offset += count;
            result = {.status = Dispatch::PublishStatus::kBytesPublished, .bytesPublished = count};
            if (pending.offset == pending.frame.size()) {
                slot.interimResponse.reset();
            }
            if (slot.interimResponse || slot.rejectionEntry || (slot.entry && slot.entry->dispatch.publicationDemand() != Dispatch::PublicationDemand::kNotReady)) {
                enqueueForDemand(slot, false);
            }
            break;
        }
        case http3_stream_buffer::send_result::full:
        case http3_stream_buffer::send_result::no_block:
            result = {.status = Dispatch::PublishStatus::kBackpressured, .blockReason = Dispatch::PublishBlockReason::kData};
            enqueueBlocked(slot, result.blockReason);
            break;
        default:
            result = {.status = Dispatch::PublishStatus::kFailed};
            requireConnectionClose(TransportCloseReason::kPublishFailure, Http3ConnectionErrorCode::kInternalError);
            break;
    }
    notifyActivation();
    return {.status = PublishStatus::kAttempted, .streamId = slot.streamId, .publication = result};
}

Http3ServerConnection::EventResult
Http3ServerConnection::startRejection(std::uint64_t streamId,
    Session::Rejection rejection, Input::Result result, bool receiveFinished) {
    const auto* request = session_.request(streamId);
    if (request == nullptr || rejection == Session::Rejection::kNone) {
        return {.status = EventStatus::kSessionNotReady,
            .input = result,
            .rejection = rejection,
            .connectionCloseRequired = true};
    }

    bool created = false;
    auto* slot = requests_.find_or_create(streamId, created);
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
        requests_.attach_rejection(*slot, *entry);
        slot->status = RequestStatus::kRejected;
        slot->rejection = rejection;
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

Http3ServerConnection::EventResult
Http3ServerConnection::retireRejectedReceive(std::uint64_t streamId,
    Input::Result result, bool reset) {
    auto* slot = requests_.find(streamId);
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

Http3ServerConnection::PublishAttempt
Http3ServerConnection::publishRejection(RequestIndexSlot& slot,
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
            publication = {Dispatch::PublishStatus::kBytesPublished, result.bytesAccepted};
            enqueueForDemand(slot, false);
            break;
        case Http3BufferedResponseOutput::Status::kFin:
            publication = {Dispatch::PublishStatus::kFinPublished};
            entry.outputTerminal = true;
            slot.status = RequestStatus::kPublished;
            entry.output.reset();
            entry.response.reset();
            finishRejection(entry);
            break;
        case Http3BufferedResponseOutput::Status::kBackpressured: {
            publication.status = Dispatch::PublishStatus::kBackpressured;
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
            publication = {Dispatch::PublishStatus::kComplete};
            entry.outputTerminal = true;
            slot.status = RequestStatus::kPublished;
            entry.output.reset();
            entry.response.reset();
            finishRejection(entry);
            break;
        case Http3BufferedResponseOutput::Status::kFailed:
            publication = {Dispatch::PublishStatus::kFailed, result.bytesAccepted};
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

void Http3ServerConnection::cancelRejectionOutput(
    RejectionEntry& entry, RequestStatus status) noexcept {
    if (entry.outputTerminal) {
        return;
    }
    output_.remove(entry.slot);
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

void Http3ServerConnection::finishRejection(RejectionEntry& entry) noexcept {
    if (!entry.outputTerminal || !entry.receiveTerminal) {
        return;
    }
    auto& slot = entry.slot;
    if (slot.queue != QueueKind::kNone || slot.rejectionEntry != &entry ||
        requests_.rejections() == 0) {
        std::terminate();
    }
    requests_.retire_rejection(slot, entry);
    std::pmr::polymorphic_allocator<RejectionEntry> allocator(worker_.resource());
    std::destroy_at(&entry);
    allocator.deallocate(&entry, 1);
}

Http3ServerConnection::EventResult
Http3ServerConnection::admitFinishedRequest(
    std::uint64_t streamId, Input::Result result) {
    const auto streamState = session_.streamState(streamId);
    if (streamState != Session::StreamState::kReady) {
        bool created = false;
        auto* slot = requests_.find_or_create(streamId, created);
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
    auto* slot = requests_.find_or_create(streamId, created);
    if (slot == nullptr) {
        requireConnectionClose(TransportCloseReason::kRequestIndexCapacityExhausted,
            Http3ConnectionErrorCode::kExcessiveLoad);
        return {.status = EventStatus::kRequestIndexFull,
            .input = result,
            .connectionCloseRequired = true};
    }
    if (slot->requestStarted || slot->status == RequestStatus::kCancelled || slot->status == RequestStatus::kProtocolError) {
        return slot->entry != nullptr ? EventResult{.status = EventStatus::kAccepted, .input = result}
                                      : EventResult{.status = EventStatus::kInputRejected, .input = result};
    }
    slot->requestStarted = true;
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

void Http3ServerConnection::enqueueRunnable(
    RequestIndexSlot& slot, QueueKind kind, bool notify) noexcept {
    if (kind != QueueKind::kDataRunnable && kind != QueueKind::kControlRunnable &&
        kind != QueueKind::kLocalRunnable) {
        std::terminate();
    }
    const auto* entry = slot.entry;
    const auto* rejection = slot.rejectionEntry;
    if (slot.queue != QueueKind::kNone || stopRequested_ ||
        (!slot.interimResponse && (entry == nullptr) == (rejection == nullptr)) ||
        (entry != nullptr && (!entry->outputEnabled || entry->outputTerminal)) ||
        (rejection != nullptr && (!rejection->outputEnabled || rejection->outputTerminal))) {
        return;
    }
    const bool wasLaneEmpty = output_.enqueue(slot, kind);
    if (notify && wasLaneEmpty) {
        notifyActivation();
    }
}

void Http3ServerConnection::enqueueForDemand(
    RequestIndexSlot& slot, bool notify) noexcept {
    if (slot.interimResponse) {
        if (slot.queue != QueueKind::kDataRunnable) {
            output_.remove(slot);
            enqueueRunnable(slot, QueueKind::kDataRunnable, notify);
        }
        return;
    }
    auto* entry = slot.entry;
    auto* rejection = slot.rejectionEntry;
    if ((entry == nullptr) == (rejection == nullptr)) {
        std::terminate();
    }
    QueueKind desired = QueueKind::kLocalRunnable;
    if (rejection != nullptr) {
        switch (rejection->output->nextStep()) {
            case ruvia::http3_buffered_response_cursor::step::bytes:
                desired = QueueKind::kDataRunnable;
                break;
            case ruvia::http3_buffered_response_cursor::step::fin:
                desired = QueueKind::kControlRunnable;
                break;
            case ruvia::http3_buffered_response_cursor::step::complete:
            case ruvia::http3_buffered_response_cursor::step::failed:
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
            case Dispatch::PublicationDemand::local_buffer_stopped:
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
    output_.remove(slot);
    enqueueRunnable(slot, desired, notify);
}

void Http3ServerConnection::enqueueBlocked(
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
        (!slot.interimResponse && (entry == nullptr) == (rejection == nullptr)) ||
        (entry != nullptr && (!entry->outputEnabled || entry->outputTerminal)) ||
        (rejection != nullptr && (!rejection->outputEnabled || rejection->outputTerminal))) {
        return;
    }
    (void)output_.enqueue(slot, kind);
}

void Http3ServerConnection::publicationStopped(RequestEntry& entry) noexcept {
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
    output_.remove(entry.slot);
    enqueueRunnable(entry.slot, QueueKind::kLocalRunnable, false);
    if (!entry.deadlineActivationQueued && entry.slot.queue == QueueKind::kLocalRunnable) {
        entry.deadlineActivationQueued = true;
        notifyActivation();
    }
}

void Http3ServerConnection::cancelDeadlineEntry(RequestEntry& entry) noexcept {
    if (entry.retirementGranted) {
        return;
    }
    // Fence output before installing the receive tombstone. A published FIN is
    // already accepted by QUIC; the transport owner decides whether its send state
    // still requires a reset, so the worker must not guess by publishing one.
    entry.outputEnabled = false;
    entry.cancelRequested = true;
    output_.remove(entry.slot);
    (void)retireRequestInput(entry.streamId);
    // FIN publication only closes the local send direction. The transport owner
    // still needs a per-stream retirement token to stop receiving if the peer
    // never finishes its direction.
    (void)enqueueResetIntent(entry.slot);
    cancelEntry(entry, RequestStatus::kCancelled);
}

void Http3ServerConnection::cancelEntry(
    RequestEntry& entry, RequestStatus status) noexcept {
    if (entry.retirementGranted) {
        return;
    }
    entry.outputEnabled = false;
    entry.cancelRequested = true;
    output_.remove(entry.slot);
    entry.slot.interimResponse.reset();
    // The input RESET/tombstone or connection stop is established by the caller.
    // outputTerminal only fences publication; it does not retire a still-running
    // request frame. A suspended handler retains its dispatch lease until
    // runHandler() returns; only then may the wrapper consume this permit.
    entry.dispatch.cancel();
    entry.outputTerminal = true;
    entry.retirementGranted = true;
    entry.slot.status = status;
    entry.publicationFinished.notify();
    notifyActivation();
}

void Http3ServerConnection::stopEntries(bool inputAlreadyStopped) noexcept {
    admissionClosed_ = true;
    criticalOutputBlocked_ = false;

    // First quiesce every output so cancellation of one handler cannot let a
    // sibling publish while the connection is being retired.
    for (auto& slot : requests_.slots()) {
        if (slot.interimResponse) {
            output_.remove(slot);
            slot.interimResponse.reset();
        }
        if (auto* rejection = slot.rejectionEntry;
            rejection != nullptr && !rejection->outputTerminal) {
            rejection->outputEnabled = false;
            output_.remove(slot);
        }
        auto* entry = slot.entry;
        if (entry == nullptr || entry->retirementGranted) {
            continue;
        }
        entry->outputEnabled = false;
        entry->cancelRequested = true;
        output_.remove(slot);
    }
    if (!inputAlreadyStopped) {
        input_.stop();
    }

    for (auto& slot : requests_.slots()) {
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
        if (entry == nullptr || entry->retirementGranted) {
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

void Http3ServerConnection::notifyActivation() noexcept {
    if (!onWorker() || !activation_.valid()) {
        std::terminate();
    }
    const auto activation = activationSnapshot();
    activation_.activate(activation_.context, epoch_, connectionGeneration_,
        activation_.slotGeneration, activation);
}

Http3ServerConnection::WorkerActivation
Http3ServerConnection::activationSnapshot() const noexcept {
    const auto intent = peekTransportIntent();
    return {.work = workState(),
        .transportIntent = intent ? std::optional<TransportIntentToken>(intent->token) : std::nullopt};
}

bool Http3ServerConnection::detachActivationAfterJoin() noexcept {
    if (!onWorker() || !joinCompleted_ || !admissionClosed_ || requests_.active() != 0 ||
        requests_.rejections() != 0 || tasks_.size() != 0 || pendingTransportIntentCount() != 0 ||
        (!retirement_.retired() && !retirement_.taken_over() && !retirement_.handed_off())) {
        return false;
    }
    activation_ = {};
    return true;
}

bool Http3ServerConnection::enqueueResetIntent(RequestIndexSlot& slot, Http3ConnectionErrorCode errorCode, ResetIntentOrigin origin) noexcept {
    switch (retirement_.enqueue_reset(slot, errorCode, origin)) {
        case transport_retirement::reset_result::queued:
            return true;
        case transport_retirement::reset_result::unavailable:
            return false;
        case transport_retirement::reset_result::capacity_exhausted:
            requireConnectionClose(TransportCloseReason::kTransportIntentCapacityExhausted, Http3ConnectionErrorCode::kInternalError);
            return false;
        case transport_retirement::reset_result::sequence_exhausted:
            requireConnectionClose(TransportCloseReason::kTransportIntentSequenceExhausted, Http3ConnectionErrorCode::kInternalError);
            return false;
    }
    std::terminate();
}

void Http3ServerConnection::requireConnectionClose(
    TransportCloseReason reason, std::optional<Http3ConnectionErrorCode> errorCode) noexcept {
    retirement_.require_close(reason, errorCode);
    stopRequested_ = true;
    if (!admissionClosed_) {
        stopEntries(false);
    }
}

void Http3ServerConnection::finishEntry(RequestEntry& entry) noexcept {
    if (entry.slot.queue != QueueKind::kNone || entry.dispatch.handlerActive() ||
        !entry.outputTerminal || entry.slot.entry != &entry || requests_.active() == 0) {
        std::terminate();
    }
    requests_.retire(entry.slot, entry);
}

bool Http3ServerConnection::onWorker() const noexcept {
    return services_.worker().isCurrent();
}

bool Http3ServerConnection::attachTunnelScanner(
    std::uint64_t streamId, ConnectionScanner::Entry& entry) noexcept {
    if (!onWorker() || connectionScanner_ == nullptr) {
        return false;
    }
    auto* slot = requests_.find(streamId);
    if (slot == nullptr || slot->entry == nullptr ||
        &slot->entry->scannerEntry != &entry) {
        return false;
    }
    if (slot->entry->scannerRegistered) {
        return true;
    }
    connectionScanner_->registerEntry(entry);
    slot->entry->scannerRegistered = true;
    return true;
}

void Http3ServerConnection::tunnelOutputReady(std::uint64_t streamId) noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    auto* slot = requests_.find(streamId);
    if (slot == nullptr || slot->entry == nullptr || slot->entry->outputTerminal ||
        slot->entry->cancelRequested || !slot->entry->outputEnabled) {
        return;
    }
    enqueueForDemand(*slot, true);
}

void Http3ServerConnection::abortTunnel(std::uint64_t streamId) noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    auto* slot = requests_.find(streamId);
    if (slot == nullptr || slot->entry == nullptr || slot->entry->retirementGranted ||
        slot->entry->cancelRequested) {
        return;
    }
    auto& entry = *slot->entry;
    // Match deadline cancellation: fence publication, tombstone input, then
    // retain a stream retirement intent even if output FIN was already accepted.
    entry.outputEnabled = false;
    entry.cancelRequested = true;
    output_.remove(entry.slot);
    (void)retireRequestInput(streamId);
    // Preserve a retirement intent after local FIN; the transport owner chooses
    // STOP_SENDING versus RESET_STREAM from the actual transport send state.
    (void)enqueueResetIntent(*slot);
    cancelEntry(entry, RequestStatus::kCancelled);
}

bool Http3ServerConnection::attachTunnelScannerThunk(
    void* context, std::uint64_t streamId, ConnectionScanner::Entry& entry) noexcept {
    return static_cast<Http3ServerConnection*>(context)->attachTunnelScanner(streamId, entry);
}

void Http3ServerConnection::requestInputConsumedThunk(void* context) noexcept {
    auto& owner = *static_cast<Http3ServerConnection*>(context);
    auto activation = owner.activationSnapshot();
    activation.inputCapacityAvailable = true;
    owner.activation_.activate(owner.activation_.context, owner.epoch_, owner.connectionGeneration_, owner.activation_.slotGeneration, activation);
}

void Http3ServerConnection::tunnelOutputReadyThunk(
    void* context, std::uint64_t streamId) noexcept {
    static_cast<Http3ServerConnection*>(context)->tunnelOutputReady(streamId);
}

void Http3ServerConnection::abortTunnelThunk(
    void* context, std::uint64_t streamId) noexcept {
    static_cast<Http3ServerConnection*>(context)->abortTunnel(streamId);
}

std::size_t Http3ServerConnection::indexCapacity(std::size_t maxTrackedStreams) {
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
